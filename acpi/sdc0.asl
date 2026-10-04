// SDC0: Allwinner SMHC0 (microSD slot) -- Orange Pi Zero 3 (H618, SMHC identical to H616).
//
// This is the node from the task description, unchanged: the driver needs NOTHING else
// from ACPI for the bring-up configuration (firmware leaves the SMHC0 clock, pins and 3.3 V
// card supply configured; card detect is not usable on this board).
//
// Verified against Linux (sun50i-h616.dtsi, node mmc0):
//   reg        = <0x04020000 0x1000>                    -> Memory32Fixed below
//   interrupts = <GIC_SPI 35 IRQ_TYPE_LEVEL_HIGH>       -> GSIV 35 + 32 = 67, level, active high
//
Device (SDC0)
{
    Name (_HID, "AWMC0001")
    Name (_UID, 0x00)
    Name (_CCA, Zero)   // non-cache-coherent DMA
    Method (_STA, 0, NotSerialized) { Return (0x0F) }
    Name (_CRS, ResourceTemplate ()
    {
        Memory32Fixed (ReadWrite, 0x04020000, 0x00001000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 67 }
    })
}

// ---------------------------------------------------------------------------------------------
// OPTIONAL, not needed by the current driver -- what a future version would need:
//
//  * Faster than the firmware's 24 MHz (50 MHz High-Speed): the driver must program the SMHC0
//    module clock itself (CCU MMC0_CLK_REG 0x830 and the MMC bus gate/reset register 0x84C,
//    offsets from u-boot clock_sun50i_h6.h).  That needs two extra Memory32Fixed windows, e.g.
//        Memory32Fixed (ReadWrite, 0x03001830, 0x4)   // MMC0 clock    (CCU base 0x03001000 per sun50i-h616.dtsi)
//        Memory32Fixed (ReadWrite, 0x0300184C, 0x4)   // MMC gate/reset
//    The driver does not look at a second memory resource yet.
//
//  * Working card detect / 1.8 V: GpioInt for CD (PF6, broken on this board per Linux) and an
//    AXP313A DLDO1 / ALDO control path (I2C PMIC), neither of which is reachable from the SMHC
//    driver today.
// ---------------------------------------------------------------------------------------------
