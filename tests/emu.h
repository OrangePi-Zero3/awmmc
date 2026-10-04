/*
 * Register-level model of the Allwinner SMHC plus an SD card, for host tests.
 *
 * The model encodes THIS PROJECT'S READING of the references (u-boot / Linux /
 * dwcmshc); where the reference is silent it makes the pessimistic choice
 * (documented in emu.c).  Passing the tests shows the driver is consistent with
 * that model, not that real silicon behaves the same way.
 */

#pragma once

#include "ntddk.h"
#include "sdport.h"
#include "../driver/smhc_regs.h"

#define EMU_ARENA_SIZE      (1u << 20)
#define EMU_PHYS_BASE       0x40000000u
#define EMU_CARD_SIZE       (1u << 20)
#define EMU_FIFO_DEPTH      64              /* words; deliberately small to force chunking */
#define EMU_MAX_LOG         256

typedef struct {
    uint32_t index, arg, cmdreg;
} EMU_CMD_LOG;

typedef struct {
    uint32_t regs[0x300 / 4];

    /* fault injection */
    int inject_resp_timeout;        /* next command: RESP_TIMEOUT then (late) COMMAND_DONE */
    int inject_data_crc_after_words;/* next read: DATA_CRC_ERROR after N words (-1 = off) */
    int reset_clears_regs;          /* soft reset clears CLKCR/WIDTH/IMASK/NTSR (pessimistic) */
    int busy_polls_after_write;
    int dreq_edge_only;             /* RXDR/TXDR assert only on a watermark crossing (not level) */
    uint32_t inject_data_err_bits;  /* RINT bit raised by inject_data_crc_after_words (default CRC) */

    /* observation */
    EMU_CMD_LOG log[EMU_MAX_LOG];
    int log_count;
    int stop_cmds;                  /* manual CMD12 with STOP_ABORT */
    uint32_t stop_imask;            /* IMASK value when the last STOP_ABORT command was written */
    int fifo_resets;
    int fifo_reset_during_xfer;     /* FIFO reset while data was still pending: a driver bug */

    /* internal */
    uint32_t fifo[EMU_FIFO_DEPTH];
    int fifo_head, fifo_level;
    int rd_active, wr_active;
    uint32_t xfer_words, xfer_done, xfer_card_off;
    int xfer_auto_stop;
    int data_over_sent;
    int crc_inject_at;
    int busy_left;
    int rx_cond_prev, tx_cond_prev;
    int late_cmd_done;
    int dma_pending;                /* IDMAC read still moving FIFO -> memory (ticks left) */
    int auto_pending;               /* AUTO_COMMAND_DONE follows DATA_OVER one tick later */
    uint32_t card_off_cur;
} EMU;

extern EMU emu;
extern uint8_t emu_arena[EMU_ARENA_SIZE];
extern uint8_t emu_card[EMU_CARD_SIZE];

void emu_init(void);
int  emu_irq_line(void);
void emu_pump(void);
void emu_card_tick(void);
void *emu_phys_to_ptr(uint32_t phys);
uint32_t emu_ptr_to_phys(const void *p);
void emu_fill_card(void);

/* workitem mock */
void emu_run_workitems(void);
int  emu_workitems_pending(void);

/* registry mock */
void emu_set_reg_value(const char *name, uint32_t value);
void emu_clear_reg_values(void);
extern int g_mock_log_level;
extern int g_mock_irql;
