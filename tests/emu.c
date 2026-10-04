/*
 * Register-level SMHC + SD card model and kernel/sdport mock functions.
 * See emu.h for the caveats.
 */

#include <assert.h>
#include <stdlib.h>
#include <wchar.h>
#include "emu.h"

EMU emu;
uint8_t emu_arena[EMU_ARENA_SIZE];
uint8_t emu_card[EMU_CARD_SIZE];
int g_mock_log_level = -1;
int g_mock_irql = PASSIVE_LEVEL;

#define R(off) (emu.regs[(off) / 4])

static void run_idmac_walk(int write);

/* -------------------------------------------------------------- helpers */

void *emu_phys_to_ptr(uint32_t phys)
{
    assert(phys >= EMU_PHYS_BASE && phys < EMU_PHYS_BASE + EMU_ARENA_SIZE);
    return emu_arena + (phys - EMU_PHYS_BASE);
}

uint32_t emu_ptr_to_phys(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    assert(b >= emu_arena && b < emu_arena + EMU_ARENA_SIZE);
    return EMU_PHYS_BASE + (uint32_t)(b - emu_arena);
}

void emu_fill_card(void)
{
    uint32_t i;
    for (i = 0; i < EMU_CARD_SIZE; i++) {
        emu_card[i] = (uint8_t)((i * 131u + (i >> 8) * 7u + 13u) & 0xFF);
    }
}

void emu_init(void)
{
    memset(&emu, 0, sizeof(emu));
    emu.inject_data_crc_after_words = -1;
    emu.reset_clears_regs = 1;
    emu.stop_imask = 0xFFFFFFFFu;
    emu.busy_polls_after_write = 5;
    /* firmware leftovers: 24 MHz module clock, clock running, 4-bit */
    R(SMHC_REG_CLKCR) = SMHC_CLKCR_CARD_CLOCK_ON;
    R(SMHC_REG_WIDTH) = SMHC_WIDTH_4BIT;
}

static int ftrgl_rx_tl(void) { return (int)((R(SMHC_REG_FTRGL) >> SMHC_FTRGL_RX_TL_SHIFT) & 0xFFF); }
static int ftrgl_tx_tl(void) { return (int)(R(SMHC_REG_FTRGL) & 0xFFF); }

/* ----------------------------------------------------------------- FIFO */

static void fifo_push(uint32_t w)
{
    assert(emu.fifo_level < EMU_FIFO_DEPTH);
    emu.fifo[(emu.fifo_head + emu.fifo_level) % EMU_FIFO_DEPTH] = w;
    emu.fifo_level++;
}

static uint32_t fifo_pop(void)
{
    uint32_t w;
    if (emu.fifo_level == 0) {
        return 0xDEADBEEF;      /* underrun */
    }
    w = emu.fifo[emu.fifo_head];
    emu.fifo_head = (emu.fifo_head + 1) % EMU_FIFO_DEPTH;
    emu.fifo_level--;
    return w;
}

static void data_over(void)
{
    if (emu.data_over_sent) return;
    emu.data_over_sent = 1;
    R(SMHC_REG_RINT) |= SMHC_INT_DATA_OVER;
    if (emu.xfer_auto_stop) {
        emu.auto_pending = 2;       /* the automatic CMD12 still has to run */
    }
}

/*
 * Advance the card/FIFO side.  Called on every register access that could
 * observe progress.  Data-request bits are level derived (re-asserted after
 * a clear while the condition holds), as dwcmshc notes for the DW core.
 * The model does NOT raise RXDR for a tail below the watermark (pessimistic).
 */
