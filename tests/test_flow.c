/*
 * Flow tests: the real driver callbacks (driver/smhc.c, driver/smhc_hw.c) driven
 * by a small sdport simulator against the SMHC/SD-card model in emu.c.
 *
 * The simulator reproduces the request choreography of Microsoft's sample:
 *   IssueRequest(command) -> ISR/DPC until the request completes
 *   -> for PIO: IssueRequest(StartTransfer) repeatedly while the driver answers
 *      STATUS_MORE_PROCESSING_REQUIRED
 *   -> for SG DMA: one IssueRequest(StartTransfer), which completes at once.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "emu.h"
#include "../driver/smhc.h"

NTSTATUS DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING);

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

/* ------------------------------------------------------- sdport simulator */

static SDPORT_INITIALIZATION_DATA g_init;
static SD_MINIPORT *g_mp;
static SDPORT_SLOT_EXTENSION *g_slot;
static PVOID g_ext;
static DEVICE_OBJECT g_fdo, g_pdo;
static DEVOBJ_EXTENSION g_fdo_ext;

static int g_completed;
static NTSTATUS g_completed_status;
static int g_complete_calls;

NTSTATUS SdPortInitialize(PVOID driver, PVOID regpath, PSDPORT_INITIALIZATION_DATA init)
{
    (void)driver; (void)regpath;
    g_init = *init;
    return STATUS_SUCCESS;
}

VOID SdPortCompleteRequest(PSDPORT_REQUEST request, NTSTATUS status)
{
    (void)request;
    g_completed = 1;
    g_completed_status = status;
    g_complete_calls++;
}

#define ALL_EVENTS (SDPORT_EVENT_CARD_RESPONSE | SDPORT_EVENT_CARD_RW_END | SDPORT_EVENT_BUFFER_EMPTY | \
                    SDPORT_EVENT_BUFFER_FULL | SDPORT_EVENT_ERROR | SDPORT_EVENT_DMA_COMPLETE)

/* Service interrupts until the current request completes (or nothing is left to do). */
static NTSTATUS port_wait(SDPORT_REQUEST *req)
{
    int i;
    for (i = 0; i < 100000 && !g_completed; i++) {
        emu_pump();
        emu_card_tick();
        if (emu_irq_line()) {
            ULONG ev, er;
            BOOLEAN cc, sdio, tun;
            BOOLEAN handled = g_init.Interrupt(g_ext, &ev, &er, &cc, &sdio, &tun);
            if (handled) {
                int saved = g_mock_irql;
                g_mock_irql = DISPATCH_LEVEL;
                g_init.RequestDpc(g_ext, req, ev, er);
                g_mock_irql = saved;
            }
            continue;
        }
        if (emu.late_cmd_done) {
            emu.late_cmd_done = 0;
            emu.regs[SMHC_REG_RINT / 4] |= SMHC_INT_COMMAND_DONE;
            continue;
        }
        if (emu_workitems_pending()) {
            emu_run_workitems();
            continue;
        }
        if (emu.wr_active || emu.auto_pending || emu.dma_pending) {
            continue;                       /* card still draining / auto CMD12 running */
        }
        break;
    }
    return g_completed ? g_completed_status : STATUS_IO_TIMEOUT;
}

static NTSTATUS port_issue_once(SDPORT_REQUEST *req)
{
    NTSTATUS st;
    int saved = g_mock_irql;

    g_completed = 0;
    g_mock_irql = DISPATCH_LEVEL;
    st = g_init.IssueRequest(g_ext, req);
    g_mock_irql = saved;

    if (st != STATUS_PENDING && st != STATUS_SUCCESS) {
        return st;                          /* rejected before reaching the bus */
    }
    return port_wait(req);
}

static int has_data(SDPORT_COMMAND *c)
{
    return c->TransferType != SdTransferTypeNone && c->TransferType != SdTransferTypeUndefined;
}

/* Run a full request the way sdport would, including the data phases. */
static uint32_t g_between_phase_err;        /* RINT bit raised after the command phase, before StartTransfer */

