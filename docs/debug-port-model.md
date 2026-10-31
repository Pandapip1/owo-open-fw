# The debug port and the J-Link in the simulator

The QEMU `efr32bg22` machine has the debug port a probe sees on SWCLK/SWDIO,
and two simulated probes that drive it: SEGGER's J-Link (the EK's on-board
J-Link OB and the suit's J-Link EDU Mini), presented to the host as a real
USB device, and a socket probe for runs without USB. OpenOCD and the
project's own procedures (`scripts/flash-bg22-openocd.sh`,
`scripts/bg22-openocd-flash.cfg`, the suit's `ro.cfg`/`raw.cfg`) run against
the simulator unchanged apart from the probe, so hardware procedures can be
rehearsed first.

Every behaviour below is either architectural (cited to the Arm
specification it follows) or evidenced on our silicon (cited to the
measurement). Anything the model needs that has not been measured is listed
in the last section and in TODO.md with a proposed measurement; nothing is
filled in from guesswork.

## Using it

Socket probe (no kernel modules, used by CI):

```sh
qemu-system-arm -M efr32bg22,flash=<image.bin> -icount shift=5 ... \
  -chardev socket,id=swd,host=localhost,port=44242,server=on,wait=off \
  -device swd-remote-bitbang,chardev=swd
openocd -f scripts/sim/qemu-swd.cfg -f target/silabs/xg21.cfg ...
OWO_BG22_ADAPTER_CFG=scripts/sim/qemu-swd.cfg scripts/flash-bg22-openocd.sh <image.elf>
```

Simulated J-Link on a host USB device controller (UDC). The UDC is handed to
the emulated probe much like a passed-through device: QEMU owns the host's
gadget interface and the probe model answers the USB host exactly as the
probe would. With a real peripheral-mode port (e.g. on a board whose USB
port can act as a device) the probe appears to whatever host is plugged in;
on a workstation, `dummy_hcd` provides a loop-back UDC:

```sh
modprobe dummy_hcd; modprobe raw_gadget       # /dev/raw-gadget, /sys/class/udc/dummy_udc.0
qemu-system-arm -M efr32bg22,flash=<image.bin> ... \
  -object usb-gadget-host,id=udc0,udc-driver=dummy_udc,udc-device=dummy_udc.0 \
  -device jlink,model=ob,gadget=udc0[,vcom=<chardev>]
# The OB's VCOM is the kit's console, USART1 (zephyr,console = &usart1):
#   -chardev socket,id=soc.usart1,host=localhost,port=P,server=on,wait=off \
#   -chardev socket,id=vcom,host=localhost,port=P  ...,vcom=vcom
openocd -f interface/jlink.cfg -c 'adapter serial 004242000105' -c 'transport select swd' ...
OWO_BG22_JLINK_SERIAL=004242000105 scripts/flash-bg22-openocd.sh <image.elf>
```

`tests/debug-port/run-e2e.sh` runs every procedure below and checks each
result against silicon (`PROBE=rbb`, or `PROBE=jlink UDC_DRIVER=...
UDC_DEVICE=...`).

## Structure

| Piece | File (vendor/qemu) | Role |
|---|---|---|
| ARMMDebugOps | `target/arm/cpu.h`, `debug_helper.c`, `tcg/m_helper.c`, `cpu.c` | the core reports BKPT, FPB matches and the end of a halting step; a core in Debug state has no work; DCRSR register access |
| `armv8m-debug` | `hw/misc/armv8m-debug.c` | M33 debug unit: DHCSR, DCRSR, DCRDR, DEMCR, DSCSR, DAUTHSTATUS, FPB, DWT CYCCNT/PCSR, PPB ROM table |
| `efr32-dap` | `hw/misc/efr32-dap.c` | SW-DP, AP0 (AHB-AP into the M33 bus), AP1 (Secure Engine DCI); SYSRESETREQ |
| MSC | `hw/arm/efr32bg22.c` (`efr32_msc_*`) | flash page erase and word programming, shared with the firmware's NVM3 |
| `swd-target` | `include/hw/misc/swd.h` | the pins a probe sees: SWDIO level per SWCLK cycle, VTref |
| `swd-remote-bitbang` | `hw/misc/swd-remote-bitbang.c` | socket probe on OpenOCD's remote_bitbang protocol |
| `usb-gadget-host` | `hw/usb/gadget-host.c` | owns a host UDC through Linux raw-gadget; silicon independent |
| `jlink` | `hw/misc/jlink.c` | the probe: USB descriptors and requests, J-Link commands, CDC-ACM VCOM, SWD pins |
| hookup | `hw/arm/efr32bg22-debug.c` | instantiates and maps the chip blocks; one call from `efr32bg22.c` |

