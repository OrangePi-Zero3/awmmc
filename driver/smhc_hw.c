/*++

Module Name:

    smhc_hw.c

Abstract:

    Hardware layer of the AWMC0001 SMHC miniport: controller reset, clock,
    bus width, command issue, PIO/FIFO and IDMAC data paths, request completion.

    Register sequences are taken from the references named in smhc_regs.h
    ([UB-C] u-boot sunxi_mmc.c, [LX] Linux sunxi-mmc.c).  The sdport request/event
    choreography follows the Microsoft sample (sdhc.c) and dwcmshc.cpp.

--*/

#include "smhc.h"

//
// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------
//

//
// Poll (Register & Mask) == 0.  Returns STATUS_IO_TIMEOUT if it stays set.
//
static NTSTATUS
SmhcWaitClear(
    _In_ PSMHC_EXTENSION Ext,
    _In_ ULONG Register,
    _In_ ULONG Mask,
    _In_ ULONG TimeoutUs
    )
{
    ULONG Elapsed;

    for (Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 1) {
        if ((SmhcRead(Ext, Register) & Mask) == 0) {
            return STATUS_SUCCESS;
        }

        SdPortWait(1);
    }

    return ((SmhcRead(Ext, Register) & Mask) == 0) ? STATUS_SUCCESS :
                                                      STATUS_IO_TIMEOUT;
}

static __inline BOOLEAN
SmhcHasData(
    _In_ PSDPORT_COMMAND Command
    )
{
    return (Command->TransferType != SdTransferTypeNone) &&
           (Command->TransferType != SdTransferTypeUndefined);
}

//
// IMASK read-modify-write that tolerates concurrent callers (ISR at DIRQL,
// ToggleEvents, request issue).  The shadow is updated atomically and the register
// is rewritten until it matches the shadow, so the last writer always wins with the
// newest value.
//
VOID
SmhcImaskUpdate(
    _In_ PSMHC_EXTENSION Ext,
    _In_ ULONG SetBits,
    _In_ ULONG ClearBits
    )
{
    LONG Value;

    if (ClearBits != 0) {
        InterlockedAnd(&Ext->ImaskShadow, (LONG)~ClearBits);
    }

    if (SetBits != 0) {
        InterlockedOr(&Ext->ImaskShadow, (LONG)SetBits);
    }

    do {
        Value = Ext->ImaskShadow;
        SmhcWrite(Ext, SMHC_REG_IMASK, (ULONG)Value);
    } while (Value != Ext->ImaskShadow);
}

//
// ---------------------------------------------------------------------------
// Configuration (device registry key, written by the INF).
// ---------------------------------------------------------------------------
//

static NTSTATUS
SmhcRegQueryUlong(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _Out_ PULONG Value
    )
{
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info;
    UNICODE_STRING ValueName;
    ULONG ResultLength;
    NTSTATUS Status;

    RtlInitUnicodeString(&ValueName, Name);
    Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    Status = ZwQueryValueKey(Key,
                             &ValueName,
                             KeyValuePartialInformation,
                             Info,
                             sizeof(Buffer),
                             &ResultLength);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    if ((Info->Type != REG_DWORD) || (Info->DataLength != sizeof(ULONG))) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    *Value = *(PULONG)Info->Data;
    return STATUS_SUCCESS;
}

VOID
SmhcLoadConfiguration(
    _Inout_ PSMHC_EXTENSION Ext
    )
{
    HANDLE Key;
    PDEVICE_OBJECT Pdo;
    ULONG Value;
    NTSTATUS Status;

    Ext->ModuleClockHz = SMHC_DEFAULT_MODULE_CLOCK_HZ;
    Ext->MaxFrequencyKhz = SMHC_DEFAULT_MAX_FREQUENCY_KHZ;
    Ext->TransferMode = SMHC_DEFAULT_TRANSFER_MODE;

    if (Ext->CrashdumpMode || (Ext->Fdo == NULL)) {
        return;
    }

    Pdo = Ext->Fdo->DeviceObjectExtension->AttachedTo;
    if (Pdo == NULL) {
        return;
    }

    Key = NULL;
    Status = IoOpenDeviceRegistryKey(Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &Key);
    if (!NT_SUCCESS(Status)) {
        SMHC_LOG_WARN("no device registry key (0x%08X), using defaults\n", Status);
        return;
    }

    if (NT_SUCCESS(SmhcRegQueryUlong(Key, L"ModuleClockHz", &Value)) && (Value != 0)) {
        Ext->ModuleClockHz = Value;
    }

    if (NT_SUCCESS(SmhcRegQueryUlong(Key, L"MaxFrequencyKhz", &Value)) && (Value != 0)) {
        Ext->MaxFrequencyKhz = Value;
    }

    if (NT_SUCCESS(SmhcRegQueryUlong(Key, L"TransferMode", &Value)) &&
        (Value <= SMHC_TRANSFER_MODE_DMA)) {

        Ext->TransferMode = Value;
    }

    ZwClose(Key);
}

//
// ---------------------------------------------------------------------------
// Controller reset and register restore.
// ---------------------------------------------------------------------------
//

//
// Reset the FIFO.  [UB-C] sunxi_mmc_send_cmd_common ends every command with
// GCTRL |= FIFO_RESET.
//
static NTSTATUS
SmhcResetFifo(
    _In_ PSMHC_EXTENSION Ext
    )
{
    SmhcWrite(Ext, SMHC_REG_GCTRL, SmhcRead(Ext, SMHC_REG_GCTRL) | SMHC_GCTRL_FIFO_RESET);
    return SmhcWaitClear(Ext, SMHC_REG_GCTRL, SMHC_GCTRL_FIFO_RESET, 1000);
}