void emu_pump(void)
{
    if (emu.rd_active) {
        while (emu.fifo_level < EMU_FIFO_DEPTH && emu.xfer_done < emu.xfer_words) {
            if (emu.crc_inject_at >= 0 && (int)emu.xfer_done >= emu.crc_inject_at) {
                R(SMHC_REG_RINT) |= emu.inject_data_err_bits ? emu.inject_data_err_bits : SMHC_INT_DATA_CRC_ERROR;
                emu.rd_active = 0;
                emu.crc_inject_at = -1;
                return;
            }
            uint32_t w;
            memcpy(&w, &emu_card[emu.xfer_card_off + emu.xfer_done * 4], 4);
            fifo_push(w);
            emu.xfer_done++;
        }
        if (emu.xfer_done == emu.xfer_words) {
            data_over();
        }
        {
            int cond = emu.fifo_level > ftrgl_rx_tl();
            if (cond && (!emu.dreq_edge_only || !emu.rx_cond_prev)) {
                R(SMHC_REG_RINT) |= SMHC_INT_RX_DATA_REQUEST;
            }
            emu.rx_cond_prev = cond;
        }
        if (emu.xfer_done == emu.xfer_words && emu.fifo_level == 0) {
            emu.rd_active = 0;              /* everything delivered and drained */
        }
    }

    if (emu.wr_active) {
        /*
         * The card drains the FIFO only in emu_card_tick(), i.e. while the CPU is
         * not busy writing, so the FIFO can really fill up and the TX watermark
         * can be crossed (needed to exercise edge-triggered data requests).
         */
        int cond = emu.fifo_level <= ftrgl_tx_tl();
        if (emu.xfer_done + (uint32_t)emu.fifo_level < emu.xfer_words || emu.fifo_level > 0) {
            if (cond && (!emu.dreq_edge_only || !emu.tx_cond_prev) &&
                emu.xfer_done + (uint32_t)emu.fifo_level < emu.xfer_words) {
                R(SMHC_REG_RINT) |= SMHC_INT_TX_DATA_REQUEST;
            }
        }
        emu.tx_cond_prev = cond;
    }
}

/* One slice of card activity: swallow up to 16 words from the FIFO. */
void emu_card_tick(void)
{
    int n = 0;

    if (emu.auto_pending > 0 && --emu.auto_pending == 0) {
        R(SMHC_REG_RINT) |= SMHC_INT_AUTO_COMMAND_DONE;
    }

    if (emu.dma_pending > 0 && --emu.dma_pending == 0) {
        if (R(SMHC_REG_DMAC) & SMHC_DMAC_IDMA_ON) {
            run_idmac_walk(0);
        }                                   /* else: torn down early, the data is lost */
    }

    if (!emu.wr_active) {
        return;
    }

    while (emu.fifo_level > 0 && n < 16) {
        uint32_t w = fifo_pop();
        memcpy(&emu_card[emu.xfer_card_off + emu.xfer_done * 4], &w, 4);
        emu.xfer_done++;
        n++;
    }

    if (emu.xfer_done == emu.xfer_words) {
        data_over();
        emu.busy_left = emu.busy_polls_after_write;
        emu.wr_active = 0;
    }

    emu_pump();
}

/* ------------------------------------------------------------------ DMA */

static void run_idmac_walk(int write)
{
    uint32_t desc_phys = R(SMHC_REG_DLBA) << SMHC_IDMAC_DES_SHIFT;
    uint32_t off = 0;
    int first = 1;

    for (;;) {
        SMHC_IDMAC_DESCRIPTOR *d = (SMHC_IDMAC_DESCRIPTOR *)emu_phys_to_ptr(desc_phys);
        uint32_t len = d->BufSize ? d->BufSize : SMHC_IDMAC_MAX_LEN_PER_DESC;
        uint32_t addr = d->BufAddr << SMHC_IDMAC_DES_SHIFT;

        assert(d->Config & SMHC_IDMAC_DES_OWN);
        assert(d->Config & SMHC_IDMAC_DES_CH);
        assert(!first || (d->Config & SMHC_IDMAC_DES_FD));
        assert(first || !(d->Config & SMHC_IDMAC_DES_FD));
        first = 0;

        if (write) {
            memcpy(&emu_card[emu.xfer_card_off + off], emu_phys_to_ptr(addr), len);
        } else {
            memcpy(emu_phys_to_ptr(addr), &emu_card[emu.xfer_card_off + off], len);
        }
        off += len;
        d->Config &= ~SMHC_IDMAC_DES_OWN;

        if (d->Config & SMHC_IDMAC_DES_LD) {
            assert(d->Config & SMHC_IDMAC_DES_ER);
            assert(!(d->Config & SMHC_IDMAC_DES_DIC));
            break;
        }
        assert(d->Config & SMHC_IDMAC_DES_DIC);
        desc_phys = d->NextDesc << SMHC_IDMAC_DES_SHIFT;
        assert(d->NextDesc != 0);
    }

    assert(off == emu.xfer_words * 4);
    R(SMHC_REG_IDST) |= write ? SMHC_IDST_TX_INTERRUPT : SMHC_IDST_RX_INTERRUPT;
}

