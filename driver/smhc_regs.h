/*++

Module Name:

    smhc_regs.h

Abstract:

    Allwinner SMHC (SD/MMC host controller) register definitions, as found on
    the H616 / H618 family (SMHC0 is the microSD slot on the Orange Pi Zero 3).

    This file contains hardware facts only; no kernel types.

    Every definition is annotated with the reference it was taken from. Where a
    value could not be confirmed from the reference sources it is marked
    UNVERIFIED and must not be trusted until it has been checked on hardware.

    References (fetched from the "master" branches, 2026-10-04):

        [UB-H]  u-boot  drivers/mmc/sunxi_mmc.h          (struct sunxi_mmc + SUNXI_MMC_* bits)
        [UB-C]  u-boot  drivers/mmc/sunxi_mmc.c          (init / clock / command / PIO flow)
        [UB-CCU] u-boot arch/arm/include/asm/arch-sunxi/clock_sun50i_h6.h
        [LX]    linux   drivers/mmc/host/sunxi-mmc.c     (SDXC_* bits, IDMAC, quirks)
        [LX-DT] linux   arch/arm64/boot/dts/allwinner/sun50i-h616.dtsi, sun50i-h618-orangepi-zero3.dts

--*/

#pragma once

//
// ---------------------------------------------------------------------------
// Register offsets.
//   [UB-H] struct sunxi_mmc (offset comments), [LX] SDXC_REG_* (lines 40-74).
// ---------------------------------------------------------------------------
//

#define SMHC_REG_GCTRL          0x000   // Global control
#define SMHC_REG_CLKCR          0x004   // Clock control
#define SMHC_REG_TMOUT          0x008   // Timeout
#define SMHC_REG_WIDTH          0x00C   // Bus width
#define SMHC_REG_BLKSZ          0x010   // Block size
#define SMHC_REG_BCNTR          0x014   // Byte count (bytes, not blocks)
#define SMHC_REG_CMD            0x018   // Command
#define SMHC_REG_ARG            0x01C   // Command argument
#define SMHC_REG_RESP0          0x020   // Response bits  [31:0]
#define SMHC_REG_RESP1          0x024   // Response bits  [63:32]
#define SMHC_REG_RESP2          0x028   // Response bits  [95:64]
#define SMHC_REG_RESP3          0x02C   // Response bits [127:96]
#define SMHC_REG_IMASK          0x030   // Interrupt mask
#define SMHC_REG_MINT           0x034   // Masked interrupt status
#define SMHC_REG_RINT           0x038   // Raw interrupt status (write 1 to clear)
#define SMHC_REG_STATUS         0x03C   // Status
#define SMHC_REG_FTRGL          0x040   // FIFO threshold / watermark
#define SMHC_REG_FUNCSEL        0x044   // Function select
#define SMHC_REG_A12A           0x058   // Auto-CMD12 argument
#define SMHC_REG_NTSR           0x05C   // New timing set register
#define SMHC_REG_HWRST          0x078   // Card hardware reset (eMMC RST_n)
#define SMHC_REG_DMAC           0x080   // IDMAC control
#define SMHC_REG_DLBA           0x084   // IDMAC descriptor list base address
#define SMHC_REG_IDST           0x088   // IDMAC status
#define SMHC_REG_IDIE           0x08C   // IDMAC interrupt enable
#define SMHC_REG_THLDC          0x100   // Card threshold control   [UB-H] (CONFIG_SUN50I_GEN_H6 layout)
#define SMHC_REG_DRV_DL         0x140   // Drive delay              [LX] SDXC_REG_DRV_DL
#define SMHC_REG_SAMP_DL        0x144   // Sample delay             [LX] SDXC_REG_SAMP_DL_REG, [UB-H] samp_dl
#define SMHC_REG_DS_DL          0x148   // Data strobe delay        [LX] SDXC_REG_DS_DL_REG