//
// Latch CLKCR into the card clock domain.  [LX] sunxi_mmc_oclk_onoff and [UB-C]
// mmc_update_clk: START | UPCLK_ONLY | WAIT_PRE_OVER, then wait for START to clear.
//
static NTSTATUS
SmhcUpdateClock(
    _In_ PSMHC_EXTENSION Ext
    )
{
    NTSTATUS Status;
    ULONG Rint;

    SmhcWrite(Ext,
              SMHC_REG_CMD,
              SMHC_CMD_START | SMHC_CMD_UPCLK_ONLY | SMHC_CMD_WAIT_PRE_OVER);

    Status = SmhcWaitClear(Ext,
                           SMHC_REG_CMD,
                           SMHC_CMD_START,
                           SMHC_CLOCK_UPDATE_TIMEOUT_US);

    //
    // The clock update sets status bits; clear them (but leave SDIO alone, as
    // [LX] does).
    //
    Rint = SmhcRead(Ext, SMHC_REG_RINT);
    SmhcWrite(Ext, SMHC_REG_RINT, Rint & ~SMHC_INT_SDIO);

    if (!NT_SUCCESS(Status)) {
        SMHC_LOG_ERROR("clock update timed out\n");
    }

    return Status;
}

//
// Gate the card clock on or off.  Mirrors [LX] sunxi_mmc_oclk_onoff exactly,
// including MASK_DATA0 which the H616 config requires (cfg.mask_data0 = true).
//
static NTSTATUS
SmhcCardClock(
    _In_ PSMHC_EXTENSION Ext,
    _In_ BOOLEAN Enable
    )
{
    NTSTATUS Status;
    ULONG Clkcr;

    Clkcr = (Ext->Clkcr & ~(SMHC_CLKCR_CARD_CLOCK_ON |
                            SMHC_CLKCR_LOW_POWER_ON |
                            SMHC_CLKCR_MASK_DATA0));

    if (Enable) {
        Clkcr |= SMHC_CLKCR_CARD_CLOCK_ON;
    }

    SmhcWrite(Ext, SMHC_REG_CLKCR, Clkcr | SMHC_CLKCR_MASK_DATA0);
    Status = SmhcUpdateClock(Ext);
    SmhcWrite(Ext, SMHC_REG_CLKCR, Clkcr);

    Ext->ClockEnabled = Enable && NT_SUCCESS(Status);
    return Status;
}

//
// Re-apply everything the driver owns after a soft reset.  Whether GCTRL.SOFT_RESET
// clears CLKCR / WIDTH / NTSR is not stated by the references ([UB-C] only calls
// mmc_update_clk after an error reset), so everything is rewritten defensively.
//
VOID
SmhcRestoreRegisters(
    _In_ PSMHC_EXTENSION Ext
    )
{
    ULONG Gctrl;

    //
    // [UB-C] sunxi_mmc_reset (H6/H616): "Setup FIFO R/W threshold. Needed on H616."
    //
    SmhcWrite(Ext, SMHC_REG_THLDC, SMHC_THLDC_DEFAULT);

    //
    // [LX] sunxi_mmc_init_host.
    //
    SmhcWrite(Ext, SMHC_REG_FTRGL, SMHC_FTRGL_DEFAULT);
    SmhcWrite(Ext, SMHC_REG_TMOUT, SMHC_TMOUT_MAX);
    SmhcWrite(Ext, SMHC_REG_IMASK, (ULONG)Ext->ImaskShadow);   // sdport's enables survive a reset
    SmhcWrite(Ext, SMHC_REG_IDIE, 0);
    SmhcWrite(Ext, SMHC_REG_RINT, SMHC_INT_ALL);
    SmhcWrite(Ext, SMHC_REG_IDST, SMHC_IDST_CLEAR_ALL);

    Gctrl = SmhcRead(Ext, SMHC_REG_GCTRL);
    Gctrl |= SMHC_GCTRL_INT_ENABLE;
    Gctrl &= ~(SMHC_GCTRL_ACCESS_DONE_DIRECT |      // [LX] "Undocumented, but found in Allwinner code"
               SMHC_GCTRL_DDR_MODE |
               SMHC_GCTRL_DMA_ENABLE |
               SMHC_GCTRL_ACCESS_BY_AHB);
    SmhcWrite(Ext, SMHC_REG_GCTRL, Gctrl);

    //
    // New timing mode.  [LX] sunxi_mmc_clk_set_rate sets NTSR bit 31 when
    // use_new_timings, which sun50i_h616_cfg.needs_new_timings forces; [LX]: "Don't
    // touch the delay bits".  ([UB-C] mmc_set_mod_clk does the same only under
    // CONFIG_MMC_SUNXI_HAS_NEW_MODE, which the fetched files do not show for H616.)
    //
    SmhcWrite(Ext,
              SMHC_REG_NTSR,
              SmhcRead(Ext, SMHC_REG_NTSR) | SMHC_NTSR_MODE_SEL_NEW);

    //
    // Sample delay: software delay enabled, value 0 ([LX] sunxi_mmc_calibrate).
    //
    SmhcWrite(Ext, SMHC_REG_SAMP_DL, SMHC_SAMP_DL_SW_EN);

    SmhcWrite(Ext, SMHC_REG_WIDTH, Ext->BusWidthReg);
    SmhcWrite(Ext, SMHC_REG_CLKCR, Ext->Clkcr);
}

static NTSTATUS
SmhcSoftReset(
    _In_ PSMHC_EXTENSION Ext
    )
{
    NTSTATUS Status;

    //
    // [LX] sunxi_mmc_reset_host: write the three reset bits, poll until clear
    // (250 ms).  [UB-C] sunxi_mmc_reset just waits 1 ms.
    //
    SmhcWrite(Ext, SMHC_REG_GCTRL, SMHC_GCTRL_ALL_RESET);
    Status = SmhcWaitClear(Ext,
                           SMHC_REG_GCTRL,
                           SMHC_GCTRL_ALL_RESET,
                           SMHC_RESET_TIMEOUT_US);

    if (!NT_SUCCESS(Status)) {
        SMHC_LOG_ERROR("controller reset timed out, GCTRL=0x%08X\n",
                       SmhcRead(Ext, SMHC_REG_GCTRL));

        return Status;
    }

    SmhcRestoreRegisters(Ext);

    //
    // Re-latch the clock registers if the card clock was running.
    //
    if (Ext->ClockEnabled) {
        SmhcCardClock(Ext, TRUE);
    }

    return STATUS_SUCCESS;
}

