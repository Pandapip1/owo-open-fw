# The suit's control unit as a Zephyr board (`owo_suit`)

**Status: builds; never run on a suit.** Everything here comes from the unit's own flash,
USERDATA and DEVINFO pages (read over SWD on 2026-10-31) and from static analysis of the stock
firmware in that flash. Evidence tags as in `stock-stim-path.md`: **[F]** read from the dump,
**[I]** inferred, **[M]** needs a measurement on the unit. The detailed notes (with stock code
locations) are kept outside this repository, next to the dump.

```
west build -b owo_suit .                  # or plain cmake with -DBOARD=owo_suit
west build -b owo_suit . -- -DOWO_HW=ON   # bench only, as for the kit
```
The board definition is out of tree (`boards/owo/owo_suit/`, found through `BOARD_ROOT` in
`CMakeLists.txt`); `boards/owo_suit.overlay` and `.conf` add what every board of the product gets
(`boards/owo_product.dtsi`), and `boards/owo_suit_hw.overlay` describes the output stage.

## What the unit is
- **A BGM220PC22WGA module, not a bare SoC.** DEVINFO's module name field reads `BGM220PC22WGA2`;
  the die is an EFR32BG22C222F352GM40 (DEVINFO part number C222, 352 KiB flash, 32 KiB RAM,
  QFN40). **[F]** Zephyr already has this module as an SoC (`bgm220pc22wga`, series `bgm22`), so
  the board selects it; no SoC or dts additions in Zephyr were needed. The open radio module
  already accepts the BGM22 series.
- Module information in DEVINFO: PCB module with built-in antenna, **no LFXO crystal**, HFXO
  calibration **valid**, LFXO calibration not valid, DCDC pad wired to VDCDC. **[F]**

## Clocks and power
| Item | Unit / stock firmware | Board |
|---|---|---|
| HFXO | 38.4 MHz crystal inside the module. Stock firmware takes CTUNE from DEVINFO when the module marks it valid (it does: 0x77 = 119 for both XI and XO on this unit), else from the USERDATA manufacturing token (erased on this unit), else 140. **[F]** | Inherited from the module dtsi. Zephyr's clock manager applies the same order (DEVINFO, USERDATA token, devicetree `ctune`), so the unit's DEVINFO value is used and `ctune = <140>` is only the last fallback, as in the stock firmware. |
| LF clock | No LFXO on the module **[F]**; the stock firmware puts the EM2/EM4 clock groups, WDOG0 and RTCC on the LFRCO in precision mode (calibrated against the HFXO). **[F]** | Same: every LF clock is the LFRCO (xG22 default) and the module dtsi enables precision mode. The board never enables the LFXO. |
| SYSCLK | Stock runs the CPU and EM01GRPACLK straight from the HFXO at 38.4 MHz. **[F]** | HFRCODPLL locked to the HFXO at 76.8 MHz, exactly the stand-in kit's clock tree, because the radio and scheduler timings were calibrated there. A deliberate difference from stock. |
| DCDC | Buck, Silicon Labs' default initialisation (regulation on, 2.3 V input comparator, 1.19 us Ton max, DCM only, 60 mA EM0/1 and 5 mA EM2/3 peak current). **[F]** | `regulator-boot-on`, buck: Zephyr runs the same default initialisation. |

Consequences to check on the unit **[M]**:
- The LFRCO in precision mode is specified to about +-500 ppm, against the kit's LFXO. The open
  link layer already advertises the worst sleep-clock accuracy (500 ppm), so Bluetooth timing is
  covered; the BURTC clock-event timings and the scheduler's handover between BURTC and TIMER0
  were measured with the LFXO and need measuring again on the LFRCO.
- Radio timing (T_IFS etc.) was calibrated on the kit with the same 76.8 MHz tree and the same
  38.4 MHz crystal frequency; it should carry over, but the module's crystal and CTUNE differ.

## Flash map
Read from the unit (352 KiB, 8 KiB pages):

| Range | Content | Board partition |
|---|---|---|
| 0x00000-0x06000 | Gecko Bootloader (13.6 KB used). Its table declares the application space as 0x6000-0x58000. **[F]** | `boot_partition`, read-only |
| 0x06000-0x16000 | AppLoader (Bluetooth OTA), up to about 0x14250 **[F]** | `apploader_partition`, read-only |
| 0x16000-0x4C000 | Stock application (ends at 0x4AA6E) **[F]** | `slot0_partition`, 216 KiB: the product links here |
| 0x4C000-0x56000 | Stock NVM3, five pages, all formatted, the first holding data **[F]** | `stock_nvm3_partition`, read-only |
| 0x56000-0x58000 | Erased; no reference to it in the bootloader or the stock application **[F]** | `storage_partition`, 8 KiB |