//
// FIFO access port.
//   [UB-H] struct sunxi_mmc: for CONFIG_SUN50I_GEN_H6 the struct has thldc @0x100,
//          res3[16], samp_dl @0x144, res4[46], then fifo => 0x148 + 46*4 = 0x200.
//   UNVERIFIED: Linux never uses PIO so it does not state the offset, and I could
//   not read the H616 manual. u-boot's own comment says "0x100 / 0x200".
//
#define SMHC_REG_FIFO           0x200

//
// Minimum register window we touch (FIFO at 0x200 + one word).
//
#define SMHC_MIN_REGISTER_SPACE 0x204

//
// ---------------------------------------------------------------------------
// GCTRL.   [UB-H] SUNXI_MMC_GCTRL_*, [LX] SDXC_* (lines 82-94)
// ---------------------------------------------------------------------------
//

#define SMHC_GCTRL_SOFT_RESET           (1UL << 0)
#define SMHC_GCTRL_FIFO_RESET           (1UL << 1)
#define SMHC_GCTRL_DMA_RESET            (1UL << 2)
#define SMHC_GCTRL_INT_ENABLE           (1UL << 4)
#define SMHC_GCTRL_DMA_ENABLE           (1UL << 5)
#define SMHC_GCTRL_DEBOUNCE_ENABLE      (1UL << 8)
#define SMHC_GCTRL_POSEDGE_LATCH_DATA   (1UL << 9)
#define SMHC_GCTRL_DDR_MODE             (1UL << 10)
#define SMHC_GCTRL_MEMORY_ACCESS_DONE   (1UL << 29)
#define SMHC_GCTRL_ACCESS_DONE_DIRECT   (1UL << 30)
#define SMHC_GCTRL_ACCESS_BY_AHB        (1UL << 31)     // 1 = CPU accesses FIFO, 0 = IDMAC

#define SMHC_GCTRL_ALL_RESET            (SMHC_GCTRL_SOFT_RESET | \
                                         SMHC_GCTRL_FIFO_RESET | \
                                         SMHC_GCTRL_DMA_RESET)  // [LX] SDXC_HARDWARE_RESET

//
// ---------------------------------------------------------------------------
// CLKCR.   [UB-H] SUNXI_MMC_CLK_*, [LX] SDXC_CARD_CLOCK_ON / LOW_POWER_ON / MASK_DATA0
// ---------------------------------------------------------------------------
//

#define SMHC_CLKCR_DIVIDER_MASK         0xFFUL          // [UB-H] SUNXI_MMC_CLK_DIVIDER_MASK
#define SMHC_CLKCR_CARD_CLOCK_ON        (1UL << 16)
#define SMHC_CLKCR_LOW_POWER_ON         (1UL << 17)
#define SMHC_CLKCR_MASK_DATA0           (1UL << 31)     // [LX] needed on H616 (cfg.mask_data0)

//
// ---------------------------------------------------------------------------
// WIDTH.   [LX] SDXC_WIDTH1/4/8, [UB-C] sunxi_mmc_set_ios_common
// ---------------------------------------------------------------------------
//

#define SMHC_WIDTH_1BIT                 0UL
#define SMHC_WIDTH_4BIT                 1UL
#define SMHC_WIDTH_8BIT                 2UL

//
// ---------------------------------------------------------------------------
// CMD.   [UB-H] SUNXI_MMC_CMD_*, [LX] SDXC_* (lines 108-127)
// ---------------------------------------------------------------------------
//

#define SMHC_CMD_INDEX_MASK             0x3FUL
#define SMHC_CMD_RESP_EXPIRE            (1UL << 6)      // a response is expected
#define SMHC_CMD_LONG_RESPONSE          (1UL << 7)      // 136-bit response (R2)
#define SMHC_CMD_CHK_RESPONSE_CRC       (1UL << 8)
#define SMHC_CMD_DATA_EXPIRE            (1UL << 9)      // data phase follows
#define SMHC_CMD_WRITE                  (1UL << 10)     // data direction: host -> card
#define SMHC_CMD_SEQUENCE_MODE          (1UL << 11)
#define SMHC_CMD_AUTO_STOP              (1UL << 12)     // send CMD12 automatically
#define SMHC_CMD_WAIT_PRE_OVER          (1UL << 13)     // wait for previous data to finish
#define SMHC_CMD_STOP_ABORT             (1UL << 14)
#define SMHC_CMD_SEND_INIT_SEQ          (1UL << 15)     // 80 clocks before the command
#define SMHC_CMD_UPCLK_ONLY             (1UL << 21)     // only latch clock registers
#define SMHC_CMD_VOLTAGE_SWITCH         (1UL << 28)
#define SMHC_CMD_USE_HOLD_REG           (1UL << 29)
#define SMHC_CMD_START                  (1UL << 31)