static void service_irqs(SDPORT_REQUEST *req)
{
    int i;
    for (i = 0; i < 100 && emu_irq_line(); i++) {
        ULONG ev, er;
        BOOLEAN cc, sdio, tun;
        if (g_init.Interrupt(g_ext, &ev, &er, &cc, &sdio, &tun)) {
            int saved = g_mock_irql;
            g_mock_irql = DISPATCH_LEVEL;
            g_init.RequestDpc(g_ext, req, ev, er);
            g_mock_irql = saved;
        }
    }
}

static NTSTATUS port_do(SDPORT_REQUEST *req)
{
    NTSTATUS st;
    int guard;

    req->Type = has_data(&req->Command) ? SdRequestTypeCommandWithTransfer
                                        : SdRequestTypeCommandNoTransfer;
    req->Status = STATUS_SUCCESS;
    st = port_issue_once(req);
    if (st != STATUS_SUCCESS || !has_data(&req->Command)) {
        return st;
    }

    if (g_between_phase_err) {
        emu.regs[SMHC_REG_RINT / 4] |= g_between_phase_err;
        g_between_phase_err = 0;
        service_irqs(req);                  /* the DPC sees a request with nothing left to wait for */
    }

    if (req->Command.TransferMethod == SdTransferMethodSgDma) {
        req->Type = SdRequestTypeStartTransfer;
        return port_issue_once(req);
    }

    for (guard = 0; guard < 100000; guard++) {
        req->Type = SdRequestTypeStartTransfer;
        st = port_issue_once(req);
        if (st != STATUS_MORE_PROCESSING_REQUIRED) {
            return st;
        }
    }
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS busop_reset(SDPORT_RESET_TYPE t)
{
    SDPORT_BUS_OPERATION op;
    memset(&op, 0, sizeof(op));
    op.Type = SdResetHost;
    op.Parameters.ResetType = t;
    return g_init.IssueBusOperation(g_ext, &op);
}

static NTSTATUS busop_clock(ULONG khz)
{
    SDPORT_BUS_OPERATION op;
    memset(&op, 0, sizeof(op));
    op.Type = SdSetClock;
    op.Parameters.FrequencyKhz = khz;
    return g_init.IssueBusOperation(g_ext, &op);
}

static NTSTATUS busop_width(SDPORT_BUS_WIDTH w)
{
    SDPORT_BUS_OPERATION op;
    memset(&op, 0, sizeof(op));
    op.Type = SdSetBusWidth;
    op.Parameters.BusWidth = w;
    return g_init.IssueBusOperation(g_ext, &op);
}

static NTSTATUS busop_voltage(SDPORT_BUS_VOLTAGE v)
{
    SDPORT_BUS_OPERATION op;
    memset(&op, 0, sizeof(op));
    op.Type = SdSetVoltage;
    op.Parameters.Voltage = v;
    return g_init.IssueBusOperation(g_ext, &op);
}

static uint32_t reg(uint32_t off) { return emu.regs[off / 4]; }

/* ---------------------------------------------------------- test fixture */

static void setup(uint32_t transfer_mode, int module_clock_hz)
{
    PHYSICAL_ADDRESS pa;
    size_t ext_size;
    UCHAR slots;

    emu_init();
    emu_fill_card();
    memset(emu_arena, 0, sizeof(emu_arena));
    emu_clear_reg_values();
    g_mock_irql = PASSIVE_LEVEL;
    g_complete_calls = 0;

    if (transfer_mode != 0xFFFFFFFFu) emu_set_reg_value("TransferMode", transfer_mode);
    if (module_clock_hz) emu_set_reg_value("ModuleClockHz", (uint32_t)module_clock_hz);

    memset(&g_init, 0, sizeof(g_init));
    CHECK(NT_SUCCESS(DriverEntry(NULL, NULL)));
    CHECK(g_init.PrivateExtensionSize > 0);

    ext_size = offsetof(SDPORT_SLOT_EXTENSION, PrivateExtension) + g_init.PrivateExtensionSize;
    free(g_slot);
    free(g_mp);
    g_slot = (SDPORT_SLOT_EXTENSION *)calloc(1, ext_size);
    g_mp = (SD_MINIPORT *)calloc(1, sizeof(SD_MINIPORT));
    g_fdo_ext.AttachedTo = &g_pdo;
    g_fdo.DeviceObjectExtension = &g_fdo_ext;
    g_mp->ConfigurationInfo.BusType = SdBusTypeAcpi;
    g_mp->ConfigurationInfo.DeviceObject = &g_fdo;
    g_mp->SlotCount = 1;
    g_mp->SlotExtensionList[0] = g_slot;
    g_slot->Miniport = g_mp;
    g_ext = g_slot->PrivateExtension;

    CHECK(NT_SUCCESS(g_init.GetSlotCount(g_mp, &slots)) && slots == 1);

    pa.QuadPart = 0x04020000;
    CHECK(NT_SUCCESS(g_init.Initialize(g_ext, pa, (PVOID)emu.regs, 0x1000, FALSE)));

    /* what sdport does at start: reset, enable its events */
    CHECK(NT_SUCCESS(busop_reset(SdResetTypeAll)));
    g_init.ToggleEvents(g_ext, ALL_EVENTS, TRUE);
}

static void teardown(void)
{
    g_init.Cleanup(g_mp);
}

static SDPORT_REQUEST cmd_req(ULONG idx, ULONG arg, SDPORT_RESPONSE_TYPE rt)
{
    SDPORT_REQUEST r;
    memset(&r, 0, sizeof(r));
    r.Command.Index = idx;
    r.Command.Argument = arg;
    r.Command.ResponseType = rt;
    r.Command.TransferType = SdTransferTypeNone;
    return r;
}

static SDPORT_REQUEST pio_req(ULONG idx, ULONG block, ULONG blocks, int write, UCHAR *buf)
{
    SDPORT_REQUEST r = cmd_req(idx, block, SdResponseTypeR1);
    r.Command.TransferType = blocks > 1 ? SdTransferTypeMultiBlock : SdTransferTypeSingleBlock;
    r.Command.TransferDirection = write ? SdTransferDirectionWrite : SdTransferDirectionRead;
    r.Command.TransferMethod = SdTransferMethodPio;
    r.Command.BlockSize = 512;
    r.Command.BlockCount = blocks;
    r.Command.Length = 512 * blocks;
    r.Command.DataBuffer = buf;
    r.Command.UseAutoCmd12 = blocks > 1;
    return r;
}

static SDPORT_REQUEST dma_req(ULONG idx, ULONG block, ULONG blocks, int write,
                              SCATTER_GATHER_LIST *sg, uint32_t desc_off)
{
    SDPORT_REQUEST r = cmd_req(idx, block, SdResponseTypeR1);
    r.Command.TransferType = blocks > 1 ? SdTransferTypeMultiBlock : SdTransferTypeSingleBlock;
    r.Command.TransferDirection = write ? SdTransferDirectionWrite : SdTransferDirectionRead;
    r.Command.TransferMethod = SdTransferMethodSgDma;
    r.Command.BlockSize = 512;
    r.Command.BlockCount = blocks;
    r.Command.Length = 512 * blocks;
    r.Command.UseAutoCmd12 = blocks > 1;
    r.Command.ScatterGatherList = sg;
    r.Command.DmaVirtualAddress = emu_arena + desc_off;
    r.Command.DmaPhysicalAddress.QuadPart = EMU_PHYS_BASE + desc_off;
    return r;
}

static SCATTER_GATHER_LIST *make_sg(int n, const uint32_t *offs, const uint32_t *lens)
{
    SCATTER_GATHER_LIST *sg = (SCATTER_GATHER_LIST *)calloc(
        1, sizeof(SCATTER_GATHER_LIST) + (n - 1) * sizeof(SCATTER_GATHER_ELEMENT));
    int i;
    sg->NumberOfElements = n;
    for (i = 0; i < n; i++) {
        sg->Elements[i].Address.QuadPart = EMU_PHYS_BASE + offs[i];
        sg->Elements[i].Length = lens[i];
    }
    return sg;
}

/* ------------------------------------------------------------------ tests */

static void test_init_and_clock(void)
{
    SDPORT_BUS_OPERATION op;
    printf("init + clock (24 MHz module)\n");
    setup(0, 0);

    CHECK(g_init.GetCardDetectState(g_ext) == TRUE);
    CHECK(g_init.GetWriteProtectState(g_ext) == FALSE);

    {
        SDPORT_CAPABILITIES caps;
        g_init.GetSlotCapabilities(g_ext, &caps);
        CHECK(caps.MaximumOutstandingRequests == 1);
        CHECK(caps.Supported.ScatterGatherDma == 0);         /* PIO is the default */
        CHECK(caps.Flags.UsePioForRead && caps.Flags.UsePioForWrite);
        CHECK(caps.Supported.HighSpeed == 0 && caps.Supported.SDR50 == 0);
        CHECK(caps.Supported.SignalingVoltage18V == 0);
        CHECK(caps.Supported.AutoCmd12 == 1);
        CHECK(caps.BaseClockFrequencyKhz == 24000);
    }

    CHECK(NT_SUCCESS(busop_voltage(SdBusVoltage33)));
    CHECK(NT_SUCCESS(busop_voltage(SdBusVoltageOff)));
    CHECK(NT_SUCCESS(busop_clock(400)));

    CHECK((reg(SMHC_REG_CLKCR) & SMHC_CLKCR_DIVIDER_MASK) == 59);
    CHECK(reg(SMHC_REG_CLKCR) & SMHC_CLKCR_CARD_CLOCK_ON);
    CHECK(!(reg(SMHC_REG_CLKCR) & SMHC_CLKCR_MASK_DATA0));      /* released after the update */
    CHECK(reg(SMHC_REG_NTSR) & SMHC_NTSR_MODE_SEL_NEW);
    CHECK(reg(SMHC_REG_SAMP_DL) == SMHC_SAMP_DL_SW_EN);
    CHECK(reg(SMHC_REG_GCTRL) & SMHC_GCTRL_INT_ENABLE);
    CHECK(reg(SMHC_REG_THLDC) == SMHC_THLDC_DEFAULT);
    CHECK(reg(SMHC_REG_TMOUT) == SMHC_TMOUT_MAX);
    CHECK(reg(SMHC_REG_WIDTH) == SMHC_WIDTH_1BIT);              /* ResetAll forces 1-bit */

    CHECK(NT_SUCCESS(busop_clock(25000)));
    CHECK((reg(SMHC_REG_CLKCR) & SMHC_CLKCR_DIVIDER_MASK) == 0);   /* 24 MHz: bypass */
    CHECK(reg(SMHC_REG_CLKCR) & SMHC_CLKCR_CARD_CLOCK_ON);

    CHECK(NT_SUCCESS(busop_width(SdBusWidth4Bit)));
    CHECK(reg(SMHC_REG_WIDTH) == SMHC_WIDTH_4BIT);
    CHECK(busop_width(SdBusWidth8Bit) == STATUS_NOT_SUPPORTED);

    memset(&op, 0, sizeof(op));
    op.Type = SdSetSignalingVoltage;
    op.Parameters.SignalingVoltage = SdSignalingVoltage18;
    CHECK(g_init.IssueBusOperation(g_ext, &op) == STATUS_NOT_SUPPORTED);
    op.Parameters.SignalingVoltage = SdSignalingVoltage33;
    CHECK(NT_SUCCESS(g_init.IssueBusOperation(g_ext, &op)));
    op.Type = SdExecuteTuning;
    CHECK(g_init.IssueBusOperation(g_ext, &op) == STATUS_NOT_SUPPORTED);

    CHECK(NT_SUCCESS(busop_clock(0)));
    CHECK(!(reg(SMHC_REG_CLKCR) & SMHC_CLKCR_CARD_CLOCK_ON));
    teardown();
}

static void test_clock_other_module_rates(void)
{
    printf("clock with 48 MHz module (registry override)\n");
    setup(0, 48000000);
    CHECK(NT_SUCCESS(busop_clock(400)));
    CHECK((reg(SMHC_REG_CLKCR) & SMHC_CLKCR_DIVIDER_MASK) == 119);
    CHECK(NT_SUCCESS(busop_clock(25000)));
    CHECK((reg(SMHC_REG_CLKCR) & SMHC_CLKCR_DIVIDER_MASK) == 1);
    teardown();
}

static void test_enumeration_commands(void)
{
    SDPORT_REQUEST r;
    ULONG resp[4];
    UCHAR out[16];
    int i;

    printf("enumeration commands\n");
    setup(0, 0);
    busop_clock(400);

    r = cmd_req(0, 0, SdResponseTypeNone);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    r = cmd_req(8, 0x1AA, SdResponseTypeR1);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    g_init.GetResponse(g_ext, &r.Command, resp);
    CHECK(resp[0] == (0x900 | 8));
    r = cmd_req(41, 0x40FF8000, SdResponseTypeR3);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    r = cmd_req(2, 0, SdResponseTypeR2);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    g_init.GetResponse(g_ext, &r.Command, out);
    {
        UCHAR expect[15] = { 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00 };
        /* emulator RESP3..0 = 00112233 44556677 8899AABB CCDDEEFF, low byte is CRC7|end and is dropped */
        CHECK(memcmp(out, expect, 15) == 0);
    }
    r = cmd_req(3, 0, SdResponseTypeR6);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    r = cmd_req(7, 0x12340000, SdResponseTypeR1B);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    (void)resp;

    CHECK(emu.log_count == 6);
    CHECK(emu.log[0].index == 0);
    CHECK(emu.log[0].cmdreg & SMHC_CMD_SEND_INIT_SEQ);          /* only CMD0 */
    CHECK(!(emu.log[0].cmdreg & SMHC_CMD_RESP_EXPIRE));
    for (i = 1; i < emu.log_count; i++) {
        CHECK(!(emu.log[i].cmdreg & SMHC_CMD_SEND_INIT_SEQ));
        CHECK(emu.log[i].cmdreg & SMHC_CMD_RESP_EXPIRE);
    }
    CHECK(!(emu.log[2].cmdreg & SMHC_CMD_CHK_RESPONSE_CRC));    /* R3: no CRC */
    CHECK(emu.log[3].cmdreg & SMHC_CMD_LONG_RESPONSE);          /* R2 */
    CHECK(emu.log[3].cmdreg & SMHC_CMD_CHK_RESPONSE_CRC);
    CHECK(emu.log[1].arg == 0x1AA);
    teardown();
}

static void check_buf(const UCHAR *buf, uint32_t block, uint32_t blocks)
{
    CHECK(memcmp(buf, &emu_card[block * 512], blocks * 512) == 0);
}

static void test_pio_reads_mode(int edge)
{
    static UCHAR buf[512 * 8];
    SDPORT_REQUEST r;

    printf("PIO reads (%s-triggered data requests)\n", edge ? "edge" : "level");
    setup(0, 0);
    emu.dreq_edge_only = edge;
    busop_clock(400);
    busop_width(SdBusWidth4Bit);

    /* tiny reads (SCR = 8 bytes): shorter than the FIFO threshold */
    memset(buf, 0, sizeof(buf));
    r = pio_req(51, 3, 1, 0, buf);
    r.Command.BlockSize = 8;
    r.Command.Length = 8;
    CHECK(port_do(&r) == STATUS_SUCCESS);
    CHECK(memcmp(buf, &emu_card[3 * 512], 8) == 0);

    /* 64-byte status read */
    memset(buf, 0, sizeof(buf));
    r = pio_req(13, 5, 1, 0, buf);
    r.Command.BlockSize = 64;
    r.Command.Length = 64;
    CHECK(port_do(&r) == STATUS_SUCCESS);
    CHECK(memcmp(buf, &emu_card[5 * 512], 64) == 0);

    /* single block: 128 words through a 64-word FIFO */
    memset(buf, 0, sizeof(buf));
    r = pio_req(17, 7, 1, 0, buf);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    check_buf(buf, 7, 1);

    /* multi block with auto-CMD12 */
    memset(buf, 0, sizeof(buf));
    r = pio_req(18, 100, 8, 0, buf);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    CHECK(emu.auto_pending == 0);                                /* not completed before the auto CMD12 */
    check_buf(buf, 100, 8);

    {
        EMU_CMD_LOG *l = &emu.log[emu.log_count - 1];
        CHECK(l->index == 18);
        CHECK(l->cmdreg & SMHC_CMD_AUTO_STOP);
        CHECK(l->cmdreg & SMHC_CMD_DATA_EXPIRE);
        CHECK(l->cmdreg & SMHC_CMD_WAIT_PRE_OVER);
        CHECK(!(l->cmdreg & SMHC_CMD_WRITE));
    }
    CHECK(emu.fifo_reset_during_xfer == 0);
    CHECK(emu.stop_cmds == 0);

    /* controller left in a clean state */
    CHECK(!(reg(SMHC_REG_GCTRL) & SMHC_GCTRL_ACCESS_BY_AHB));
    CHECK(emu.fifo_level == 0);
    teardown();
}

static void test_pio_reads(void)
{
    test_pio_reads_mode(0);
    test_pio_reads_mode(1);
}

static void test_pio_writes_mode(int edge)
{
    static UCHAR buf[512 * 8];
    SDPORT_REQUEST r;
    uint32_t i;

    printf("PIO writes, card busy (%s-triggered data requests)\n", edge ? "edge" : "level");
    setup(0, 0);
    emu.dreq_edge_only = edge;
    busop_clock(400);

    for (i = 0; i < sizeof(buf); i++) buf[i] = (UCHAR)(i * 3 + 1);

    r = pio_req(24, 40, 1, 1, buf);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    CHECK(memcmp(&emu_card[40 * 512], buf, 512) == 0);
    CHECK(emu.busy_left == 0);                                  /* busy was waited out */

    r = pio_req(25, 60, 8, 1, buf);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    CHECK(emu.auto_pending == 0);
    CHECK(memcmp(&emu_card[60 * 512], buf, 8 * 512) == 0);
    {
        EMU_CMD_LOG *l = &emu.log[emu.log_count - 1];
        CHECK(l->index == 25);
        CHECK((l->cmdreg & SMHC_CMD_WRITE) && (l->cmdreg & SMHC_CMD_AUTO_STOP));
    }
    CHECK(emu.fifo_reset_during_xfer == 0);
    teardown();
}

static void test_pio_writes(void)
{
    test_pio_writes_mode(0);
    test_pio_writes_mode(1);
}

static void test_dma(void)
{
    SDPORT_REQUEST r;
    SCATTER_GATHER_LIST *sg;
    uint32_t offs[3], lens[3];
    uint32_t desc = 0x0000;          /* descriptor table at arena start */
    uint32_t base = 0x4000;
    uint32_t i;

    printf("DMA reads/writes (IDMAC chain)\n");
    setup(1, 0);

    {
        SDPORT_CAPABILITIES caps;
        g_init.GetSlotCapabilities(g_ext, &caps);
        CHECK(caps.Supported.ScatterGatherDma == 1);
        CHECK(caps.DmaDescriptorSize == 16);
        CHECK(caps.AlignmentRequirement == 3);
        CHECK(caps.Supported.Address64Bit == 0);
    }

    busop_clock(400);
    busop_clock(25000);
    busop_width(SdBusWidth4Bit);

    /* single block read, one element */
    offs[0] = base; lens[0] = 512;
    sg = make_sg(1, offs, lens);
    r = dma_req(17, 9, 1, 0, sg, desc);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    CHECK(((PSMHC_EXTENSION)g_ext)->OutstandingRequest == NULL);     /* released after the StartTransfer phase */
    CHECK(memcmp(emu_arena + base, &emu_card[9 * 512], 512) == 0);
    CHECK(!(emu.log[emu.log_count - 1].cmdreg & SMHC_CMD_AUTO_STOP));
    free(sg);

    /* multi block read, three discontiguous elements */
    memset(emu_arena + base, 0, 0x20000);
    offs[0] = base;           lens[0] = 4096;
    offs[1] = base + 0x10000; lens[1] = 2048;
    offs[2] = base + 0x8000;  lens[2] = 2048;
    sg = make_sg(3, offs, lens);
    r = dma_req(18, 200, 16, 0, sg, desc);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    CHECK(emu.auto_pending == 0);
    CHECK(memcmp(emu_arena + offs[0], &emu_card[200 * 512], 4096) == 0);
    CHECK(memcmp(emu_arena + offs[1], &emu_card[200 * 512 + 4096], 2048) == 0);
    CHECK(memcmp(emu_arena + offs[2], &emu_card[200 * 512 + 6144], 2048) == 0);
    CHECK(emu.log[emu.log_count - 1].cmdreg & SMHC_CMD_AUTO_STOP);
    CHECK(!(reg(SMHC_REG_GCTRL) & SMHC_GCTRL_DMA_ENABLE));       /* cleaned up */
    CHECK(reg(SMHC_REG_DMAC) == 0);
    free(sg);

    /* one 192 KiB element: split into 64 KiB descriptors, 0-encoded size */
    for (i = 0; i < 0x30000; i++) emu_arena[0x40000 + i] = (UCHAR)(i ^ (i >> 9));
    offs[0] = 0x40000; lens[0] = 0x30000;
    sg = make_sg(1, offs, lens);
    r = dma_req(25, 1000, 0x30000 / 512, 1, sg, desc);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    CHECK(memcmp(&emu_card[1000 * 512], emu_arena + 0x40000, 0x30000) == 0);
    CHECK(emu.busy_left == 0);
    free(sg);

    /* write, two elements */
    for (i = 0; i < 4096; i++) emu_arena[base + i] = (UCHAR)(i * 5);
    offs[0] = base; lens[0] = 1024;
    offs[1] = base + 0x800; lens[1] = 3072;
    sg = make_sg(2, offs, lens);
    r = dma_req(25, 2000, 8, 1, sg, desc);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    CHECK(memcmp(&emu_card[2000 * 512], emu_arena + base, 1024) == 0);
    CHECK(memcmp(&emu_card[2000 * 512 + 1024], emu_arena + base + 0x800, 3072) == 0);
    free(sg);

    /* bad scatter/gather (unaligned) is rejected cleanly and leaves no request pending */
    offs[0] = base + 2; lens[0] = 512;
    sg = make_sg(1, offs, lens);
    r = dma_req(17, 9, 1, 0, sg, desc);
    CHECK(port_do(&r) == STATUS_INVALID_PARAMETER);
    free(sg);
    r = cmd_req(13, 0, SdResponseTypeR1);
    CHECK(port_do(&r) == STATUS_SUCCESS);

    CHECK(emu.fifo_reset_during_xfer == 0);
    teardown();
}

static void test_errors_and_reset(void)
{
    static UCHAR buf[512 * 4];
    SDPORT_REQUEST r;
    int stops;

    printf("error paths + reset restore\n");
    setup(0, 0);
    busop_clock(400);
    busop_clock(25000);
    busop_width(SdBusWidth4Bit);

    /* response timeout: the late COMMAND_DONE must be waited for */
    r = cmd_req(8, 0x1AA, SdResponseTypeR1);
    emu.inject_resp_timeout = 1;
    CHECK(port_do(&r) == STATUS_IO_TIMEOUT);
    CHECK(emu.late_cmd_done == 0);                               /* completed only after COMMAND_DONE */
    CHECK(NT_SUCCESS(busop_reset(SdResetTypeCmd)));
    CHECK(emu.stop_cmds == 0);                                   /* no data: no stop needed */

    /* controller reset must preserve what the driver owns */
    CHECK(reg(SMHC_REG_WIDTH) == SMHC_WIDTH_4BIT);
    CHECK((reg(SMHC_REG_CLKCR) & SMHC_CLKCR_DIVIDER_MASK) == 0);
    CHECK(reg(SMHC_REG_CLKCR) & SMHC_CLKCR_CARD_CLOCK_ON);
    CHECK(reg(SMHC_REG_NTSR) & SMHC_NTSR_MODE_SEL_NEW);
    CHECK(reg(SMHC_REG_IMASK) & SMHC_INT_COMMAND_DONE);          /* sdport's enables survive */
    CHECK(reg(SMHC_REG_IMASK) & SMHC_INT_ERRORS);

    /* data CRC error half way through a PIO read */
    memset(buf, 0, sizeof(buf));
    r = pio_req(18, 20, 4, 0, buf);
    emu.inject_data_crc_after_words = 100;
    CHECK(port_do(&r) == STATUS_CRC_ERROR);
    stops = emu.stop_cmds;
    CHECK(NT_SUCCESS(busop_reset(SdResetTypeCmd)));              /* returns early: data failed */
    CHECK(emu.stop_cmds == stops);
    CHECK(NT_SUCCESS(busop_reset(SdResetTypeDat)));
    CHECK(emu.stop_cmds == stops + 1);                           /* manual CMD12 with STOP_ABORT */
    CHECK(emu.stop_imask == 0);                                  /* interrupts masked while polling it */
    CHECK(reg(SMHC_REG_IMASK) & SMHC_INT_COMMAND_DONE);          /* ... and sdport's enables are back */
    CHECK(reg(SMHC_REG_WIDTH) == SMHC_WIDTH_4BIT);
    CHECK(!(reg(SMHC_REG_GCTRL) & SMHC_GCTRL_ACCESS_BY_AHB));

    /* data timeout: no DATA_OVER follows, must not hang */
    memset(buf, 0, sizeof(buf));
    r = pio_req(17, 21, 1, 0, buf);
    emu.inject_data_crc_after_words = 10;
    emu.inject_data_err_bits = SMHC_INT_DATA_TIMEOUT;
    CHECK(port_do(&r) == STATUS_IO_TIMEOUT);
    emu.inject_data_err_bits = 0;
    CHECK(NT_SUCCESS(busop_reset(SdResetTypeCmd)));
    CHECK(NT_SUCCESS(busop_reset(SdResetTypeDat)));
    CHECK(emu.stop_cmds == stops + 2);

    /* an error that lands between the command phase and StartTransfer must fail the
       request instead of waiting for events that never come */
    memset(buf, 0, sizeof(buf));
    r = pio_req(17, 22, 1, 0, buf);
    g_between_phase_err = SMHC_INT_DATA_CRC_ERROR;
    CHECK(port_do(&r) == STATUS_CRC_ERROR);
    CHECK(NT_SUCCESS(busop_reset(SdResetTypeCmd)));
    CHECK(NT_SUCCESS(busop_reset(SdResetTypeDat)));

    /* and the host is usable afterwards */
    memset(buf, 0, sizeof(buf));
    r = pio_req(18, 20, 4, 0, buf);
    CHECK(port_do(&r) == STATUS_SUCCESS);
    check_buf(buf, 20, 4);

    /* full reset returns to a quiet 1-bit, clock-off state */
    CHECK(NT_SUCCESS(busop_reset(SdResetTypeAll)));
    CHECK(reg(SMHC_REG_WIDTH) == SMHC_WIDTH_1BIT);
    CHECK(!(reg(SMHC_REG_CLKCR) & SMHC_CLKCR_CARD_CLOCK_ON));
    CHECK(reg(SMHC_REG_IMASK) == 0);
    teardown();
}

static void test_bad_register_window(void)
{
    PHYSICAL_ADDRESS pa;
    SDPORT_BUS_OPERATION op;

    printf("undersized register window\n");
    setup(0, 0);
    teardown();

    pa.QuadPart = 0x04020000;
    CHECK(NT_SUCCESS(g_init.Initialize(g_ext, pa, (PVOID)emu.regs, 0x100, FALSE)));
    memset(&op, 0, sizeof(op));
    op.Type = SdSetClock;
    op.Parameters.FrequencyKhz = 400;
    CHECK(g_init.IssueBusOperation(g_ext, &op) == STATUS_DEVICE_CONFIGURATION_ERROR);
}

int main(int argc, char **argv)
{
    if (argc > 1) g_mock_log_level = atoi(argv[1]);

    test_init_and_clock();
    test_clock_other_module_rates();
    test_enumeration_commands();
    test_pio_reads();
    test_pio_writes();
    test_dma();
    test_errors_and_reset();
    test_bad_register_window();

    printf("%d checks, %s (%d failures)\n", g_checks, g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail != 0;
}
