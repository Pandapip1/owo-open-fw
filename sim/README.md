# QEMU simulation of the OWO suit (EFR32BG22)

**Unvalidated on hardware. Bench-test the output stage on a dummy load first.**

* `vendor/qemu/` — pinned QEMU v9.2.0 fork adding the `efr32bg22` machine
  (Cortex-M33, IDAU, a preloaded RAM-backed flash image, and an in-process
  native MMIO model). Flash contents survive CPU reset in one process but are
  not written back to the input image or retained across separate QEMU runs.
* `sim/bootloader/` — hand-configured build of the real Gecko Bootloader main stage
  (sources from gecko_sdk, see `Makefile`; app base moved to 0x16000 to match the suit).
  SHA-256 is a software shim and ECDSA verification is a stub: signatures are NOT checked.
* `sim/scripts/` — `mkflash.py` composes bootloader + app into a flash image,
  `run-qemu.sh` / `iter.sh` run it, `disasm_stock.py` disassembles the stock image.

## Building and running the simulator

Enter the project environment first:

```sh
nix develop
mkdir -p build/qemu
(cd build/qemu && ../../vendor/qemu/configure --target-list=arm-softmmu --disable-docs --disable-capstone --enable-slirp)
ninja -C build/qemu qemu-system-arm
```

The build directory is deliberately retained as the incremental QEMU cache. The launcher uses
`build/qemu/qemu-system-arm` by default; set `QEMU_BIN` only to run a different binary. It writes
QEMU's own transient runtime log under `build/qemu-runtime/`; remove that directory after inspection.

For native RAC command diagnostics, enable QEMU's built-in trace event (not an external
peripheral server):

```sh
build/qemu/qemu-system-arm -trace efr32bg22_rac_command,file=build/qemu-runtime/rac.trace ...
```

It records written command words and every modeled RAC state transition, including timer-driven
completion. The trace file is transient and belongs under `build/qemu-runtime/`.

## The suit board and stimulation scenarios

`-M efr32bg22,board=owo-suit` attaches the suit control unit's parts (DAC, channel switches,
expander) to the SoC's SPI, I2C and GPIO lines and models the output stage; it records a VCD
and injects faults at chosen virtual times. `sim/scripts/run-suit-board.sh` runs an image on it;
`sim/scripts/stim_scenario.py` runs a scenario (commands through gdb, faults, recording,
summary). Method, parameters, results and limits: `docs/stim-simulation.md`.

## Host devices

A host device is handed to an emulated peripheral much as a PCI device is passed through to a
VM, except that the emulated silicon sits in between and translates: the peripheral model
(`vendor/qemu/hw/arm/efr32-usart.c`) implements the EFR32's registers, buffers, interrupts and
line timing, and its line side is a QEMU character device that owns the host device. Nothing
outside QEMU bridges the two.

Each USART is claimed by the chardev whose id is `<soc>.<usart>`: `soc.usart0` / `soc.usart1`
on `efr32bg22`, `soc0.usart1`, `soc1.usart0`, ... on `efr32bg22-dual`. Without one, SoC *n*'s
USART0 and USART1 take `-serial` 2*n* and 2*n*+1, QEMU's usual order. Any chardev backend
works:

| Give the USART             | Option                                              |
|----------------------------|-----------------------------------------------------|
| a host serial port         | `-chardev serial,id=soc.usart1,path=/dev/ttyUSB0`   |
| a new pty                  | `-chardev pty,id=soc.usart1` (QEMU prints its path) |
| a file (console capture)   | `-chardev file,id=soc.usart1,path=console.log`      |
| a socket                   | `-chardev socket,id=soc.usart1,path=u.sock,server=on,wait=off` |
| the Linux virtual HCI      | `-chardev vhci,id=soc1.usart1[,path=/dev/vhci]`     |

The USART tells a `serial` backend the guest's baud rate, parity, data and stop bits, so a real
port follows the firmware's configuration. Frames take their real duration in virtual time at
the programmed rate (start, data, parity and stop bits; PCLK from CMU.SYSCLKCTRL); while the
receiver is disabled the line is held, and once enabled, a frame completing into a full RX
buffer is lost with RXOF as on silicon. Not modelled: IrDA, I2S, parity errors, TX sequencer delays,
AUTOCS and secondary synchronous mode. The EUART is a separate model (docs/model-fidelity.md, ch. 21-22).