//
// Stop and clean up an IDMAC / FIFO transfer.
//   [LX] sunxi_mmc_finalize_request: IDST = 0x337; DMAC = 0; GCTRL |= DMA_RESET;
//   GCTRL &= ~DMA_ENABLE; GCTRL |= FIFO_RESET.
//
NTSTATUS
SmhcStopDmaAndFifo(
    _In_ PSMHC_EXTENSION Ext
    )
{
    ULONG Gctrl;

    SmhcWrite(Ext, SMHC_REG_IDIE, 0);
    SmhcWrite(Ext, SMHC_REG_IDST, SMHC_IDST_CLEAR_ALL);
    SmhcWrite(Ext, SMHC_REG_DMAC, 0);

    Gctrl = SmhcRead(Ext, SMHC_REG_GCTRL);
    SmhcWrite(Ext, SMHC_REG_GCTRL, Gctrl | SMHC_GCTRL_DMA_RESET);
    Gctrl &= ~(SMHC_GCTRL_DMA_ENABLE | SMHC_GCTRL_ACCESS_BY_AHB);
    SmhcWrite(Ext, SMHC_REG_GCTRL, Gctrl);
    Ext->DmaActive = FALSE;

    return SmhcResetFifo(Ext);
}

//
// CMD12 sent directly (polled), used to end a data transfer that failed.
//   [LX] sunxi_mmc_send_manual_stop: START | RESP_EXPIRE | STOP_ABORT_CMD |
//   CHECK_RESPONSE_CRC | 12, argument 0, poll RINT for COMMAND_DONE or an error.
//
NTSTATUS
SmhcSendStopCommandPolled(
    _In_ PSMHC_EXTENSION Ext
    )
{
    ULONG Rint;
    ULONG Elapsed;
    NTSTATUS Status;

    //
    // sdport has already re-enabled interrupts when it calls SdResetTypeDat; with
    // COMMAND_DONE unmasked the ISR would acknowledge the very bit polled below.
    // Mask everything at the register (the shadow keeps sdport's enables and
    // SmhcSoftReset writes it back afterwards).
    //
    SmhcWrite(Ext, SMHC_REG_IMASK, 0);
    SmhcWrite(Ext, SMHC_REG_RINT, SMHC_INT_ALL);
    SmhcWrite(Ext, SMHC_REG_ARG, 0);
    SmhcWrite(Ext,
              SMHC_REG_CMD,
              SMHC_CMD_START | SMHC_CMD_RESP_EXPIRE | SMHC_CMD_STOP_ABORT |
              SMHC_CMD_CHK_RESPONSE_CRC | SMHC_SDCMD_STOP_TRANSMISSION);

    Status = STATUS_IO_TIMEOUT;
    for (Elapsed = 0; Elapsed < SMHC_STOP_TIMEOUT_US; Elapsed += 10) {
        Rint = SmhcRead(Ext, SMHC_REG_RINT);
        if (Rint & (SMHC_INT_COMMAND_DONE | SMHC_INT_ERRORS)) {
            Status = (Rint & SMHC_INT_ERRORS) ? STATUS_IO_DEVICE_ERROR :
                                                STATUS_SUCCESS;
            break;
        }

        SdPortWait(10);
    }

    SmhcWrite(Ext, SMHC_REG_RINT, SMHC_INT_ALL);
    SmhcWrite(Ext, SMHC_REG_IMASK, (ULONG)Ext->ImaskShadow);
    return Status;
}

