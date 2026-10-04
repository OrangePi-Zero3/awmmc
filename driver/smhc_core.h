/*++

Module Name:

    smhc_core.h

Abstract:

    Pure, hardware-independent logic of the AWMC0001 SMHC miniport: clock
    divider selection, interrupt-to-sdport event mapping, IDMAC descriptor chain
    construction and R2 response formatting.

    Nothing here touches registers or calls kernel services, so the same code is
    compiled by the driver and by the host-side unit tests in tests/.  Include
    smhc.h (driver) or the test mock (tests) before this file; both provide the
    base types, NTSTATUS codes and SDPORT_EVENT_* / SDPORT_ERROR_* constants.

--*/

#pragma once

#include "smhc_regs.h"

//
// ---------------------------------------------------------------------------
// Card clock divider.
//
// The module clock (CCU MMC0_CLK_REG) is owned by firmware and is NOT touched by
// this driver version; only the SMHC-internal divider CLKCR[7:0] is.
//
// What the references establish about CLKCR[7:0]:
//   [LX] sunxi_mmc_clk_set_rate:   rval |= div - 1;  ...  rate /= div;
//        used only with div = 1 (field 0) and div = 2 (field 1), i.e. "card clock
//        = module clock / (field + 1)" for those two values.
//   [UB-C] mmc_config_clock: "Clear internal divider" (field 0) and never uses it.
//
// Nothing in the references says what fields >= 2 do.  Two plausible rules:
//   linear:      card = module / (field + 1)       (extrapolating Linux)
//   DesignWare:  card = module / (2 * field)       (field 0 = bypass; the SMHC is
//                                                   DW-MMC derived)
// They agree at field 0 (bypass) and field 1 (both /2).  The driver uses the
// LINEAR field, field = ceil(module / target) - 1.  If the DesignWare rule is the
// true one, the same field divides by roughly twice as much, so the clock is about
// HALF of what was asked for (e.g. 203 kHz instead of 400 kHz): slower, but never
// faster than the card's limit.  (Picking the larger of the two candidate fields
// is the same thing: the linear one is never the smaller.)  The cost of being wrong
// is therefore speed only, and only for divided clocks.  UNVERIFIED: measure CLK on
// hardware to learn which rule applies.
// ---------------------------------------------------------------------------
//

static __inline ULONG
SmhcDivCeil(
    ULONG Numerator,
    ULONG Denominator
    )
{
    return (ULONG)(((ULONGLONG)Numerator + Denominator - 1) / Denominator);
}

static __inline ULONG
SmhcCalcClkcrDivider(
    ULONG ModuleHz,
    ULONG TargetHz,
    ULONG *ActualHz
    )
{
    ULONG Field;

    if (TargetHz == 0) {
        *ActualHz = 0;
        return 0;
    }

    if (ModuleHz <= TargetHz) {
        *ActualHz = ModuleHz;
        return 0;
    }

    Field = SmhcDivCeil(ModuleHz, TargetHz) - 1;     // >= 1 because ModuleHz > TargetHz
    if (Field > SMHC_CLKCR_DIVIDER_MASK) {
        Field = SMHC_CLKCR_DIVIDER_MASK;
    }

    *ActualHz = ModuleHz / (Field + 1);   // linear interpretation (upper bound)
    return Field;
}

//
// ---------------------------------------------------------------------------
// Interrupt status -> sdport events / errors.
//
// Same structure as dwcmshc.cpp MshcConvertIntStatusToStandardEvents (DW-MMC
// miniport for sdport), with the Allwinner differences:
//
//  * Multi-block transfers with AUTO_STOP complete on AUTO_COMMAND_DONE and
//    ignore DATA_OVER; everything else completes on DATA_OVER.
//    [UB-C] sunxi_mmc_send_cmd_common waits for AUTO_COMMAND_DONE when blocks > 1
//    and DATA_OVER otherwise; [LX] sunxi_mmc_request sets imask the same way.
//  * IDMAC receive-interrupt (RI) means the FIFO has been drained to memory;
//    Linux (sunxi_mmc_irq: wait_dma) will not finalize a read before it.
//  * VOLTAGE_CHANGE_DONE is ignored (no voltage switching).
// ---------------------------------------------------------------------------
//