//
// ---------------------------------------------------------------------------
// RINT / MINT / IMASK bits.   [UB-H] SUNXI_MMC_RINT_*, [LX] SDXC_* (lines 130-147)
// ---------------------------------------------------------------------------
//

#define SMHC_INT_RESP_ERROR             (1UL << 1)
#define SMHC_INT_COMMAND_DONE           (1UL << 2)
#define SMHC_INT_DATA_OVER              (1UL << 3)
#define SMHC_INT_TX_DATA_REQUEST        (1UL << 4)
#define SMHC_INT_RX_DATA_REQUEST        (1UL << 5)
#define SMHC_INT_RESP_CRC_ERROR         (1UL << 6)
#define SMHC_INT_DATA_CRC_ERROR         (1UL << 7)
#define SMHC_INT_RESP_TIMEOUT           (1UL << 8)
#define SMHC_INT_DATA_TIMEOUT           (1UL << 9)
#define SMHC_INT_VOLTAGE_CHANGE_DONE    (1UL << 10)
#define SMHC_INT_FIFO_RUN_ERROR         (1UL << 11)
#define SMHC_INT_HARDWARE_LOCKED        (1UL << 12)
#define SMHC_INT_START_BIT_ERROR        (1UL << 13)
#define SMHC_INT_AUTO_COMMAND_DONE      (1UL << 14)
#define SMHC_INT_END_BIT_ERROR          (1UL << 15)
#define SMHC_INT_SDIO                   (1UL << 16)
#define SMHC_INT_CARD_INSERT            (1UL << 30)
#define SMHC_INT_CARD_REMOVE            (1UL << 31)

//
// Error group handed to sdport.  Deliberately excludes VOLTAGE_CHANGE_DONE
// (bit 10).  [UB-H] lists it inside its error mask (0xbfc2); [LX]'s
// SDXC_INTERRUPT_ERROR_BIT excludes it and counts it as a "done" bit.  It belongs
// to the voltage-switch sequence, which this driver does not implement.
//
#define SMHC_INT_ERRORS \
    (SMHC_INT_RESP_ERROR | SMHC_INT_RESP_CRC_ERROR | SMHC_INT_DATA_CRC_ERROR | \
     SMHC_INT_RESP_TIMEOUT | SMHC_INT_DATA_TIMEOUT | SMHC_INT_FIFO_RUN_ERROR | \
     SMHC_INT_HARDWARE_LOCKED | SMHC_INT_START_BIT_ERROR | SMHC_INT_END_BIT_ERROR)

#define SMHC_INT_ALL                    0xFFFFFFFFUL

//
// ---------------------------------------------------------------------------
// STATUS.   [UB-H] SUNXI_MMC_STATUS_*
// ---------------------------------------------------------------------------
//

#define SMHC_STATUS_RXWL_FLAG           (1UL << 0)
#define SMHC_STATUS_TXWL_FLAG           (1UL << 1)
#define SMHC_STATUS_FIFO_EMPTY          (1UL << 2)
#define SMHC_STATUS_FIFO_FULL           (1UL << 3)
#define SMHC_STATUS_CARD_PRESENT        (1UL << 8)
#define SMHC_STATUS_CARD_DATA_BUSY      (1UL << 9)      // DAT0 low
#define SMHC_STATUS_DATA_FSM_BUSY       (1UL << 10)
#define SMHC_STATUS_FIFO_LEVEL_SHIFT    17
#define SMHC_STATUS_FIFO_LEVEL_MASK     0x3FFFUL        // [UB-H] SUNXI_MMC_STATUS_FIFO_LEVEL

