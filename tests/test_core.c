/*
 * Unit tests for the hardware-independent logic in driver/smhc_core.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include "ntddk.h"
#include "sdport.h"
#include "../driver/smhc_core.h"

int g_mock_log_level = -1;
int g_mock_irql = 0;

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

/* Both candidate divider semantics must stay at or below the requested clock. */
static void check_divider(ULONG module, ULONG target)
{
    ULONG actual;
    ULONG f = SmhcCalcClkcrDivider(module, target, &actual);
    ULONG linear_hz = module / (f + 1);
    ULONG dw_hz = (f == 0) ? module : module / (2 * f);

    printf("  module %u target %u -> field %u (linear %u Hz, DW %u Hz)\n",
           module, target, f, linear_hz, dw_hz);

    CHECK(f <= SMHC_CLKCR_DIVIDER_MASK);
    if (module > target) {
        CHECK(linear_hz <= target);
        CHECK(dw_hz <= target);
        /* and not absurdly slow under either rule: within 2x + 1 step */
        CHECK(linear_hz * 2 >= target || f == SMHC_CLKCR_DIVIDER_MASK);
    } else {
        CHECK(f == 0);
        CHECK(actual == module);
    }
}

static void test_divider(void)
{
    ULONG actual;
    printf("divider\n");
    check_divider(24000000, 400000);
    check_divider(24000000, 25000000);
    check_divider(24000000, 24000000);
    check_divider(24000000, 12000000);
    check_divider(24000000, 1000000);
    check_divider(48000000, 400000);
    check_divider(48000000, 25000000);
    check_divider(50000000, 400000);

    CHECK(SmhcCalcClkcrDivider(24000000, 400000, &actual) == 59);   /* documented example */
    CHECK(actual == 400000);
    CHECK(SmhcCalcClkcrDivider(24000000, 0, &actual) == 0 && actual == 0);
    CHECK(SmhcCalcClkcrDivider(24000000, 24000000, &actual) == 0 && actual == 24000000);
    CHECK(SmhcCalcClkcrDivider(24000000, 12000000, &actual) == 1 && actual == 12000000);
    CHECK(SmhcCalcClkcrDivider(1000000000, 100, &actual) == 255);   /* clamps */
}

static void test_events(void)
{
    ULONG ev, er;
    printf("event mapping\n");

    SmhcConvertInterrupts(SMHC_INT_COMMAND_DONE, 0, FALSE, &ev, &er);
    CHECK(ev == SDPORT_EVENT_CARD_RESPONSE && er == 0);

    /* plain transfer completes on DATA_OVER */
    SmhcConvertInterrupts(SMHC_INT_DATA_OVER, 0, FALSE, &ev, &er);
    CHECK(ev == SDPORT_EVENT_CARD_RW_END);

    /* auto-stop transfer: DATA_OVER ignored, AUTO_COMMAND_DONE completes */
    SmhcConvertInterrupts(SMHC_INT_DATA_OVER, 0, TRUE, &ev, &er);
    CHECK(ev == 0);
    SmhcConvertInterrupts(SMHC_INT_AUTO_COMMAND_DONE, 0, TRUE, &ev, &er);
    CHECK(ev == SDPORT_EVENT_CARD_RW_END);
    SmhcConvertInterrupts(SMHC_INT_AUTO_COMMAND_DONE, 0, FALSE, &ev, &er);
    CHECK(ev == 0);

    SmhcConvertInterrupts(SMHC_INT_RX_DATA_REQUEST, 0, FALSE, &ev, &er);
    CHECK(ev == SDPORT_EVENT_BUFFER_FULL);
    SmhcConvertInterrupts(SMHC_INT_TX_DATA_REQUEST, 0, FALSE, &ev, &er);
    CHECK(ev == SDPORT_EVENT_BUFFER_EMPTY);

    SmhcConvertInterrupts(0, SMHC_IDST_RX_INTERRUPT, FALSE, &ev, &er);
    CHECK(ev == SDPORT_EVENT_DMA_COMPLETE);

    SmhcConvertInterrupts(SMHC_INT_RESP_TIMEOUT, 0, FALSE, &ev, &er);
    CHECK((ev & SDPORT_EVENT_ERROR) && er == SDPORT_ERROR_CMD_TIMEOUT);
    SmhcConvertInterrupts(SMHC_INT_DATA_CRC_ERROR, 0, FALSE, &ev, &er);
    CHECK(er == SDPORT_ERROR_DATA_CRC_ERROR);
    SmhcConvertInterrupts(SMHC_INT_FIFO_RUN_ERROR, 0, FALSE, &ev, &er);
    CHECK(er == SDPORT_GENERIC_IO_ERROR);
    SmhcConvertInterrupts(0, SMHC_IDST_FATAL_BUS_ERROR, FALSE, &ev, &er);
    CHECK(er == SDPORT_ERROR_ADMA_ERROR);

    /* voltage-change-done is not an error and not an event */
    SmhcConvertInterrupts(SMHC_INT_VOLTAGE_CHANGE_DONE, 0, FALSE, &ev, &er);
    CHECK(ev == 0 && er == 0);

    CHECK((SMHC_INT_ERRORS & SMHC_INT_VOLTAGE_CHANGE_DONE) == 0);
    CHECK(SmhcEventsToIntMask(SDPORT_EVENT_ERROR) == SMHC_INT_ERRORS);
    CHECK(SmhcEventsToIntMask(SDPORT_EVENT_CARD_RW_END) ==
          (SMHC_INT_DATA_OVER | SMHC_INT_AUTO_COMMAND_DONE));
}