static __inline VOID
SmhcConvertInterrupts(
    ULONG Mint,
    ULONG Idst,
    BOOLEAN AutoStopActive,
    ULONG *Events,
    ULONG *Errors
    )
{
    ULONG E = 0;
    ULONG X = 0;

    if (Mint & SMHC_INT_COMMAND_DONE) {
        E |= SDPORT_EVENT_CARD_RESPONSE;
    }

    if (AutoStopActive) {
        if (Mint & SMHC_INT_AUTO_COMMAND_DONE) {
            E |= SDPORT_EVENT_CARD_RW_END;
        }
    } else if (Mint & SMHC_INT_DATA_OVER) {
        E |= SDPORT_EVENT_CARD_RW_END;
    }

    if (Mint & SMHC_INT_TX_DATA_REQUEST) {
        E |= SDPORT_EVENT_BUFFER_EMPTY;
    }

    if (Mint & SMHC_INT_RX_DATA_REQUEST) {
        E |= SDPORT_EVENT_BUFFER_FULL;
    }

    if (Idst & (SMHC_IDST_RX_INTERRUPT | SMHC_IDST_TX_INTERRUPT)) {
        E |= SDPORT_EVENT_DMA_COMPLETE;
    }

    if (Mint & SMHC_INT_RESP_TIMEOUT) {
        X |= SDPORT_ERROR_CMD_TIMEOUT;
    }

    if (Mint & SMHC_INT_RESP_CRC_ERROR) {
        X |= SDPORT_ERROR_CMD_CRC_ERROR;
    }

    if (Mint & SMHC_INT_RESP_ERROR) {
        X |= SDPORT_ERROR_CMD_END_BIT_ERROR | SDPORT_ERROR_CMD_INDEX_ERROR;
    }

    if (Mint & SMHC_INT_DATA_TIMEOUT) {
        X |= SDPORT_ERROR_DATA_TIMEOUT;
    }

    if (Mint & SMHC_INT_DATA_CRC_ERROR) {
        X |= SDPORT_ERROR_DATA_CRC_ERROR;
    }

    if (Mint & SMHC_INT_END_BIT_ERROR) {
        X |= SDPORT_ERROR_DATA_END_BIT_ERROR;
    }

    if (Mint & (SMHC_INT_FIFO_RUN_ERROR |
                SMHC_INT_HARDWARE_LOCKED |
                SMHC_INT_START_BIT_ERROR)) {
        X |= SDPORT_GENERIC_IO_ERROR;
    }

    if (Idst & SMHC_IDST_ERRORS) {
        X |= SDPORT_ERROR_ADMA_ERROR;
    }

    if (X != 0) {
        E |= SDPORT_EVENT_ERROR;
    }

    *Events = E;
    *Errors = X;
}

//
// sdport event mask (ToggleEvents / ClearEvents) -> RINT/IMASK bits.
//
static __inline ULONG
SmhcEventsToIntMask(
    ULONG Events
    )
{
    ULONG M = 0;

    if (Events & SDPORT_EVENT_CARD_RESPONSE) {
        M |= SMHC_INT_COMMAND_DONE;
    }

    if (Events & SDPORT_EVENT_CARD_RW_END) {
        M |= SMHC_INT_DATA_OVER | SMHC_INT_AUTO_COMMAND_DONE;
    }

    if (Events & SDPORT_EVENT_BUFFER_EMPTY) {
        M |= SMHC_INT_TX_DATA_REQUEST;
    }

    if (Events & SDPORT_EVENT_BUFFER_FULL) {
        M |= SMHC_INT_RX_DATA_REQUEST;
    }

    if (Events & SDPORT_EVENT_ERROR) {
        M |= SMHC_INT_ERRORS;
    }

    return M;
}

//
// ---------------------------------------------------------------------------
// R2 formatting.
//
// sdport expects R2 the way an SDHCI host presents it: the 120 payload bits
// without the trailing CRC7 + end bit (the sample's SdhcGetResponse copies the
// SDHCI response registers verbatim).  The SMHC response registers hold the
// full 128 bits with CRC7|end at the bottom: [LX] sunxi_mmc_finalize_request
// stores RESP3..RESP0 as the raw 128-bit response and the MMC core decodes
// CSD/CID from it.  dwcmshc.cpp (MshcSlotGetResponse) drops that low byte by
// copying bytes [1..15]; the same is done here.  Output buffer: 16 bytes
// (byte 15 is zeroed).
// ---------------------------------------------------------------------------
//

static __inline VOID
SmhcFormatR2(
    const ULONG Resp[4],
    UCHAR *Out16
    )
{
    const UCHAR *Raw = (const UCHAR *)Resp;
    ULONG I;

    for (I = 0; I < 15; I += 1) {
        Out16[I] = Raw[I + 1];
    }

    Out16[15] = 0;
}