#define SMHC_STATUS_FIFO_LEVEL(_Status) \
    (((_Status) >> SMHC_STATUS_FIFO_LEVEL_SHIFT) & SMHC_STATUS_FIFO_LEVEL_MASK)

//
// ---------------------------------------------------------------------------
// FTRGL (FIFO threshold).
//   [LX] sunxi_mmc_init_host: writes 0x20070008 and comments
//        "Burst 8 transfers, RX trigger level: 7, TX trigger level: 8".
//   Field positions below follow the DesignWare FIFOTH layout (dwcmshc.h,
//   MSHC_FIFOTH_*) which 0x20070008 matches: MSIZE=2 @[30:28], RX_TL=7 @[27:16],
//   TX_TL=8 @[11:0].  The Allwinner manual was not available => UNVERIFIED.
// ---------------------------------------------------------------------------
//

#define SMHC_FTRGL_MSIZE_SHIFT          28
#define SMHC_FTRGL_RX_TL_SHIFT          16
#define SMHC_FTRGL_TX_TL_SHIFT          0
#define SMHC_FTRGL_MSIZE_8              2UL

#define SMHC_FTRGL_VALUE(_RxTl, _TxTl)  \
    ((SMHC_FTRGL_MSIZE_8 << SMHC_FTRGL_MSIZE_SHIFT) | \
     (((ULONG)(_RxTl)) << SMHC_FTRGL_RX_TL_SHIFT) | \
     (((ULONG)(_TxTl)) << SMHC_FTRGL_TX_TL_SHIFT))

#define SMHC_FTRGL_DEFAULT              0x20070008UL    // [LX] sunxi_mmc_init_host

//
// TMOUT.   [LX] sunxi_mmc_init_host writes 0xffffffff ("Maximum timeout value").
//
#define SMHC_TMOUT_MAX                  0xFFFFFFFFUL

//
// ---------------------------------------------------------------------------
// NTSR (new timing).
//   [UB-H] SUNXI_MMC_NTSR_MODE_SEL_NEW (bit 31), [LX] SDXC_2X_TIMING_MODE (bit 31).
// ---------------------------------------------------------------------------
//

#define SMHC_NTSR_MODE_SEL_NEW          (1UL << 31)

//
// SAMP_DL.   [UB-H] SUNXI_MMC_CAL_DL_SW_EN, [LX] SDXC_CAL_DL_SW_EN (bit 7).
// Linux (sunxi_mmc_calibrate): "The best rate have been obtained by simply setting
// the delay to 0, as Allwinner does in its BSP."  => write SW_EN with delay 0.
//
#define SMHC_SAMP_DL_SW_EN              (1UL << 7)

//
// THLDC (card threshold).   [UB-H] SUNXI_MMC_THLDC_*, written in sunxi_mmc_reset()
// for CONFIG_SUN50I_GEN_H6 with the comment "Needed on H616".
//
#define SMHC_THLDC_READ_EN              (1UL << 0)
#define SMHC_THLDC_BSY_CLR_INT_EN       (1UL << 1)
#define SMHC_THLDC_WRITE_EN             (1UL << 2)
#define SMHC_THLDC_READ_THLD(_X)        ((((ULONG)(_X)) & 0xFFFUL) << 16)

#define SMHC_THLDC_DEFAULT \
    (SMHC_THLDC_READ_THLD(512) | SMHC_THLDC_WRITE_EN | SMHC_THLDC_READ_EN)

//
// ---------------------------------------------------------------------------
// IDMAC.   [UB-H] SUNXI_MMC_IDMAC_*, SUNXI_MMC_IDIE_*, [LX] SDXC_IDMAC_* (177-199)
// ---------------------------------------------------------------------------
//

#define SMHC_DMAC_SOFT_RESET            (1UL << 0)
#define SMHC_DMAC_FIX_BURST             (1UL << 1)
#define SMHC_DMAC_IDMA_ON               (1UL << 7)