`efr32bg22_usart_tx` still traces every transmitted byte (now at the frame's stop bit) and
`efr32_usart_rx` every received one, so trace-based console reconstruction keeps working.

### Bluetooth controller on the host (`vhci`)

The `vhci` backend (`vendor/qemu/chardev/char-vhci.c`, Linux only) owns `/dev/vhci`. It is
silicon-independent: it splits the frontend's H4 byte stream into the one-packet-per-`write()`
form vhci takes and serialises host packets back into the stream, paced by what the UART
accepts. The kernel sees the emulated controller as it would a UART-attached one under
`btattach`: a new `hciN` that `bluetoothd`, `bluetoothctl` and bleak use directly. The
controller is created when the guest first enables its receiver, as a host would attach to a
booted controller: the kernel sends HCI_Reset on registration and allows HCI_CMD_TIMEOUT (2 s)
for each answer. The path can be a QEMU fd set (`-add-fd` + `path=/dev/fdset/N`), which is how
`tests/hci-uart-port/fake-vhci-scan.py` tests the whole chain without root.

`tests/hci-uart-port` is Zephyr's `hci_uart` sample on our controller (Zephyr's split link layer
over `modules/efr32-radio`), H4 on USART1 at 115200 baud and its console on USART0.
`tests/hci-uart-port/run-qemu-vhci.sh <device-image>` runs the device under test on soc0 and the
controller on soc1 with `/dev/vhci` passed through. Select the new adapter explicitly
(`bluetoothctl` `list`/`select`, bleak `adapter="hciN"`); the host's other controllers are not
touched. Access: `/dev/vhci` must be readable and writable by the user running QEMU.

### Timing against a real-time host

The guests run on `-icount` virtual time; BlueZ's HCI command (2 s), connection and ATT (30 s)
timeouts are real time. On-air timing (supervision timeouts, connection events) involves only
emulated parties and stays entirely virtual, so it is unaffected by host speed. Run with
`-icount shift=3,sleep=on`: idle periods then advance virtual time no faster than real time,
so a real-time host deadline always covers at least as much virtual time, and the emulated
controller can never time out a host that is merely slow in real terms. (`sleep=off` jumps
idle periods and ran 2.2x faster than real time on the dual machine.) Measured on a loaded
host (load average about 25 on 10 cores), dual machine, advertiser on soc0: each HCI command's
round trip over vhci was 100 to 270 ms, and up to 1 s for the first HCI_Reset during boot, all
inside HCI_CMD_TIMEOUT; with both SoCs busy virtual time ran at about 0.3x real time. Real
time is never fed into the guests: bytes from the host enter at the current virtual time.

## Debugging and flashing the simulator

The machine has the BG22's SWD debug port (SW-DP, AHB-AP, DCI, M33 halting
debug, MSC). OpenOCD connects through QEMU's socket probe
(`-device swd-remote-bitbang` and `scripts/sim/qemu-swd.cfg`) or a simulated
J-Link on a host USB device controller (`-object usb-gadget-host`, `-device
jlink`), and the flashing scripts run unchanged apart from the probe. See
`docs/debug-port-model.md`; `tests/debug-port/run-e2e.sh` rehearses the
hardware procedures.

## Status
* The current in-process model cleanly transfers from the bootloader reset vector to the stock
  application's reset vector. A bounded native-QEMU capture reached early radio initialization:
  it recorded 19 radio writes, including the eight `+0x004 = 1` setup accesses documented in
  `docs/radio-firmware-plan.md`. This does not establish sequencer loading, a RAC command, or a
  radio transaction. The retained `evidence/stock/radio-trace.log` remains historical register
  evidence from an older external socket bridge and must not be treated as native-model
  end-to-end validation.
* Native CPU tracing found and cleared one early boot dependency: stock application code at image
  file offsets `0x5e90..0x5e9c` (`0x0001be90..0x0001be9c` when loaded) enables DCDC control
  through the SET alias and waits for `DCDC.IF.REGULATION`. The public xG22 header names that
  bit 5 (`efr32bg22_dcdc.h:276-279`); the QEMU DCDC shim now presents it after control enable.
  A subsequent trace reaches the later HFXO setup sequence. Its read-only ready status is modeled
  with the public `RDY`, `COREBIASOPTRDY`, `ENS`, `HWREQ`, and `ISWARM` fields
  (`efr32bg22_hfxo.h:354-380`). This is boot-progress evidence only,
  not a claim that the native model reaches radio configuration beyond the bounded early setup
  capture or completes a radio transaction.
* Radio blocks have an in-process native register model with source- and trace-backed RAC,
  BUFC, PROTIMER, FRC, MODEM, AGC, SYNTH, and RFCRC register handling. BLE packet traffic,
  sequencer behavior, and air delivery remain unmodelled, so stock-firmware stimulation cannot
  yet be triggered from the outside. Identifying the on-chip dependency between native early
  setup and the historical extended RAIL configuration remains a separate validation item.