The host-device backend is a thin owner of the kernel interface; everything
the USB host observes is the probe model's behaviour. The probes are silicon
independent and drive any `swd-target`.

## The chip side

### SW-DP (ADIv5.2, DPv2)

Line reset (50 or more ones), JTAG-to-SWD select (0xE79E) from the power-on
JTAG mode (RM 7.3.1), request/turnaround/ACK/data phases with parity, and the
sticky-flag rules: with STICKYERR, STICKYCMP, STICKYORUN or WDATAERR set,
every access except a DPIDR read, a CTRL/STAT read and an ABORT write gets
FAULT; AP reads are posted (RDBUFF); after a line reset only a DPIDR read is
answered. Power-up requests are acknowledged at once. No WAIT is ever given:
AP transfers complete within the access.

| Value | Model | Evidence |
|---|---|---|
| DPIDR | 0x6ba02477 | OpenOCD "SWD DPIDR 0x6ba02477", EK and suit |
| AP0 IDR | 0x84770001 | read from the suit |
| AP0 BASE | 0xe00ff003 (Cortex-M33 PPB ROM table) | architectural default; the part's BASE is not captured |
| TARGETID, DLPIDR | read-as-one bit only | not captured |

### Cortex-M33 debug unit

DHCSR with DBGKEY, C_DEBUGEN/C_HALT/C_STEP/C_MASKINTS, S_HALT, S_REGRDY,
S_SLEEP, sticky S_RETIRE_ST and S_RESET_ST (cleared by a read), S_SDE;
DCRSR/DCRDR transfers for every REGSEL OpenOCD uses (Armv8-M encoding,
including the banked MSP/PSP and packed CONTROL words); DEMCR; DSCSR (CDS
switches the core's security state in Debug state); DFSR (the NVIC's) gets
HALTED, BKPT or VCATCH. Halts enter on a debugger C_HALT (at the next
instruction boundary), BKPT, an FPB match, or the end of a halting step.
Writes from the core itself to DHCSR/DCRSR/DSCSR are ignored.

FPB version 2 with 8 code comparators and DWT with 4 comparators: OpenOCD
reports "target has 8 breakpoints, 4 watchpoints" on the EK and the suit.
CPUID is r0p4 ("Cortex-M33 r0p4 processor detected"; QEMU's default is
r0p3). DWT CYCCNT counts core clock cycles while TRCENA and CYCCNTENA are set
and the core is not halted; PCSR returns the core's PC (as of the last
translation-block boundary) while it runs and all ones when halted. DWT
watchpoint comparators are stored but do not match.

The debug unit is in the debug domain: a system reset does not reset it.
DHCSR.C_HALT and C_STEP are cleared by the reset; C_DEBUGEN, DEMCR, the FPB
and the DWT stay.

### AP1: the Secure Engine's Debug Challenge Interface