#define SMHC_IDST_TX_INTERRUPT          (1UL << 0)
#define SMHC_IDST_RX_INTERRUPT          (1UL << 1)
#define SMHC_IDST_FATAL_BUS_ERROR       (1UL << 2)
#define SMHC_IDST_DESTINATION_INVALID   (1UL << 4)
#define SMHC_IDST_CARD_ERROR_SUM        (1UL << 5)
#define SMHC_IDST_NORMAL_SUM            (1UL << 8)
#define SMHC_IDST_ABNORMAL_SUM          (1UL << 9)
#define SMHC_IDST_HOST_ABORT            (1UL << 10)

#define SMHC_IDST_ERRORS \
    (SMHC_IDST_FATAL_BUS_ERROR | SMHC_IDST_DESTINATION_INVALID | \
     SMHC_IDST_CARD_ERROR_SUM | SMHC_IDST_ABNORMAL_SUM)

#define SMHC_IDST_CLEAR_ALL             0x337UL         // [LX] sunxi_mmc_finalize_request

#define SMHC_IDIE_TX_INTERRUPT          SMHC_IDST_TX_INTERRUPT
#define SMHC_IDIE_RX_INTERRUPT          SMHC_IDST_RX_INTERRUPT

//
// ---------------------------------------------------------------------------
// IDMAC descriptor.   [LX] struct sunxi_idma_des, SDXC_IDMAC_DES0_*
//
//   config        - flags below
//   buf_size      - bytes; (1 << idma_des_size_bits) is encoded as 0.
//   buf_addr_ptr1 - data buffer address   >> idma_des_shift
//   buf_addr_ptr2 - next descriptor addr  >> idma_des_shift (chain mode)
//
// H616 config [LX] sun50i_h616_cfg: idma_des_size_bits = 16, idma_des_shift = 2.
// ---------------------------------------------------------------------------
//

#define SMHC_IDMAC_DES_DIC              (1UL << 1)      // disable interrupt on completion
#define SMHC_IDMAC_DES_LD               (1UL << 2)      // last descriptor
#define SMHC_IDMAC_DES_FD               (1UL << 3)      // first descriptor
#define SMHC_IDMAC_DES_CH               (1UL << 4)      // chained (buf_addr_ptr2 is next)
#define SMHC_IDMAC_DES_ER               (1UL << 5)      // end of ring
#define SMHC_IDMAC_DES_CES              (1UL << 30)     // card error summary
#define SMHC_IDMAC_DES_OWN              (1UL << 31)     // 1 = owned by IDMAC

#define SMHC_IDMAC_DES_SIZE_BITS        16              // [LX] sun50i_h616_cfg.idma_des_size_bits
#define SMHC_IDMAC_DES_SHIFT            2               // [LX] sun50i_h616_cfg.idma_des_shift
#define SMHC_IDMAC_MAX_LEN_PER_DESC     (1UL << SMHC_IDMAC_DES_SIZE_BITS)

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif
typedef struct _SMHC_IDMAC_DESCRIPTOR {
    ULONG Config;
    ULONG BufSize;
    ULONG BufAddr;
    ULONG NextDesc;
} SMHC_IDMAC_DESCRIPTOR, *PSMHC_IDMAC_DESCRIPTOR;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

//
// ---------------------------------------------------------------------------
// H616 CCU (module clock) -- documented for reference, NOT used by this driver
// version.  The firmware leaves SMHC0 clocked at 24 MHz.
//   [UB-CCU] CCU_MMC0_CLK_CFG 0x830, CCU_H6_MMC_GATE_RESET 0x84c,
//            CCM_MMC_CTRL_M/N/OSCM24/PLL6/ENABLE.
//   CCU base 0x03001000: [LX-DT] sun50i-h616.dtsi, node ccu: clock@3001000, reg = <0x03001000 0x1000>.
// ---------------------------------------------------------------------------
//

#define SMHC_CCU_MMC0_CLK_CFG_OFFSET    0x830
#define SMHC_CCU_MMC_BGR_OFFSET         0x84C