/*
 * Writes are fetched from memory at once.  Reads model the IDMAC lagging behind the
 * card: DATA_OVER is raised when the card side is done, the FIFO -> memory copy and
 * the RX interrupt (IDST.RI) come a few ticks later (Linux waits for RI: wait_dma).
 * A driver that completes at DATA_OVER sees stale memory, and one that tears the
 * IDMAC down early aborts the copy.
 */
static void run_idmac(int write)
{
    if (write) {
        run_idmac_walk(1);
    } else {
        emu.dma_pending = 3;
    }
    data_over();
}

/* ------------------------------------------------------------- commands */

static void exec_command(uint32_t cmd, uint32_t arg)
{
    uint32_t idx = cmd & SMHC_CMD_INDEX_MASK;
    int has_data = (cmd & SMHC_CMD_DATA_EXPIRE) != 0;
    int write = (cmd & SMHC_CMD_WRITE) != 0;

    assert((R(SMHC_REG_CMD) & SMHC_CMD_START) == 0);

    if (emu.log_count < EMU_MAX_LOG) {
        emu.log[emu.log_count].index = idx;
        emu.log[emu.log_count].arg = arg;
        emu.log[emu.log_count].cmdreg = cmd;
        emu.log_count++;
    }

    if ((cmd & SMHC_CMD_STOP_ABORT) && idx == 12) {
        emu.stop_cmds++;
        emu.stop_imask = R(SMHC_REG_IMASK);
        /* abort any running transfer */
        emu.rd_active = emu.wr_active = 0;
    }

    if (emu.inject_resp_timeout) {
        emu.inject_resp_timeout = 0;
        R(SMHC_REG_RINT) |= SMHC_INT_RESP_TIMEOUT;
        emu.late_cmd_done = 1;          /* Linux: COMMAND_DONE still follows */
        return;
    }

    R(SMHC_REG_RINT) |= SMHC_INT_COMMAND_DONE;

    if (cmd & SMHC_CMD_RESP_EXPIRE) {
        if (cmd & SMHC_CMD_LONG_RESPONSE) {
            /* 128-bit response with a recognisable pattern; low byte = CRC7|end */
            R(SMHC_REG_RESP3) = 0x00112233;
            R(SMHC_REG_RESP2) = 0x44556677;
            R(SMHC_REG_RESP1) = 0x8899AABB;
            R(SMHC_REG_RESP0) = 0xCCDDEEFF;
        } else {
            R(SMHC_REG_RESP0) = 0x00000900 | idx;
        }
    }

    if (!has_data) {
        return;
    }

    emu.xfer_words = R(SMHC_REG_BCNTR) / 4;
    emu.xfer_done = 0;
    emu.xfer_card_off = arg * 512u;
    emu.xfer_auto_stop = (cmd & SMHC_CMD_AUTO_STOP) != 0;
    emu.data_over_sent = 0;
    emu.crc_inject_at = emu.inject_data_crc_after_words;
    emu.inject_data_crc_after_words = -1;

    {
        uint32_t g = R(SMHC_REG_GCTRL);
        int dma = (g & SMHC_GCTRL_DMA_ENABLE) && !(g & SMHC_GCTRL_ACCESS_BY_AHB);
        int pio = (g & SMHC_GCTRL_ACCESS_BY_AHB) != 0;
        assert(dma != pio);                     /* exactly one data path must be selected */

        if (dma) {
            assert(R(SMHC_REG_DMAC) & SMHC_DMAC_IDMA_ON);
            run_idmac(write);
        } else if (write) {
            emu.wr_active = 1;
        } else {
            emu.rd_active = 1;
        }
    }
    emu_pump();
}