static void test_r2(void)
{
    ULONG resp[4] = { 0xCCDDEEFF, 0x8899AABB, 0x44556677, 0x00112233 };
    UCHAR out[16];
    UCHAR expect[15] = { 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55,
                         0x44, 0x33, 0x22, 0x11, 0x00 };
    printf("R2\n");
    SmhcFormatR2(resp, out);
    CHECK(memcmp(out, expect, 15) == 0);
    CHECK(out[15] == 0);
}

static void test_descriptors(void)
{
    SMHC_IDMAC_DESCRIPTOR d[8];
    SMHC_IDMAC_BUILDER b;
    NTSTATUS s;

    printf("descriptors\n");

    /* three elements, the middle one longer than 64 KiB (splits into 2) */
    memset(d, 0xAA, sizeof(d));
    SmhcIdmacBuilderInit(&b, d, 0x40001000, 8);
    CHECK(NT_SUCCESS(SmhcIdmacBuilderAppend(&b, 0x40010000, 4096)));
    CHECK(NT_SUCCESS(SmhcIdmacBuilderAppend(&b, 0x40020000, 0x10000 + 512)));
    CHECK(NT_SUCCESS(SmhcIdmacBuilderAppend(&b, 0x40040000, 512)));
    CHECK(NT_SUCCESS(SmhcIdmacBuilderFinish(&b)));
    CHECK(b.Count == 4);
    CHECK(b.TotalBytes == 4096 + 0x10000 + 512 + 512);

    CHECK(d[0].Config == (SMHC_IDMAC_DES_CH | SMHC_IDMAC_DES_OWN | SMHC_IDMAC_DES_DIC | SMHC_IDMAC_DES_FD));
    CHECK(d[0].BufSize == 4096);
    CHECK(d[0].BufAddr == (0x40010000u >> 2));
    CHECK(d[0].NextDesc == ((0x40001000u + 16) >> 2));

    CHECK(d[1].BufSize == 0);                              /* 65536 encodes as 0 */
    CHECK(d[1].BufAddr == (0x40020000u >> 2));
    CHECK(d[2].BufSize == 512);
    CHECK(d[2].BufAddr == ((0x40020000u + 0x10000) >> 2));
    CHECK(!(d[1].Config & SMHC_IDMAC_DES_FD));

    CHECK(d[3].Config == (SMHC_IDMAC_DES_CH | SMHC_IDMAC_DES_OWN | SMHC_IDMAC_DES_LD | SMHC_IDMAC_DES_ER));
    CHECK(d[3].NextDesc == 0);

    /* single descriptor is both first and last */
    SmhcIdmacBuilderInit(&b, d, 0x40001000, 8);
    CHECK(NT_SUCCESS(SmhcIdmacBuilderAppend(&b, 0x40010000, 512)));
    CHECK(NT_SUCCESS(SmhcIdmacBuilderFinish(&b)));
    CHECK((d[0].Config & (SMHC_IDMAC_DES_FD | SMHC_IDMAC_DES_LD)) ==
          (SMHC_IDMAC_DES_FD | SMHC_IDMAC_DES_LD));

    /* rejects */
    SmhcIdmacBuilderInit(&b, d, 0x40001000, 2);
    s = SmhcIdmacBuilderAppend(&b, 0x40010001, 512);       CHECK(s == STATUS_INVALID_PARAMETER);
    s = SmhcIdmacBuilderAppend(&b, 0x40010000, 510);       CHECK(s == STATUS_INVALID_PARAMETER);
    s = SmhcIdmacBuilderAppend(&b, 0x40010000, 0);         CHECK(s == STATUS_INVALID_PARAMETER);
    s = SmhcIdmacBuilderAppend(&b, 0xFFFFFE00ull, 1024);   CHECK(s == STATUS_INVALID_PARAMETER);
    s = SmhcIdmacBuilderAppend(&b, 0x100000000ull, 512);   CHECK(s == STATUS_INVALID_PARAMETER);
    CHECK(NT_SUCCESS(SmhcIdmacBuilderAppend(&b, 0x40010000, 512)));
    CHECK(NT_SUCCESS(SmhcIdmacBuilderAppend(&b, 0x40020000, 512)));
    s = SmhcIdmacBuilderAppend(&b, 0x40030000, 512);       CHECK(s == STATUS_BUFFER_TOO_SMALL);

    SmhcIdmacBuilderInit(&b, d, 0x40001000, 2);
    CHECK(SmhcIdmacBuilderFinish(&b) == STATUS_INVALID_PARAMETER);   /* empty */
}

static void test_layout(void)
{
    printf("layout\n");
    CHECK(sizeof(SMHC_IDMAC_DESCRIPTOR) == 16);
    CHECK(SMHC_FTRGL_VALUE(7, 8) == SMHC_FTRGL_DEFAULT);            /* matches Linux's 0x20070008 */
    CHECK(SMHC_THLDC_DEFAULT == ((512u << 16) | 4u | 1u));
    CHECK(SMHC_GCTRL_ALL_RESET == 7u);
    CHECK(SMHC_STATUS_FIFO_LEVEL(0x20000u * 5) == 5);
    CHECK(SMHC_INT_ERRORS == 0xBFC2u - SMHC_INT_VOLTAGE_CHANGE_DONE);   /* 0xbfc2 minus bit 10 */
}

int main(void)
{
    test_divider();
    test_events();
    test_r2();
    test_descriptors();
    test_layout();
    printf(g_fail ? "CORE TESTS FAILED (%d)\n" : "core tests passed\n", g_fail);
    return g_fail != 0;
}