//
// ---------------------------------------------------------------------------
// IDMAC descriptor chain.
//
// One descriptor per (<= 64 KiB) piece of every scatter/gather element:
//   config   = CH | OWN | DIC          [LX] sunxi_mmc_init_idma_des
//   buf_size = length, 0 means 65536   [LX] "0 == max_len"
//   buf_addr = address >> 2
//   next     = address of the next descriptor >> 2
// first descriptor |= FD; last |= LD | ER, DIC cleared, next = 0.
//
// Addresses are limited to 32 bits: Linux uses the default 32-bit DMA mask for
// this controller (not stated in the sources for Windows; kept conservative) and
// the >> 2 shift (34-bit reach) is not relied upon.  4-byte alignment is
// mandatory because of the shift ([LX] sunxi_mmc_map_dma rejects sg->offset & 3
// and sg->length & 3).
// ---------------------------------------------------------------------------
//

typedef struct _SMHC_IDMAC_BUILDER {
    PSMHC_IDMAC_DESCRIPTOR Descriptors;     // virtual address of the table
    ULONG PhysicalBase;                     // physical address of the table (< 4 GiB)
    ULONG MaxDescriptors;
    ULONG Count;
    ULONG TotalBytes;
} SMHC_IDMAC_BUILDER, *PSMHC_IDMAC_BUILDER;

static __inline VOID
SmhcIdmacBuilderInit(
    PSMHC_IDMAC_BUILDER Builder,
    PSMHC_IDMAC_DESCRIPTOR Descriptors,
    ULONG PhysicalBase,
    ULONG MaxDescriptors
    )
{
    Builder->Descriptors = Descriptors;
    Builder->PhysicalBase = PhysicalBase;
    Builder->MaxDescriptors = MaxDescriptors;
    Builder->Count = 0;
    Builder->TotalBytes = 0;
}

static __inline NTSTATUS
SmhcIdmacBuilderAppend(
    PSMHC_IDMAC_BUILDER Builder,
    ULONGLONG Address,
    ULONG Length
    )
{
    ULONG Chunk;

    if ((Length == 0) || ((Address & 3) != 0) || ((Length & 3) != 0)) {
        return STATUS_INVALID_PARAMETER;
    }

    if ((Address + Length) > 0x100000000ULL) {
        return STATUS_INVALID_PARAMETER;      // above 4 GiB: not supported
    }

    while (Length > 0) {
        PSMHC_IDMAC_DESCRIPTOR D;

        if (Builder->Count >= Builder->MaxDescriptors) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        Chunk = (Length > SMHC_IDMAC_MAX_LEN_PER_DESC) ?
                    SMHC_IDMAC_MAX_LEN_PER_DESC : Length;

        D = &Builder->Descriptors[Builder->Count];
        D->Config = SMHC_IDMAC_DES_CH | SMHC_IDMAC_DES_OWN | SMHC_IDMAC_DES_DIC;
        D->BufSize = (Chunk == SMHC_IDMAC_MAX_LEN_PER_DESC) ? 0 : Chunk;
        D->BufAddr = (ULONG)(Address >> SMHC_IDMAC_DES_SHIFT);
        D->NextDesc = (ULONG)(((ULONGLONG)Builder->PhysicalBase +
                               (ULONGLONG)(Builder->Count + 1) *
                               sizeof(SMHC_IDMAC_DESCRIPTOR)) >>
                              SMHC_IDMAC_DES_SHIFT);

        Builder->Count += 1;
        Builder->TotalBytes += Chunk;
        Address += Chunk;
        Length -= Chunk;
    }

    return STATUS_SUCCESS;
}

static __inline NTSTATUS
SmhcIdmacBuilderFinish(
    PSMHC_IDMAC_BUILDER Builder
    )
{
    PSMHC_IDMAC_DESCRIPTOR First;
    PSMHC_IDMAC_DESCRIPTOR Last;

    if (Builder->Count == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    First = &Builder->Descriptors[0];
    Last = &Builder->Descriptors[Builder->Count - 1];

    First->Config |= SMHC_IDMAC_DES_FD;
    Last->Config |= SMHC_IDMAC_DES_LD | SMHC_IDMAC_DES_ER;
    Last->Config &= ~SMHC_IDMAC_DES_DIC;
    Last->NextDesc = 0;
    return STATUS_SUCCESS;
}