/* --------------------------------------------------------- register I/O */

int emu_irq_line(void)
{
    if (!(R(SMHC_REG_GCTRL) & SMHC_GCTRL_INT_ENABLE)) return 0;
    return (R(SMHC_REG_RINT) & R(SMHC_REG_IMASK)) != 0 ||
           (R(SMHC_REG_IDST) & R(SMHC_REG_IDIE)) != 0;
}

static uint32_t emu_status(void)
{
    uint32_t s = 0;
    emu_pump();
    if (emu.fifo_level == 0) s |= SMHC_STATUS_FIFO_EMPTY;
    if (emu.fifo_level >= EMU_FIFO_DEPTH) s |= SMHC_STATUS_FIFO_FULL;
    /* A completely full FIFO reports level 0 (u-boot: "Some SoCs (A20)") */
    if (emu.fifo_level < EMU_FIFO_DEPTH) {
        s |= ((uint32_t)emu.fifo_level & SMHC_STATUS_FIFO_LEVEL_MASK) << SMHC_STATUS_FIFO_LEVEL_SHIFT;
    }
    if (emu.busy_left > 0) {
        s |= SMHC_STATUS_CARD_DATA_BUSY;
        emu.busy_left--;
    }
    return s;
}

ULONG SdPortReadRegisterUlong(PVOID base, ULONG reg)
{
    (void)base;
    assert(reg < 0x300 && (reg & 3) == 0);
    switch (reg) {
    case SMHC_REG_MINT:
        return R(SMHC_REG_RINT) & R(SMHC_REG_IMASK);
    case SMHC_REG_STATUS:
        return emu_status();
    case SMHC_REG_FIFO: {
        uint32_t w;
        emu_pump();
        w = fifo_pop();
        emu_pump();
        return w;
    }
    default:
        return R(reg);
    }
}

VOID SdPortWriteRegisterUlong(PVOID base, ULONG reg, ULONG v)
{
    (void)base;
    assert(reg < 0x300 && (reg & 3) == 0);

    switch (reg) {
    case SMHC_REG_GCTRL: {
        uint32_t keep = v & ~SMHC_GCTRL_ALL_RESET;
        if (v & SMHC_GCTRL_FIFO_RESET) {
            emu.fifo_resets++;
            if (emu.rd_active || emu.wr_active) {
                emu.fifo_reset_during_xfer = 1;
            }
            emu.fifo_head = emu.fifo_level = 0;
        }
        if (v & SMHC_GCTRL_SOFT_RESET) {
            emu.rd_active = emu.wr_active = 0;
            emu.fifo_head = emu.fifo_level = 0;
            if (emu.reset_clears_regs) {
                R(SMHC_REG_CLKCR) = 0;
                R(SMHC_REG_WIDTH) = 0;
                R(SMHC_REG_IMASK) = 0;
                R(SMHC_REG_NTSR) = 0;
                R(SMHC_REG_SAMP_DL) = 0;
                R(SMHC_REG_FTRGL) = 0;
                R(SMHC_REG_THLDC) = 0;
                keep &= ~(SMHC_GCTRL_INT_ENABLE | SMHC_GCTRL_DMA_ENABLE | SMHC_GCTRL_ACCESS_BY_AHB);
            }
        }
        R(SMHC_REG_GCTRL) = keep;          /* reset bits self-clear */
        return;
    }
    case SMHC_REG_RINT:
        R(SMHC_REG_RINT) &= ~v;
        emu_pump();                         /* level-derived bits re-assert */
        return;
    case SMHC_REG_IDST:
        R(SMHC_REG_IDST) &= ~v;
        return;
    case SMHC_REG_CMD:
        if (!(v & SMHC_CMD_START)) {
            R(SMHC_REG_CMD) = v;
            return;
        }
        if (v & SMHC_CMD_UPCLK_ONLY) {
            R(SMHC_REG_CMD) = v & ~SMHC_CMD_START;
            R(SMHC_REG_RINT) |= 0;          /* [UB-C]: clock update may set status bits; none modelled */
            return;
        }
        R(SMHC_REG_CMD) = v & ~SMHC_CMD_START;     /* controller accepts it instantly */
        exec_command(v, R(SMHC_REG_ARG));
        return;
    case SMHC_REG_FIFO:
        emu_pump();
        assert(emu.wr_active);
        assert(emu.fifo_level < EMU_FIFO_DEPTH);
        fifo_push(v);
        emu_pump();
        return;
    case SMHC_REG_DMAC:
        R(SMHC_REG_DMAC) = v & ~SMHC_DMAC_SOFT_RESET;
        return;
    default:
        R(reg) = v;
        return;
    }
}