A MEM-AP whose bus holds the DCI mailbox at 0x1000 (WDATA, RDATA, STATUS,
ID 0xdc11d), as OpenOCD's `target/silabs/series2.cfg` uses it. GET_STATUS
(0xfe010000) answers with a length word and four status words; the debug-lock
word reads "Debug lock (config): Disabled, Device erase: Enabled, Secure
debug: Disabled, Debug lock (hw status): Disabled", as read from the suit and
the EK. The other status words were not captured and read zero. Device erase
(0x430f0000) erases main flash and resets the chip. Other commands get no
reply (OpenOCD's DCI read then times out).

### MSC

There is one MSC model, in `efr32bg22.c`; the AHB-AP reaches it like the
core does (0x40030000 and the non-secure alias 0x50030000 that OpenOCD's
efm32 driver uses), so the debugger's flash path and the firmware's NVM3
path are the same code. Page erase (8 KiB main pages, the User Data page)
and word programming through WRITECTRL.WREN, ADDRB and WDATA; programming
only clears bits; INVADDR and LOCKED follow ADDRB; page locks,
MISCLOCKWORD.UDLOCKBIT and MELOCKBIT refuse operations; MSC_LOCK locks
READCTRL and WRITECTRL; IF/IEN drive IRQ 49 (RM 6.3.12, 6.7, 6.8). A core
can only set the MISCLOCKWORD locks, a debugger may also clear them ("Host
is only allowed to write one. Root and debug can clear this bit", RM 6.8.13).
Operations complete at once (BUSY is never seen); see the open items.

### SYSRESETREQ

One path: the NVIC's SYSRESETREQ sets EMU.RSTCAUSE.SYSREQ, tells the DAP
that a reset began, and requests the system reset (`efr32_soc_sysresetreq`).
AP accesses fail (FAULT and STICKYERR) from the request until the reset has
been performed, and for `sysreset-blackout-ns` after it (default 0; see
quirk 4). A SYSRESETREQ written by the debugger reads back as RSTCAUSE
0x41 (POR and SYSREQ) in the simulator.

## Silicon quirks, evidence and model

1. **`reset halt` never halts.** On silicon, `reset halt` prints "external
   reset detected" and then "timed out while waiting for target halted"; the
   core keeps running (bg22-openocd-flash.cfg). Model: the reset vector catch
   (DEMCR.VC_CORERESET) is not honoured on this part
   (`vc-corereset-honoured=false`). Reproduced: `reset halt` times out.

   The explanation in bg22-openocd-flash.cfg — that with "Secure debug:
   Disabled" no halt is honoured in the Secure state — does not hold.
   "Secure debug" in the DCI status is the Secure Engine's certificate-based
   secure debug unlock feature, not TrustZone invasive debug; DHCSR.S_SDE
   reads 1 on the suit (DHCSR 0x01100001 and 0x03130003), so Secure invasive
   debug is enabled; and an FPB comparator halted the core at the Secure
   bootloader's reset handler (suit, 2026-10-31). Why the vector catch is
   not honoured is open (TODO.md).

2. **FPB comparators survive SYSRESETREQ and catch later.** Verified 5/5 on
   the EK (`bg22_reset_to_breakpoint`, main and z_cstart) and on the suit
   (raw AHB-AP writes, then SYSRESETREQ: DHCSR 0x03130003, PC at the
   bootloader reset handler, SP the initial MSP). Model: the debug unit is
   not reset by a system reset. Reproduced exactly, including DHCSR
   0x03130003 (C_DEBUGEN, C_HALT, S_REGRDY, S_HALT, S_SDE, S_RETIRE_ST,
   S_RESET_ST).

3. **"external reset detected".** Architectural: DHCSR.S_RESET_ST is set by
   every reset and cleared by a read; OpenOCD prints the message on its first
   poll after a reset it did not itself command. Reproduced.

4. **The AP is briefly unreadable after SYSRESETREQ.** run-control.log (EK):
   after the reset, "Polling failed, trying to reexamine", "Examination
   failed", "Failed to read memory and, additionally, failed to find out
   where" and "Failed to read DCIID" while the DP itself answered ("SWD DPIDR
   0x6ba02477"), then recovery. So the APs, including DCI, fail while the DP
   works. Model: AP accesses fail with FAULT/STICKYERR during the reset and
   for `-global efr32-dap.sysreset-blackout-ns=<ns>` after it. The duration
   was not measured, so the default is 0; `bg22_reset_to_breakpoint`
   tolerates either.

5. **TARGET_HALTED with an all-zero register file.** Not a chip behaviour:
   the simulator reproduces it exactly ("halted due to undefined ...
   xPSR: 00000000 pc: 00000000 msp: 00000000", as in run-control.log). In
   OpenOCD's `cortex_m_poll_one()` the registers are read
   (`cortex_m_debug_entry()`) only when the previous state was RUNNING or
   RESET. When a script writes DHCSR.C_HALT before OpenOCD has polled the
   target (its state is still UNKNOWN after examine), the next `poll` marks
   it HALTED without reading the registers, and prints the empty cache. With
   `-d3` no register read happens before the print. The SRAM-execution proof
   is still the right guard: it does not trust OpenOCD's state.

6. **Zephyr's MPU makes SRAM execute-never.** Architectural (the M33 MPU
   model); `bg22_require_sram_execution` clears MPU_CTRL and passes.

7. **Fast flash, slow recovery.** The fast path (OpenOCD's Series 2 loader
   in the 2 KiB work area, `WORKAREASIZE 0x800`) and the fault-injected slow
   path (`OWO_BG22_TEST_FAIL_AFTER_ERASE=1`, AP writes with `WORKAREASIZE 0`)
   both run on the MSC model: with the fault injected, the script reports
   "fast flash failed after erase; restoring with AP writes", then "BG22
   slow recovery write/verify passed" and exits 1, as designed. Over the
   socket probe the slow path takes over ten minutes for a 160 KiB image
   (one socket round trip per sampled SWDIO bit); the J-Link batches SWD
   I/O like the real probe.

8. **"Erasing vectors can change the observable core/debug state".** No
   mechanism is known; the simulator's second execution proof passes. Open
   (TODO.md).

9. **A C_HALT request while the stock application runs is not honoured**
   (suit: DHCSR read back 0x01100001, then "external reset detected"). Not
   modelled: the mechanism is unknown, and with Zephyr on the EK the same
   request halts. Under the simulator the request halts whatever runs. Open
   (TODO.md).

10. **DWT PCSR samples a running core** (suit). Modelled.

## The probes

| | J-Link OB (EK) | J-Link EDU Mini (suit) |
|---|---|---|
| USB VID:PID | 1366:0105 | 1366:0101 |
| Simulated serial | 004242000105 | 004242000101 |
| Real serial (never used) | 000440200548 | 000801032573 |
| Firmware string | "Silicon Labs J-Link OB compiled Oct 20 2020 14:45:20" (OpenOCD on the EK) | placeholder, not captured |
| Hardware version | 1.00 (OpenOCD on the EK) | placeholder 1.00 |
| SWD clock limit | 1900 kHz ("Reduced speed from 4000 kHz to 1900 kHz (maximum)") | 4000 kHz (SEGGER's product description, ~4 MHz) |
| VTref | the target's: 3.320 V (EK, 3.318-3.322 V over 66 sessions) | the target's |
| VCOM | CDC-ACM function, bridged to the `vcom` chardev | none |

Simulated serial numbers are ten digits, 4242000xxx: SEGGER's factory
serials have nine, so a simulated probe can never be selected instead of a
real one (libjaylink parses the last ten digits into 32 bits).

Commands (libjaylink's encoding): GET_VERSION, GET_CAPS (GET_HW_VERSION,
GET_SPEEDS, SELECT_TIF), GET_HW_VERSION, GET_HW_STATUS (VTref), GET_SPEEDS,
SELECT_TIF (SWD only), SET_SPEED (recorded; SWCLK timing is not modelled),
SWD I/O (each bit one SWCLK cycle of the target), reset and TRST lines
(accepted: no nRESET is wired on the EK). The probes enumerate full speed
with one vendor-specific interface (bulk IN 0x81, OUT 0x01); the OB adds an
IAD and a CDC-ACM pair (notification 0x83, data 0x82/0x02). The real probes'
descriptors and capability words were not captured (TODO.md).

## Results

- qtest `efr32bg22-dap-test`: 6/6 (DP identity and the post-reset DPIDR
  rule, AP identity and memory, ROM table, sticky errors, DCI status, halt
  and core registers, FPB across SYSRESETREQ, MSC programming).
  `efr32bg22-test` 98/98 and `efr32bg22-rf-test` 24/24 still pass.
- Dual-MCU radio run (scratch/sched/dual.sh b-adv b-scan, shift 3, 60 s):
  122 SCAN_REQ, 122 SCAN_RSP, no late/unsat lines.
- USB: on dummy_hcd the OB enumerates as 1366:0105 "J-Link" serial
  004242000105 with three interfaces; /dev/ttyACM0 carries USART1's console
  output when the VCOM is paired with USART1. The EDU Mini enumerates as
  1366:0101 serial 004242000101. OpenOCD (libusb) could not yet open either:
  the node /dev/bus/usb/003/NNN is root:root 0664 on this host, which has no
  udev rule for SEGGER's vendor ID (libjaylink ships 60-libjaylink.rules).
- The fault-injected slow recovery of `flash-bg22-openocd.sh` (socket probe).
- `tests/debug-port/run-e2e.sh` with the socket probe: all procedures behave
  as on silicon (DEVINFO through the read-only mem_ap session, DCI status,
  `reset halt` times out, preflight, fast erase/write/verify of a Zephyr
  ELF that then runs, `bg22_reset_to_breakpoint` catches main(), the raw
  procedure reads DHCSR 0x03130003 with PC and SP from the vector table,
  PCSR samples).

## Not modelled, or modelled without a measurement

- Reset vector catch on silicon: why VC_CORERESET is not honoured (is
  DEMCR cleared by the reset, or does the catch not fire?).
- Duration and failure mode (FAULT or WAIT) of the AP window after
  SYSRESETREQ.
- MSC erase and write durations; BUSY is never set.
- The part's ROM table, AP0 BASE, AP1 IDR, TARGETID/DLPIDR, component ID
  revisions, the other DCI status words.
- The M0+ (AP3, deferred examine in xg22-dual-deferred.cfg): not on the
  model's DAP.
- DWT watchpoints; C_MASKINTS while running; S_LOCKUP; ITM, ETM, CTI, TPIU.
- Debug-access side effects (a debugger read popping a FIFO) and the SWD
  access cost on the bus; EM2 with a debugger attached.
- The probes' real USB descriptors, capability words, EDU Mini firmware
  string and hardware version; JTAG; SWCLK timing.
- The OB VCOM joins USART1 through a pair of socket chardevs (configuration,
  not an in-process link); line coding from the host is not applied to the
  USART.

Note for test images: with the USART's line timing, the radio-port QEMU
build (radio-port-qemu-new) prints "in_delay_get needs software" from its
IRQ 18 handler; the SRAM-execution proof resumed into that handler's
uart_poll_out loop and did not reach its BKPT within the 5 s wait, so the
proof fails on that image. The e2e run uses radio-port-qemu, which passes.