**Storage decision.** The product's storage page is the last page, 0x56000. It is the only flash
that neither the bootloader, the AppLoader nor the stock firmware uses, so the open firmware
never touches stock data: the stock NVM3 (settings, and possibly calibration) stays intact for a
stock firmware reinstalled by OTA, and an OTA image cannot reach it (the AppLoader only rewrites
the pages the image covers; the 216 KiB application partition ends where the NVM3 starts and the
link fails beyond it). The product does not use storage today (`CONFIG_BT_SETTINGS=n`). One page
is enough for a raw record but not for NVS or ZMS, which need at least two sectors (they keep one
erased for garbage collection), nor for the settings subsystem on them. The build enforces it:
with `CONFIG_NVS`, `CONFIG_ZMS` or `CONFIG_SETTINGS` (which `CONFIG_BT_SETTINGS` needs) and a
`storage_partition` of fewer than two erase pages, the configure step fails and points here
(`CMakeLists.txt`).

Decision path if the product is to store bonds (a product decision; nothing is moved today):
1. A backend needs two contiguous pages, and only one is free: the application partition ends
   where the stock NVM3 starts (0x4C000), and 0x56000 is the last page.
2. Preferred: the top two pages of the application partition, 0x48000-0x4C000, become
   `storage_partition` (the application partition shrinks from 216 to 200 KiB; the images today
   use 135 to 155 KiB of it), and 0x56000 stays unused. The stock NVM3 stays intact. A stock
   application reinstalled by OTA (it ends at 0x4AA6E) overwrites those pages, which only loses
   the open firmware's bonds; an open-firmware OTA image cannot reach them (the link fails past
   its partition).
3. Not without more work: pages of the stock NVM3 (0x4C000-0x56000) joined to 0x56000. Taking
   over the NVM3 is acceptable only after its contents are understood and backed up, since a
   stock firmware reinstalled later would find it changed.
4. Not recommended: a raw record in the single page without a Zephyr backend; erasing its only
   page loses the bond on a power failure.

## Serial number and USERDATA
- USERDATA holds the unit's record: the serial `022529200116` as twelve ASCII digits at offset
  0x1C, preceded and followed by what look like checksums and calibration values; the advertised
  stock name is `OWO` + those digits. The HFXO manufacturing-token slot (offset 0x100) is erased.
  **[F]**
- The stock firmware copies the first 160 bytes of USERDATA into RAM at start-up and has a routine
  that erases the page and writes the record back. **[F]** The open firmware must never write the
  page (it does not: nothing in the product writes outside its own image).
- The firmware reads the serial from the record at run time: the board describes it in
  devicetree (`serial_record`, compatible `owo,serial-record`, `reg = <0x0fe0001c 12>`), and
  `src/serial.c` accepts it only as twelve ASCII digits (an erased page reads 0xFF). The advertised
  and GAP name (`"OWO"` and the digits, `src/ble.rs`, `CONFIG_BT_DEVICE_NAME_DYNAMIC`: +16 B of
  RAM) and the passkey (`src/lib.rs`) derive from it, so one OTA image serves every unit and
  neither can disagree with the unit's record. A board without the node, or a record that fails
  the check, falls back to the build's `OWO_SERIAL` (`build.rs`). QEMU, `owo_suit` at 0x16000 with
  `userdata=` the unit's USERDATA dump: the device name in `bt_dev` reads `OWO022529200116`;
  with the model's erased USERDATA, `OWO000000000000` (the default `OWO_SERIAL`). **[E]**

## Output stage: safe state from reset
What the stock firmware does **[F]**: its board initialisation drives PB0-PB3 low (all four
bridge gates off), PC7 low (boost PWM pin), PA5 and PA6 high (DAC select and the second select),
PC3 high (switch latch), and the expander's P2/P3 low; it then calls its bridge routine in the
"off" mode, which is PB0/PB1 low and PB2/PB3 high: both high sides off, both low sides on. The
boost PWM duty is 0 whenever stimulation is not running.

What the board does: GPIO hogs (applied when the GPIO driver starts, in every build, also the
default one that never touches the output stage again) put
- PB0, PB1 (high sides) low and PB2, PB3 (low sides) high: the stock "off" state, which is also
  the state `owo-stim`'s reset safe-off and `Stimulator::safe_off` enforce;
- PC7 (boost) low, so the converter cannot switch;
- PA5, PA6 and PC3 high (deselected / latched), as stock.