VOID SdPortWait(ULONG us)
{
    (void)us;
    if (emu.late_cmd_done) {
        /* the late COMMAND_DONE arrives only when interrupts are serviced (see harness) */
    }
}

/* ------------------------------------------------------------ misc mocks */

static struct { PIO_WORKITEM_ROUTINE routine; PVOID ctx; int queued; } g_wi;

PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT dev)
{
    (void)dev;
    return (PIO_WORKITEM)calloc(1, sizeof(IO_WORKITEM));
}

void IoFreeWorkItem(PIO_WORKITEM w) { free(w); }

void IoQueueWorkItem(PIO_WORKITEM w, PIO_WORKITEM_ROUTINE r, int type, PVOID ctx)
{
    (void)w; (void)type;
    g_wi.routine = r; g_wi.ctx = ctx; g_wi.queued = 1;
}

int emu_workitems_pending(void) { return g_wi.queued; }

void emu_run_workitems(void)
{
    if (g_wi.queued) {
        int saved = g_mock_irql;
        g_wi.queued = 0;
        g_mock_irql = PASSIVE_LEVEL;
        g_wi.routine(NULL, g_wi.ctx);
        g_mock_irql = saved;
    }
}

#define MAX_REGVALS 8
static struct { char name[32]; uint32_t value; } g_regvals[MAX_REGVALS];
static int g_nregvals;

void emu_set_reg_value(const char *name, uint32_t value)
{
    strncpy(g_regvals[g_nregvals].name, name, 31);
    g_regvals[g_nregvals].value = value;
    g_nregvals++;
}

void emu_clear_reg_values(void) { g_nregvals = 0; }

NTSTATUS IoOpenDeviceRegistryKey(PDEVICE_OBJECT pdo, ULONG type, ULONG access, HANDLE *key)
{
    (void)pdo; (void)type; (void)access;
    *key = (HANDLE)1;
    return STATUS_SUCCESS;
}

NTSTATUS ZwQueryValueKey(HANDLE key, PUNICODE_STRING name, KEY_VALUE_INFORMATION_CLASS c,
                         PVOID buf, ULONG len, PULONG result)
{
    int i;
    (void)key; (void)c; (void)len; (void)result;
    for (i = 0; i < g_nregvals; i++) {
        char n[32];
        size_t k;
        for (k = 0; name->Buffer[k] && k < 31; k++) n[k] = (char)name->Buffer[k];
        n[k] = 0;
        if (strcmp(n, g_regvals[i].name) == 0) {
            PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
            info->Type = REG_DWORD;
            info->DataLength = 4;
            memcpy(info->Data, &g_regvals[i].value, 4);
            return STATUS_SUCCESS;
        }
    }
    return 0xC0000034; /* STATUS_OBJECT_NAME_NOT_FOUND */
}
