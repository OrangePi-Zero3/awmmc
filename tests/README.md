# Host-side tests

```
make -C tests check      # unit tests + flow tests, built with -fsanitize=address,undefined
make -C tests syntax     # compile-check the driver sources only
```

## What this is and is not

* `mock/ntddk.h`, `mock/sdport.h` are **hand-written stand-ins** for the WDK headers, reconstructed from the identifiers used by Microsoft's sdhc sample and the
  dwcmshc miniport. They let the real driver sources (`../driver/smhc.c`, `smhc_hw.c`, `smhc_core.h`) compile and run on a host. They are **not** the real API:
  field order, types and event-bit values differ, so a clean host build says nothing about WDK compile errors.
* `emu.c` models an SMHC and an SD card at register level (command execution, FIFO with a small depth, level- or edge-triggered data requests,
  IDMAC descriptor walking with a lagging copy, auto-CMD12, busy, fault injection, soft reset that wipes registers).
  The model encodes this project's reading of the Linux/u-boot sources (cited in the comments of `driver/smhc_regs.h`); where those are silent it picks the pessimistic behaviour.
  Passing means *consistent with that model*, not *works on silicon*.
* `test_flow.c` plays the part of sdport: it calls `IssueRequest`, services the (emulated) interrupt through the driver's `Interrupt` and `RequestDpc`, issues the PIO/DMA
  `StartTransfer` phases the way the Microsoft sample's request flow does, and runs the reset sequence after errors.
* `test_core.c` unit-tests the pure logic in `smhc_core.h`.

Adding a test for a hardware finding: reproduce the behaviour in `emu.c` first (it should make an existing test fail), then fix the driver.