PC4 is the expander's open-drain /INT, not a button (`stock-stim-path.md`); the board no longer
describes it as one. `boards/owo_suit_hw.overlay` gives it to the expander driver (`int-gpios`)
and the power button, expander P0, to the output stage as its stop input (`stop-gpios`,
`stim-simulation.md` finding 1).

Limits of this:
- From reset until the hogs run the pins are high impedance (GPIO reset state), through the Gecko
  Bootloader and Zephyr's early init. Whether the gate drivers and the boost stage are pulled off
  in that window is a property of the PCB. **[M]**
- The gate polarity (high = on) is read from the stock code, not from a schematic. **[I]**
- The expander's outputs come out of reset weakly high (PCF8575 behaviour), so P2/P3 (the supply
  enables, as the product reads them) are high until a firmware writes the expander; the default
  build never does. What P2/P3 switch is unknown. **[M]**
- The stock firmware checks a hardware variant (two expander inputs) and drives either expander
  P14/P15 or PC5/PC6 accordingly; their purpose and this unit's variant are unknown, so the board
  leaves them alone. **[M]**

## QEMU smoke boot
The `efr32bg22` machine still models a 512 KiB part with a BGM220 module's DEVINFO, so the run below
pads the unit's 352 KiB flash image with erased bytes to 512 KiB; nothing in it reaches beyond
0x58000. The image is the unit's own flash (Gecko Bootloader, AppLoader, stock NVM3) with the
application area replaced by the default `owo_suit` build. **[E]**
- As built, the bootloader starts the AppLoader and the AppLoader stays in its OTA mode: the open
  firmware never runs, because it carries no ApplicationProperties_t (`ota.md`). With the stock
  application in place the bootloader hands over at once.
- With an ApplicationProperties_t added to the image after the build (unsigned, type Bluetooth
  application, vector-table word 13 pointing at it) the open firmware boots once, with no reset:
  the GPIO hogs write DOUT before switching each pin to push-pull (PB0/PB1 cleared, PB2/PB3 set,
  PC3 set, PC7 cleared, then the modes), and the radio runs repeated TX sequences (advertising).
  The model reports only reads of the CPU's debug status register (DHCSR), which it does not
  implement.
- The product now carries that descriptor (`src/gecko_app_properties.c`, `ota.md`); the default
  `owo_suit` build starts straight from the unit's bootloader and runs the radio. **[E]**

## Build numbers (2026-10-31, with the budget settings in `prj.conf`)
| Board | Flash | RAM (of 32 760 B) | Reserve check (31 000 B) |
|---|---|---|---|
| `bg22_ek4108a` | 143 068 B | 29 972 B (91.5 %) | passes, 2 788 B free |
| `bg22_ek4108a`, `-DOWO_HW=ON` | 156 316 B | 30 988 B (94.6 %) | passes, 1 772 B free |
| `owo_suit` | 137 812 B of 221 184 B (62.3 %) | 29 588 B (90.3 %) | passes, 3 172 B free |
| `owo_suit`, `-DOWO_HW=ON` | 156 324 B of 221 184 B (70.7 %) | 30 972 B (94.5 %) | passes, 1 788 B free |

What made the output-stage image fit (each number measured as the linker's RAM total before and
after): four PSA key slots instead of sixteen (-1 248 B; the bound is argued in `prj.conf`), the
radio HAL's BUFC TX FIFO and scratch PDU sized for the largest PDU this configuration stages
instead of RAIL's 512 bytes and a 251-octet data PDU (-672 B, `hal/radio.c`), and the IADC
driver's channel table cut to the one channel the output stage reads, without the unused DMA
state (-192 B, `owo-suit-hw.conf`). Thread stacks are unchanged: most of their worst-case
workloads (pairing, GATT writes, encrypted connections, a running output stage) cannot be driven
in QEMU yet, so they keep their sizes and the reserve.

**Not yet counted: pairing needs a heap.** The image has no mbedtls heap and no CCM, so AES-CMAC,
the ECB session key and link-layer CCM fail (see `prj.conf`); LE Secure Connections pairing and
encryption cannot complete. Adding them costs about 1.4 KB of RAM (measured per operation, four
contexts at once), which would leave the output-stage images below the 1 760 B reserve again,
unless the AES work moves to the BG22's AES hardware or stacks are sized from measurements of
those workloads.

Before the budget settings (LTO, no printk, no controller privacy or filter list, no fault dump,
no GATT caching, no vendor HCI) the kit needed 192 708 B / 33 068 B (RAM over by 308 B) and the
suit 186 284 B / 32 612 B; `-DOWO_HW=ON` was 1 444 B over. With those settings alone the kit
needed 31 956 B, the suit 31 500 B and the suit with `-DOWO_HW=ON` 33 084 B (324 B over).