NTSTATUS
SmhcResetHost(
    _In_ PSMHC_EXTENSION Ext,
    _In_ SDPORT_RESET_TYPE ResetType
    )
{
    PSDPORT_REQUEST Request;
    BOOLEAN FailedData;
    NTSTATUS Status;

    //
    // Abort whatever was outstanding.  Completed requests have already cleared
    // the pointer; a failed one is still here (SmhcCompleteRequest keeps it).
    //
    Request = (PSDPORT_REQUEST)InterlockedExchangePointer(
                  (PVOID volatile *)&Ext->OutstandingRequest, NULL);

    FailedData = (Request != NULL) && SmhcHasData(&Request->Command);

    switch (ResetType) {
    case SdResetTypeAll:
        break;

    case SdResetTypeCmd:
        //
        // As in dwcmshc.cpp: SdResetTypeCmd arrives with interrupts masked; if a
        // data transfer failed, sdport follows with SdResetTypeDat and the full
        // sequence runs there.
        //
        if (FailedData) {
            return STATUS_SUCCESS;
        }

        break;

    case SdResetTypeDat:
        //
        // Take the card out of the data state.  [LX] sends CMD12 by hand after
        // every failed data request.  Harmless if the card is not in a transfer.
        // NeedStop (not just this call's Request) because the preceding
        // SdResetTypeCmd already took the outstanding request pointer.
        //
        if (InterlockedExchange(&Ext->NeedStop, 0) && !Ext->CrashdumpMode) {
            SmhcSendStopCommandPolled(Ext);
        }

        break;

    default:
        return STATUS_INVALID_PARAMETER;
    }

    Ext->AutoStopActive = FALSE;
    Ext->PioWordsRemaining = 0;
    InterlockedExchange(&Ext->CurrentEvents, 0);
    InterlockedExchange(&Ext->CurrentErrors, 0);

    Status = SmhcSoftReset(Ext);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Ext->DmaActive = FALSE;

    if (ResetType == SdResetTypeAll) {
        InterlockedExchange(&Ext->NeedStop, 0);

        //
        // Power and signaling voltage are owned by firmware/PMIC and cannot be
        // cycled from here; clock off, 1-bit bus, everything masked.
        //
        SmhcSetClock(Ext, 0);
        Ext->BusWidthReg = SMHC_WIDTH_1BIT;
        SmhcWrite(Ext, SMHC_REG_WIDTH, Ext->BusWidthReg);
        SmhcImaskUpdate(Ext, 0, 0xFFFFFFFFUL);      // sdport re-enables through ToggleEvents
    }

    SmhcWrite(Ext, SMHC_REG_RINT, SMHC_INT_ALL);
    return STATUS_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// Clock and bus width.
// ---------------------------------------------------------------------------
//

NTSTATUS
SmhcSetClock(
    _In_ PSMHC_EXTENSION Ext,
    _In_ ULONG FrequencyKhz
    )
{
    NTSTATUS Status;
    ULONG TargetHz;
    ULONG ActualHz;
    ULONG Field;

    //
    // [LX] sunxi_mmc_clk_set_rate: gate the card clock, set the divider, set new
    // timing mode, calibrate (sample delay 0), ungate.
    //
    Status = SmhcCardClock(Ext, FALSE);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    if (FrequencyKhz == 0) {
        Ext->BusFrequencyKhz = 0;
        return STATUS_SUCCESS;
    }

    if (FrequencyKhz > Ext->MaxFrequencyKhz) {
        FrequencyKhz = Ext->MaxFrequencyKhz;
    }

    TargetHz = FrequencyKhz * 1000;
    Field = SmhcCalcClkcrDivider(Ext->ModuleClockHz, TargetHz, &ActualHz);

    Ext->Clkcr = (Ext->Clkcr & ~SMHC_CLKCR_DIVIDER_MASK) | Field;
    SmhcWrite(Ext, SMHC_REG_CLKCR, Ext->Clkcr);

    SmhcWrite(Ext,
              SMHC_REG_NTSR,
              SmhcRead(Ext, SMHC_REG_NTSR) | SMHC_NTSR_MODE_SEL_NEW);

    SmhcWrite(Ext, SMHC_REG_SAMP_DL, SMHC_SAMP_DL_SW_EN);

    Status = SmhcCardClock(Ext, TRUE);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Ext->BusFrequencyKhz = ActualHz / 1000;

    SMHC_LOG_INFO("clock: requested %lu kHz, module %lu Hz, CLKCR div field %lu, ~%lu kHz\n",
                  FrequencyKhz,
                  Ext->ModuleClockHz,
                  Field,
                  Ext->BusFrequencyKhz);

    //
    // sdhc.c SdhcSetClock: let the clock stabilize.
    //
    SdPortWait(SMHC_REMOVABLE_SETTLE_US);
    return STATUS_SUCCESS;
}

NTSTATUS
SmhcSetBusWidth(
    _In_ PSMHC_EXTENSION Ext,
    _In_ SDPORT_BUS_WIDTH Width
    )
{
    switch (Width) {
    case SdBusWidth1Bit:
        Ext->BusWidthReg = SMHC_WIDTH_1BIT;
        break;

    case SdBusWidth4Bit:
        Ext->BusWidthReg = SMHC_WIDTH_4BIT;
        break;

    case SdBusWidth8Bit:
        return STATUS_NOT_SUPPORTED;            // SMHC0 only wires D0-D3 (PF0-PF5)

    default:
        return STATUS_INVALID_PARAMETER;
    }

    SmhcWrite(Ext, SMHC_REG_WIDTH, Ext->BusWidthReg);
    return STATUS_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// Data transfer set-up.
// ---------------------------------------------------------------------------
//

static __inline ULONG
SmhcMin(
    _In_ ULONG A,
    _In_ ULONG B
    )
{
    return (A < B) ? A : B;
}

static VOID
SmhcProgramPioThreshold(
    _In_ PSMHC_EXTENSION Ext
    )
{
    ULONG Threshold;

    //
    // Data-request interrupts fire when the FIFO holds more than RX_TL (read) or
    // at most TX_TL (write) words; the threshold must never exceed what is still
    // to come or the last interrupt would never be raised (dwcmshc.cpp
    // MshcBuildPioTransfer: "so we still get a Data Request interrupt").
    //
    Threshold = SmhcMin(Ext->PioWordsRemaining, 8);
    if (Threshold == 0) {
        Threshold = 1;
    }

    Ext->PioThreshold = Threshold;
    SmhcWrite(Ext, SMHC_REG_FTRGL, SMHC_FTRGL_VALUE(Threshold - 1, Threshold));
}

static NTSTATUS
SmhcBuildPioTransfer(
    _In_ PSMHC_EXTENSION Ext,
    _In_ PSDPORT_REQUEST Request
    )
{
    PSDPORT_COMMAND Command;
    NTSTATUS Status;
    ULONG Gctrl;

    Command = &Request->Command;

    if ((Command->Length & 3) != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    Status = SmhcResetFifo(Ext);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    //
    // CPU access to the FIFO: [UB-C] mmc_trans_data_by_cpu sets
    // GCTRL |= ACCESS_BY_AHB.  Linux never uses PIO; the DMA_ENABLE/ACCESS_BY_AHB
    // interplay when mixing the two is UNVERIFIED.
    //
    Gctrl = SmhcRead(Ext, SMHC_REG_GCTRL);
    Gctrl &= ~SMHC_GCTRL_DMA_ENABLE;
    Gctrl |= SMHC_GCTRL_ACCESS_BY_AHB;
    SmhcWrite(Ext, SMHC_REG_GCTRL, Gctrl);

    Ext->DmaActive = FALSE;
    Ext->PioWordsRemaining = Command->Length / sizeof(ULONG);
    SmhcProgramPioThreshold(Ext);

    SmhcWrite(Ext, SMHC_REG_BLKSZ, Command->BlockSize);
    SmhcWrite(Ext, SMHC_REG_BCNTR, Command->BlockSize * Command->BlockCount);
    return STATUS_SUCCESS;
}

static NTSTATUS
SmhcBuildDmaTransfer(
    _In_ PSMHC_EXTENSION Ext,
    _In_ PSDPORT_REQUEST Request
    )
{
    PSDPORT_COMMAND Command;
    SMHC_IDMAC_BUILDER Builder;
    PSCATTER_GATHER_LIST SgList;
    ULONG Index;
    ULONG MaxDescriptors;
    ULONG Gctrl;
    NTSTATUS Status;

    Command = &Request->Command;
    SgList = Command->ScatterGatherList;

    if ((SgList == NULL) || (SgList->NumberOfElements == 0) ||
        (Command->DmaVirtualAddress == NULL)) {

        return STATUS_INVALID_PARAMETER;
    }

    if (Command->DmaPhysicalAddress.HighPart != 0) {
        SMHC_LOG_ERROR("descriptor table above 4 GiB (0x%llX)\n",
                       (ULONGLONG)Command->DmaPhysicalAddress.QuadPart);

        return STATUS_INVALID_PARAMETER;
    }

    //
    // Upper bound on the descriptors this request needs: one per scatter/gather
    // element plus one per additional 64 KiB split.  UNVERIFIED: how large the
    // descriptor memory sdport hands us is (it is derived from
    // Capabilities.DmaDescriptorSize and is not documented in the sample); the
    // builder only guards against exceeding this computed bound.
    //
    MaxDescriptors = SgList->NumberOfElements;
    for (Index = 0; Index < SgList->NumberOfElements; Index += 1) {
        MaxDescriptors += SgList->Elements[Index].Length / SMHC_IDMAC_MAX_LEN_PER_DESC;
    }

    SmhcIdmacBuilderInit(&Builder,
                         (PSMHC_IDMAC_DESCRIPTOR)Command->DmaVirtualAddress,
                         Command->DmaPhysicalAddress.LowPart,
                         MaxDescriptors);

    for (Index = 0; Index < SgList->NumberOfElements; Index += 1) {
        Status = SmhcIdmacBuilderAppend(&Builder,
                                        (ULONGLONG)SgList->Elements[Index].Address.QuadPart,
                                        SgList->Elements[Index].Length);

        if (!NT_SUCCESS(Status)) {
            SMHC_LOG_ERROR("bad SG element %lu (addr 0x%llX len %lu): 0x%08X\n",
                           Index,
                           (ULONGLONG)SgList->Elements[Index].Address.QuadPart,
                           SgList->Elements[Index].Length,
                           Status);

            return Status;
        }
    }

    Status = SmhcIdmacBuilderFinish(&Builder);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    if (Builder.TotalBytes != (Command->BlockSize * Command->BlockCount)) {
        SMHC_LOG_WARN("SG total %lu != block bytes %lu\n",
                      Builder.TotalBytes,
                      Command->BlockSize * Command->BlockCount);
    }

    Status = SmhcResetFifo(Ext);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    SmhcWrite(Ext, SMHC_REG_FTRGL, SMHC_FTRGL_DEFAULT);
    SmhcWrite(Ext, SMHC_REG_BLKSZ, Command->BlockSize);
    SmhcWrite(Ext, SMHC_REG_BCNTR, Command->BlockSize * Command->BlockCount);

    //
    // Descriptors must be visible to the IDMAC before it is started.
    //
    SmhcDmaBarrier();

    //
    // [LX] sunxi_mmc_start_dma:
    //   GCTRL |= DMA_ENABLE; GCTRL |= DMA_RESET; DMAC = SOFT_RESET;
    //   (reads) IDIE = RECEIVE; DMAC = FIX_BURST | IDMA_ON.
    // DLBA = descriptor list address >> idma_des_shift  (sunxi_mmc_init_host).
    //
    Gctrl = SmhcRead(Ext, SMHC_REG_GCTRL);
    Gctrl |= SMHC_GCTRL_DMA_ENABLE;
    Gctrl &= ~SMHC_GCTRL_ACCESS_BY_AHB;
    SmhcWrite(Ext, SMHC_REG_GCTRL, Gctrl);
    SmhcWrite(Ext, SMHC_REG_GCTRL, Gctrl | SMHC_GCTRL_DMA_RESET);

    SmhcWrite(Ext, SMHC_REG_DMAC, SMHC_DMAC_SOFT_RESET);

    //
    // DLBA is written after the IDMAC soft reset: Linux writes it once at init and
    // resets the IDMAC per transfer, but whether the reset clears DLBA is not
    // stated, so ordering it after the reset is safe either way.
    //
    SmhcWrite(Ext, SMHC_REG_DLBA, Command->DmaPhysicalAddress.LowPart >> SMHC_IDMAC_DES_SHIFT);
    SmhcWrite(Ext, SMHC_REG_IDST, SMHC_IDST_CLEAR_ALL);

    if (Command->TransferDirection == SdTransferDirectionRead) {
        SmhcWrite(Ext, SMHC_REG_IDIE, SMHC_IDIE_RX_INTERRUPT);
    } else {
        SmhcWrite(Ext, SMHC_REG_IDIE, 0);
    }

    SmhcWrite(Ext, SMHC_REG_DMAC, SMHC_DMAC_FIX_BURST | SMHC_DMAC_IDMA_ON);
    Ext->DmaActive = TRUE;
    return STATUS_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// Command issue.
// ---------------------------------------------------------------------------
//

NTSTATUS
SmhcSendCommand(
    _In_ PSMHC_EXTENSION Ext,
    _In_ PSDPORT_REQUEST Request
    )
{
    PSDPORT_COMMAND Command;
    ULONG Cmd;
    ULONG Imask;
    BOOLEAN HasData;
    NTSTATUS Status;

    Command = &Request->Command;
    HasData = SmhcHasData(Command);

    //
    // The previous command must have been accepted by the controller; writing CMD
    // while START is still set raises the hardware-locked error.
    //
    Status = SmhcWaitClear(Ext, SMHC_REG_CMD, SMHC_CMD_START, 1000);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Ext->AutoStopActive = FALSE;
    InterlockedExchange(&Ext->CurrentEvents, 0);
    InterlockedExchange(&Ext->CurrentErrors, 0);
    InterlockedExchange(&Ext->PhaseClaim, 0);

    if (HasData) {
        if (Command->TransferMethod == SdTransferMethodSgDma) {
            Status = SmhcBuildDmaTransfer(Ext, Request);

        } else if (Command->TransferMethod == SdTransferMethodPio) {
            Status = SmhcBuildPioTransfer(Ext, Request);

        } else {
            Status = STATUS_NOT_SUPPORTED;
        }

        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    //
    // Command word.  [UB-C] sunxi_mmc_send_cmd_common, [LX] sunxi_mmc_request.
    //
    Cmd = SMHC_CMD_START | (Command->Index & SMHC_CMD_INDEX_MASK);

    switch (Command->ResponseType) {
    case SdResponseTypeNone:
        break;

    case SdResponseTypeR1:
    case SdResponseTypeR1B:
    case SdResponseTypeR5:
    case SdResponseTypeR5B:
    case SdResponseTypeR6:
        Cmd |= SMHC_CMD_RESP_EXPIRE | SMHC_CMD_CHK_RESPONSE_CRC;
        break;

    case SdResponseTypeR2:
        Cmd |= SMHC_CMD_RESP_EXPIRE | SMHC_CMD_LONG_RESPONSE | SMHC_CMD_CHK_RESPONSE_CRC;
        break;

    case SdResponseTypeR3:
    case SdResponseTypeR4:
        Cmd |= SMHC_CMD_RESP_EXPIRE;
        break;

    default:
        return STATUS_INVALID_PARAMETER;
    }

    if (Command->Index == SMHC_SDCMD_GO_IDLE_STATE) {
        Cmd |= SMHC_CMD_SEND_INIT_SEQ;                      // [UB-C]/[LX]: CMD0 only
    }

    if (Command->Index == SMHC_SDCMD_STOP_TRANSMISSION) {
        Cmd |= SMHC_CMD_STOP_ABORT;                         // [LX] manual stop
    }

    if (HasData) {
        Cmd |= SMHC_CMD_DATA_EXPIRE | SMHC_CMD_WAIT_PRE_OVER;

        if (Command->TransferDirection == SdTransferDirectionWrite) {
            Cmd |= SMHC_CMD_WRITE;
        }

        if (Command->UseAutoCmd12 && (Command->TransferType == SdTransferTypeMultiBlock)) {
            Cmd |= SMHC_CMD_AUTO_STOP;
            Ext->AutoStopActive = TRUE;
        }
    }

    //
    // Events sdport must see before the request is complete.
    //
    Request->RequiredEvents = SDPORT_EVENT_CARD_RESPONSE;

    if (HasData) {
        if (Command->TransferMethod == SdTransferMethodSgDma) {
            Request->RequiredEvents |= SDPORT_EVENT_CARD_RW_END;
            if (Command->TransferDirection == SdTransferDirectionRead) {
                Request->RequiredEvents |= SDPORT_EVENT_DMA_COMPLETE;   // IDMAC drained the FIFO
            }

        } else {
            Request->RequiredEvents |= (Command->TransferDirection == SdTransferDirectionRead) ?
                                           SDPORT_EVENT_BUFFER_FULL :
                                           SDPORT_EVENT_BUFFER_EMPTY;
        }
    }

    //
    // Interrupt enables: sdport drives them through ToggleEvents, but the data
    // request interrupts are masked by the ISR and re-armed per chunk, and the IDMAC
    // completion is ours alone; make sure the ones this request depends on are on.
    //
    Imask = SMHC_INT_ERRORS | SMHC_INT_COMMAND_DONE;
    if (HasData) {
        Imask |= Ext->AutoStopActive ? SMHC_INT_AUTO_COMMAND_DONE : SMHC_INT_DATA_OVER;
        if (Command->TransferMethod == SdTransferMethodPio) {
            Imask |= (Command->TransferDirection == SdTransferDirectionRead) ?
                         SMHC_INT_RX_DATA_REQUEST : SMHC_INT_TX_DATA_REQUEST;
        }
    }

    SmhcWrite(Ext, SMHC_REG_RINT, SMHC_INT_ALL);
    SmhcImaskUpdate(Ext, Imask, 0);

    SMHC_LOG_TRACE("CMD%lu arg 0x%08X cmd 0x%08X %s%s req-events 0x%X\n",
                   Command->Index,
                   Command->Argument,
                   Cmd,
                   HasData ? (Command->TransferMethod == SdTransferMethodSgDma ? "DMA " : "PIO ") : "",
                   HasData ? (Command->TransferDirection == SdTransferDirectionRead ? "R" : "W") : "",
                   Request->RequiredEvents);

    SmhcWrite(Ext, SMHC_REG_ARG, Command->Argument);
    SmhcWrite(Ext, SMHC_REG_CMD, Cmd);

    return STATUS_PENDING;
}

//
// PIO data phase, called by sdport (SdRequestTypeStartTransfer) after each
// BUFFER_FULL / BUFFER_EMPTY event.  Mirrors the sample's SdhcStartPioTransfer
// (consume one chunk, extend RequiredEvents, STATUS_MORE_PROCESSING_REQUIRED
// until the last chunk) with the u-boot FIFO-status handling.
//   [UB-C] mmc_trans_data_by_cpu: reads use the FIFO level in STATUS[30:17]; writes
//   check FIFO_FULL before every word because the FIFO depth is not known.
//
static NTSTATUS
SmhcStartPioTransfer(
    _In_ PSMHC_EXTENSION Ext,
    _In_ PSDPORT_REQUEST Request
    )
{
    PSDPORT_COMMAND Command;
    PULONG Buffer;
    ULONG Words;
    ULONG Status;
    ULONG Done;
    ULONG CurrentEvents;
    ULONG CurrentErrors;
    BOOLEAN Read;

    Command = &Request->Command;
    Read = (Command->TransferDirection == SdTransferDirectionRead);
    Buffer = (PULONG)Command->DataBuffer;

    //
    // An error that arrived between phases was recorded by the DPC but could not
    // complete the request (nothing was being waited for): fail it now rather than
    // wait for events that will never come.
    //
    CurrentErrors = (ULONG)InterlockedCompareExchange(&Ext->CurrentErrors, 0, 0);
    if (CurrentErrors != 0) {
        InterlockedExchange(&Ext->CurrentErrors, 0);
        Request->RequiredEvents = 0;
        InterlockedExchange(&Ext->PhaseClaim, 1);
        SmhcCompleteRequest(Ext, Request, SmhcConvertErrorToStatus(CurrentErrors));
        return STATUS_PENDING;
    }

    InterlockedExchange(&Ext->PhaseClaim, 0);

    //
    // Events the DPC has already seen for this command.  Not cleared here: DATA_OVER
    // can arrive while the FIFO is still being drained over several phases and must
    // still be known at the last one (cleared when the next command is issued).
    //
    CurrentEvents = (ULONG)InterlockedCompareExchange(&Ext->CurrentEvents, 0, 0);

    //
    // Move everything the FIFO can take / give right now, not just one watermark's
    // worth: whether a data-request interrupt re-asserts after being cleared while
    // its condition still holds is not established for the SMHC (dw_mmc relies on it
    // with a re-check loop), so never leave serviceable data behind.  The next
    // interrupt then always follows a fresh watermark crossing.
    //
    Done = 0;

    if (Read) {
        while (Ext->PioWordsRemaining > Done) {
            Status = SmhcRead(Ext, SMHC_REG_STATUS);
            Words = SMHC_STATUS_FIFO_LEVEL(Status);

            //
            // A completely full FIFO can report level 0 ([UB-C]: "Some SoCs (A20)
            // report a level of 0 if the FIFO is completely full"; u-boot then
            // assumes 32 words).  Assume 16, the smallest depth ([LX] SDXC_FIFO_SIZE).
            //
            if ((Words == 0) && (Status & SMHC_STATUS_FIFO_FULL)) {
                Words = 16;
            }

            Words = SmhcMin(Words, Ext->PioWordsRemaining - Done);
            if (Words == 0) {
                break;
            }

            while (Words > 0) {
                Buffer[Done] = SmhcRead(Ext, SMHC_REG_FIFO);
                Done += 1;
                Words -= 1;
            }
        }

    } else {
        while ((Ext->PioWordsRemaining > Done) &&
               ((SmhcRead(Ext, SMHC_REG_STATUS) & SMHC_STATUS_FIFO_FULL) == 0)) {

            SmhcWrite(Ext, SMHC_REG_FIFO, Buffer[Done]);
            Done += 1;
        }
    }

    Ext->PioWordsRemaining -= Done;
    Command->DataBuffer += Done * sizeof(ULONG);

    //
    // Data-request interrupts can only be cleared once the FIFO has been serviced.
    //
    SmhcWrite(Ext, SMHC_REG_RINT, SMHC_INT_RX_DATA_REQUEST | SMHC_INT_TX_DATA_REQUEST);

    if (Ext->PioWordsRemaining > 0) {
        SmhcProgramPioThreshold(Ext);

        Request->RequiredEvents |= Read ? SDPORT_EVENT_BUFFER_FULL : SDPORT_EVENT_BUFFER_EMPTY;
        Request->Status = STATUS_MORE_PROCESSING_REQUIRED;

        SmhcImaskUpdate(Ext,
                        Read ? SMHC_INT_RX_DATA_REQUEST : SMHC_INT_TX_DATA_REQUEST,
                        0);

    } else {
        Request->Status = STATUS_SUCCESS;

        //
        // Make sure the end-of-data interrupt is enabled for the last phase
        // (ToggleEvents may have disabled it in between).
        //
        SmhcImaskUpdate(Ext,
                        Ext->AutoStopActive ? SMHC_INT_AUTO_COMMAND_DONE : SMHC_INT_DATA_OVER,
                        0);

        if (CurrentEvents & SDPORT_EVENT_CARD_RW_END) {
            InterlockedExchange(&Ext->PhaseClaim, 1);
            SmhcCompleteRequest(Ext, Request, Request->Status);

        } else {
            Request->RequiredEvents |= SDPORT_EVENT_CARD_RW_END;

            //
            // The end-of-data event may have been delivered on another processor
            // between the snapshot above and the line before: the DPC saw nothing
            // to wait for and only recorded it.  Look again, and claim the
            // completion so the DPC cannot complete the same phase too.
            //
            if (((ULONG)InterlockedCompareExchange(&Ext->CurrentEvents, 0, 0) &
                 SDPORT_EVENT_CARD_RW_END) &&
                (InterlockedCompareExchange(&Ext->PhaseClaim, 1, 0) == 0)) {

                Request->RequiredEvents = 0;
                SmhcCompleteRequest(Ext, Request, Request->Status);
            }
        }
    }

    return STATUS_PENDING;
}

static NTSTATUS
SmhcStartDmaTransfer(
    _In_ PSMHC_EXTENSION Ext,
    _In_ PSDPORT_REQUEST Request
    )
{
    //
    // The IDMAC was started when the command was built; nothing to do here, the
    // sample's SdhcStartAdmaTransfer / dwcmshc's MshcStartDmaTransfer just
    // complete the phase.
    //
    Request->Status = STATUS_SUCCESS;
    InterlockedExchangePointer((PVOID volatile *)&Ext->OutstandingRequest, NULL);
    SdPortCompleteRequest(Request, Request->Status);
    return STATUS_SUCCESS;
}

NTSTATUS
SmhcStartTransfer(
    _In_ PSMHC_EXTENSION Ext,
    _In_ PSDPORT_REQUEST Request
    )
{
    switch (Request->Command.TransferMethod) {
    case SdTransferMethodPio:
        return SmhcStartPioTransfer(Ext, Request);

    case SdTransferMethodSgDma:
        return SmhcStartDmaTransfer(Ext, Request);

    default:
        return STATUS_NOT_SUPPORTED;
    }
}

//
// ---------------------------------------------------------------------------
// Response and error mapping.
// ---------------------------------------------------------------------------
//

VOID
SmhcGetResponse(
    _In_ PSMHC_EXTENSION Ext,
    _In_ PSDPORT_COMMAND Command,
    _Out_ PVOID ResponseBuffer
    )
{
    ULONG Resp[4];

    RtlZeroMemory(ResponseBuffer, sizeof(ULONG));

    switch (Command->ResponseType) {
    case SdResponseTypeNone:
        break;

    case SdResponseTypeR1:
    case SdResponseTypeR1B:
    case SdResponseTypeR3:
    case SdResponseTypeR4:
    case SdResponseTypeR5:
    case SdResponseTypeR5B:
    case SdResponseTypeR6:
        *(PULONG)ResponseBuffer = SmhcRead(Ext, SMHC_REG_RESP0);
        break;

    case SdResponseTypeR2:
        Resp[0] = SmhcRead(Ext, SMHC_REG_RESP0);
        Resp[1] = SmhcRead(Ext, SMHC_REG_RESP1);
        Resp[2] = SmhcRead(Ext, SMHC_REG_RESP2);
        Resp[3] = SmhcRead(Ext, SMHC_REG_RESP3);
        SmhcFormatR2(Resp, (PUCHAR)ResponseBuffer);
        break;

    default:
        NT_ASSERTMSG("SMHC - invalid response type", FALSE);
        break;
    }
}

NTSTATUS
SmhcConvertErrorToStatus(
    _In_ ULONG Errors
    )
{
    //
    // Same mapping as sdhc.h SdhcConvertErrorToStatus / dwcmshc.cpp.
    //
    if (Errors == 0) {
        return STATUS_SUCCESS;
    }

    if (Errors & (SDPORT_ERROR_CMD_TIMEOUT | SDPORT_ERROR_DATA_TIMEOUT)) {
        return STATUS_IO_TIMEOUT;
    }

    if (Errors & (SDPORT_ERROR_CMD_CRC_ERROR | SDPORT_ERROR_DATA_CRC_ERROR)) {
        return STATUS_CRC_ERROR;
    }

    if (Errors & (SDPORT_ERROR_CMD_END_BIT_ERROR | SDPORT_ERROR_DATA_END_BIT_ERROR)) {
        return STATUS_DEVICE_DATA_ERROR;
    }

    if (Errors & SDPORT_ERROR_CMD_INDEX_ERROR) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    if (Errors & SDPORT_ERROR_BUS_POWER_ERROR) {
        return STATUS_DEVICE_POWER_FAILURE;
    }

    return STATUS_IO_DEVICE_ERROR;
}

//
// ---------------------------------------------------------------------------
// Request completion.
//
// R1b commands and write transfers leave DAT0 low while the card is busy; no new
// command may be issued until it releases ([UB-C]: poll STATUS.CARD_DATA_BUSY for
// MMC_RSP_BUSY commands).  Writes can stay busy for many hundred microseconds,
// so at DISPATCH_LEVEL the wait is moved to a work item (as dwcmshc.cpp does).
// ---------------------------------------------------------------------------
//

static NTSTATUS
SmhcWaitNotBusy(
    _In_ PSMHC_EXTENSION Ext
    )
{
    ULONG Elapsed;

    for (Elapsed = 0; Elapsed < SMHC_BUSY_TIMEOUT_US; Elapsed += 1) {
        if ((SmhcRead(Ext, SMHC_REG_STATUS) & SMHC_STATUS_CARD_DATA_BUSY) == 0) {
            return STATUS_SUCCESS;
        }

        SdPortWait(1);
    }

    return STATUS_IO_TIMEOUT;
}

static VOID
SmhcBusyWorker(
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_ PVOID Context
    )
{
    PSMHC_EXTENSION Ext;
    PSDPORT_REQUEST Request;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    Ext = (PSMHC_EXTENSION)Context;
    Request = Ext->OutstandingRequest;
    InterlockedExchange(&Ext->BusyPending, 0);
    if (Request == NULL) {
        return;
    }

    Status = SmhcWaitNotBusy(Ext);
    if (!NT_SUCCESS(Status)) {
        SMHC_LOG_ERROR("card stayed busy after CMD%lu\n", Request->Command.Index);
    }

    //
    // A reset may have aborted the request while we were waiting.
    //
    if (Ext->OutstandingRequest != Request) {
        return;
    }

    //
    // Re-enter with RequiredEvents == 0 and the busy condition resolved.
    //
    Request->RequiredEvents = 0;
    SmhcCompleteRequest(Ext, Request, Status);
}

VOID
SmhcCompleteRequest(
    _In_ PSMHC_EXTENSION Ext,
    _In_ PSDPORT_REQUEST Request,
    _In_ NTSTATUS Status
    )
{
    PSDPORT_COMMAND Command;
    BOOLEAN HasData;

    Command = &Request->Command;
    HasData = SmhcHasData(Command);
    Request->Status = Status;

    if ((Status == STATUS_SUCCESS) && (Request->RequiredEvents == 0) &&
        ((Command->ResponseType == SdResponseTypeR1B) ||
         (Command->ResponseType == SdResponseTypeR5B) ||
         (HasData && (Command->TransferDirection == SdTransferDirectionWrite)))) {

        if (SmhcRead(Ext, SMHC_REG_STATUS) & SMHC_STATUS_CARD_DATA_BUSY) {
            if ((Ext->BusyWorkItem != NULL) && (KeGetCurrentIrql() == DISPATCH_LEVEL)) {
                InterlockedExchange(&Ext->BusyPending, 1);
                IoQueueWorkItem(Ext->BusyWorkItem,
                                SmhcBusyWorker,
                                CriticalWorkQueue,
                                Ext);
                return;
            }

            Status = SmhcWaitNotBusy(Ext);
            Request->Status = Status;
        }
    }

    //
    // End of the data phase: stop the IDMAC / reset the FIFO.  This must NOT run
    // between PIO phases:
    //   - STATUS_MORE_PROCESSING_REQUIRED means more chunks follow;
    //   - for PIO the command-phase completion (SdRequestTypeCommandWithTransfer)
    //     only announces that the first data-request event arrived.
    // DMA data is complete when the command request completes (it waited for
    // CARD_RW_END / DMA_COMPLETE); PIO data is complete with the final
    // SdRequestTypeStartTransfer.  Any failure ends the phase.
    //
    if (HasData && (Request->Status != STATUS_MORE_PROCESSING_REQUIRED)) {
        BOOLEAN Finished;

        Finished = (Request->Status != STATUS_SUCCESS) ||
                   (Command->TransferMethod == SdTransferMethodSgDma) ||
                   (Request->Type == SdRequestTypeStartTransfer);

        if (Finished) {
            SmhcStopDmaAndFifo(Ext);
            Ext->PioWordsRemaining = 0;
            Ext->AutoStopActive = FALSE;
        }
    }

    SMHC_LOG_TRACE("CMD%lu done, status 0x%08X\n", Command->Index, Request->Status);

    //
    // Release the outstanding request on success only; on failure it stays so
    // SmhcResetHost can tell what kind of request failed (dwcmshc.cpp does the same).
    //
    if ((Request->Status == STATUS_SUCCESS) ||
        (Request->Status == STATUS_MORE_PROCESSING_REQUIRED)) {

        InterlockedExchangePointer((PVOID volatile *)&Ext->OutstandingRequest, NULL);
    }

    SdPortCompleteRequest(Request, Request->Status);
}
