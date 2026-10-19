# OWO suit open firmware - proof of concept (Rust on Zephyr)

A Rust application on Zephyr that speaks the suit's GATT protocol, as
reverse-engineered from the 7.9.07 OTA image and the two Android apps (see
`myowo-ble-protocol.md` and `owo-suit-firmware-analysis.md`).

**Default build: it cannot stimulate anything.** Output goes through `Output` in `src/hal.rs`, which
logs unless `CONFIG_OWO_STIM_UNVALIDATED_HARDWARE=y`. With that switch on, the reusable
`owo-stim` module (below) drives the real output stage. **That path has never run on hardware.**
The pin map and sequencing were read out of firmware 7.9.07 (`docs/stock-stim-path.md`), not
taken from a schematic. Bench-test it with the checklist below before it goes near a person.

## Layout
| Path | What |
|---|---|
| `crates/owo-core/` | `no_std`, no-alloc, `forbid(unsafe_code)`: packet parsing (`proto.rs`) and the tickless pulse scheduler (`engine.rs`). Host-testable. |
| `src/lib.rs` | Shared state, application hooks, and the pump loop (sleeps on a semaphore until the engine's next wake-up; no periodic timer) |
| `src/ble.rs` | GATT table built at run time, pairing (passkey), advertising, connection handling |
| `src/hal.rs` | `Output`: dry-run log, or (opt-in) the `owo-stim` supervisor + xG22 driver |
| `modules/owo-stim/` | **Reusable Zephyr module** (Kconfig + Rust crate): `Hw` trait, safety `Stimulator`, EFR32xG22 backend, board pin map. See below |
| `docs/stock-stim-path.md` | What the stock firmware's output stage does, its interlocks, and the ones it lacks |
| `build.rs` | Generates bindings for `conn.h` / `gatt.h` (see "Why a bindgen step") and bakes in `OWO_SERIAL` |
| `owo_wrapper.h` | Header for that bindgen run |
| `patches/`, `scripts/apply-patches.sh` | Local patch to zephyr-lang-rust (see above) and its applier |
| `west.yml` | Pins Zephyr + `zephyr-lang-rust` and imports only the modules needed |
| `flake.nix` | flake-parts dev shell (+ a `checks.owo-core` host-test check) |

## Build
```
nix develop                              # Rust (thumbv8m.main targets), west, Arm GCC 13, cmake, ninja, dtc, libclang
git init -q && git add -A                # flakes only see tracked files; skip if this is already in a repo
west init -l . && west update            # workspace goes in the parent directory
./scripts/apply-patches.sh               # required: lets zephyr-lang-rust coexist with the Silabs sleeptimer
west blobs fetch hal_silabs              # Silicon Labs' radio libraries
OWO_SERIAL=<your serial> west build -b bg22_ek4108a .      # dry run (no output)
# bench only:  ... -- -DCONFIG_OWO_STIM_UNVALIDATED_HARDWARE=y
cargo test --manifest-path crates/owo-core/Cargo.toml      # protocol + scheduler
cargo test --manifest-path modules/owo-stim/rust/Cargo.toml   # safety supervisor; or: nix flake check
```
`flake.lock` is not included (see "Not validated"); `nix` creates it on first use.
`bg22_ek4108a` is a stand-in dev kit (EFR32BG22, the same family as the suit; the firmware's peripheral
addresses and vector table match xG22). The suit's exact part number is unknown.

## The `owo-stim` module (output stage)
`modules/owo-stim/` is a Zephyr module with its own `Kconfig` and a `no_std` Rust crate, usable by any
application: add the directory to `ZEPHYR_EXTRA_MODULES` and depend on `modules/owo-stim/rust`.
- `hw::Hw`: the hardware interface (bridge, DAC, channel switch, sense, supply, delays, IRQ lock).
- `stim::Stimulator`: every safety decision, hardware-independent, 19 host tests against a mock.
- `xg22` (feature `xg22`): the EFR32xG22 backend for the pin map in `board::OWO_SUIT`. GPIO bit-bang for
  both SPI buses and the I2C expander; TIMER1 PWM for the boost; IADC for the feedback; Silicon Labs
  emlib/clock-manager bindings generated at build time (`modules/owo-stim/rust/build.rs`).
- Interlocks (see `docs/stock-stim-path.md` for which the stock firmware has and which are new): hard
  level ceiling (default 40 of 126), coverage window + dead-man, soft start, per-channel duty budget,
  phase/gap limits, charge-balanced phases, fault latch + lockout, supply off when idle, bounded
  IRQ-off window, ADC sense timeout, break-before-make bridge, switch read-back.
- Kconfig: `OWO_STIM_UNVALIDATED_HARDWARE` (the opt-in), `OWO_STIM_HARD_MAX_LEVEL`,
  `OWO_STIM_DEADMAN_MS`, `OWO_STIM_MAX_PHASE_US`, `OWO_STIM_SOFT_START_MS`, `OWO_STIM_MAX_DUTY_PERMILLE`,
  `OWO_STIM_WIDTH_SCALE_PERCENT`, `OWO_STIM_FAULT_LOCKOUT`.

### Bench checklist (before any skin contact)
1. Dummy load only (resistor network, a few kOhm per channel), current-limited bench supply in place of the
   battery, scope on a channel pair, a way to cut power.
2. Build with the opt-in; confirm the log says "REAL stimulation backend enabled" and `Xg22::new` succeeded.
3. Before connecting anything: scope PB0..PB3 with the firmware idle. Expect PB2/PB3 high, PB0/PB1 low.
   Probe PC7 (boost PWM) idle: low. Probe the DAC output: ~0 V.
4. Level 1-5 on one channel. Check: phase length (compare to `CONFIG_OWO_STIM_WIDTH_SCALE_PERCENT`: the
   stock tick suggests 2x the table value), gap, polarity order, that there is never overlap between phases
   or both high sides on, and that the output returns to 0 V between pulses.
5. Compare the measured amplitude with the commanded level at several levels to check the sense scaling
   (`SENSE_NUM/SENSE_DEN` in `xg22.rs`; the stock firmware also applies a RAM trim table that is not used here).
6. Pull the BLE link and stop sending frames: output must stop within the coverage window + dead-man.
7. Force a fault (shunt the sense input high, or disconnect the load at level 20): the channel must latch
   and the supply must turn off; three faults must lock the whole output until Start.
8. Only then consider a person, and then start at the lowest level with the `HARD_MAX_LEVEL` ceiling in
   place. This is an electrical device that crosses the skin: stop immediately if anything is unexpected.

## Rust together with the Silicon Labs sleeptimer (needs a patch)
Stock `zephyr-lang-rust` refuses systems whose timer reads its frequency at run time:
`RUST_SUPPORTED` requires `!TIMER_READS_ITS_FREQUENCY_AT_RUNTIME` and `zephyr/src/time.rs` has a
`compile_error!` for it, added as a precaution ("until this support can be added"). Zephyr's Silicon
Labs sleeptimer driver selects that option, and Silicon Labs' power manager (EM2/EM3 sleep) needs the
sleeptimer (without it the link fails on `sl_sleeptimer_*`).

What the option actually changes: only the *hardware cycle* frequency becomes a run-time value
(`silabs_sleeptimer_timer.c` reads `sl_sleeptimer_get_timer_frequency()` and derives
`cyc_per_tick = hz / CONFIG_SYS_CLOCK_TICKS_PER_SEC`). Kernel *ticks* per second stay the compile-time
`CONFIG_SYS_CLOCK_TICKS_PER_SEC` (32768 here), which is all the Rust `time` module uses
(`SYS_FREQUENCY`, `Duration`, `Instant`, `Timeout`); nothing in the crate converts cycles. The one hazard is
code that converts cycle counts, because `CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC` is then not a usable
value (0 by default; on this SoC the defconfig overrides it to 32768, which would be stale if the
hardware frequency differs). The patch therefore makes `zephyr::kconfig::CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC`
a compile error whenever `TIMER_READS_ITS_FREQUENCY_AT_RUNTIME` (or
`SYSTEM_CLOCK_HW_CYCLES_PER_SEC_RUNTIME_UPDATE`) is set, and adds `zephyr::time::hw_cycles_per_sec()`,
which calls the kernel's run-time getter (or reads the Kconfig value when it is a constant). The constant
becomes a value of a type named `HwCyclesPerSecIsARuntimeValue_UseZephyrTimeHwCyclesPerSec`, so the error
reads e.g. ``non-primitive cast: `HwCyclesPerSecIsARuntimeValue_UseZephyrTimeHwCyclesPerSec` as `f64` ``.
The `bench` and `embassy` samples in zephyr-lang-rust use the Kconfig value and would now stop compiling
on such targets (they could not be built there before, since Rust was disabled).

`patches/0001-*.patch` removes both guards and adds that check (if you applied the earlier version of this patch, run `git -C ../modules/lang/rust checkout .` first). `scripts/apply-patches.sh` applies it to the west-managed
checkout (idempotent; re-run after every `west update`). It is a local patch, not upstream. With it,
`prj.conf` keeps the sleeptimer and `CONFIG_PM=y`. Without it, Kconfig reports
`RUST ... was assigned the value 'y' but got the value 'n'`.

Not shown by any of this: that the core actually reaches EM2/EM3 while connected and idle. That needs
hardware and the Silicon Labs blobs; the energy-mode requirements the BLE stack holds are unknown.

## Why a bindgen step
`zephyr-sys` binds only `<zephyr/bluetooth/bluetooth.h>`; `bt_conn_*` and `bt_gatt_*` are
missing. `build.rs` generates them with the include paths, defines and clang target the Zephyr
build already passes to `zephyr-sys`. Generated, not hand-copied, because layouts drift: in this
Zephyr, `bt_gatt_attr.perm` is a 15-bit bitfield, which is why the attribute tables are filled in
at start-up rather than in a `const`. `static inline` functions are not bound (they need a
compiled wrapper), so e.g. `bt_gatt_notify_cb` is used instead of `bt_gatt_notify_uuid`.

## What was validated
- `owo-stim`: 19 host tests pass (supervisor against a mock output stage: bridge ordering, clamps, overdrive trip at the early and the mid-pulse sample, switch-verify failure, lockout, coverage window and dead-man, soft start, duty budget, open-circuit hint). Not run on hardware.
- Both configurations compile and link for `bg22_ek4108a` (blobs excluded): dry-run ~129 KB flash / ~27 KB RAM, `OWO_STIM_UNVALIDATED_HARDWARE=y` ~136 KB. The emlib bindings (GPIO, TIMER, IADC, clock manager) generate and the symbols are in the ELF.
- Channel switch table and the pin/DAC/ADC details in `docs/stock-stim-path.md` were obtained by reading the stock code and, for the table, by emulating its start-up with `unicorn`.
- `owo-core`: 13 host tests pass (the vectors from the firmware analysis: frame/config/power/
  widths/pairing, plus scheduler behaviour: 100 wake-ups for a 500 ms, 100 Hz, 2-channel frame
  where a 1 kHz tick would need 500). Also via `nix flake check`.
- Compile and link only: Zephyr `a915b2f` (4.5.0-rc2), board `bg22_ek4108a`, Rust 1.99.1, clippy
  clean under Zephyr's `-D warnings` and mandatory `// SAFETY:` comments, ~126 KB flash / ~26 KB RAM with the earlier SysTick configuration.
  Done twice: with Ubuntu's gcc-arm-none-eabi 13.2.1, and from a fresh `west init -l` + `west
  update` workspace using this `west.yml`, inside `nix develop` (Arm GNU 13.3.Rel1).
- With the patch: Rust + `SILABS_SLEEPTIMER_TIMER=y` + `TIMER_READS_ITS_FREQUENCY_AT_RUNTIME=y` + `PM=y` +
  `TICKLESS_KERNEL=y` compiles and links (~134 KB flash / ~27 KB RAM), from a fresh `west init -l` +
  `west update` workspace plus `scripts/apply-patches.sh`; without it, Rust is disabled by Kconfig.
  Also checked: `hw_cycles_per_sec()` builds in that configuration; using the Kconfig constant fails to
  compile with the error above; with SysTick (option off) the constant is numeric again (78000000) and
  both forms compile. The patch applies cleanly to the pristine pinned revision.
- The 128-bit UUIDs are present in the linked ELF in little-endian byte order.

## Not validated
- **Not runnable:** the builds used Zephyr's CI-only `CONFIG_BUILD_ONLY_NO_BLOBS=y`, because
  `west blobs fetch hal_silabs` was refused (HTTP 403) where this was built.
- No on-air behaviour: pairing, attribute permissions, advertising, or whether the official apps
  accept it. Attribute handles differ from stock (apps look up by UUID).
- `flake.lock` is omitted: GitHub tarball downloads were refused in the build environment, so the
  flake was evaluated against local clones of nixpkgs, flake-parts and rust-overlay instead.
  The nixpkgs default `gcc-arm-embedded` (15.3) segfaulted in that sandbox, so the flake pins
  `gcc-arm-embedded-13`; even that exact package from the newer nixpkgs revision segfaulted there,
  while the same version from an older revision ran, so the final build above used the older one.
  I don't know whether that is the sandbox or the package; check `arm-none-eabi-gcc --version` in
  your shell.
- The `BINDGEN_EXTRA_CLANG_ARGS` workaround in `flake.nix` (skip GCC's `arm_acle.h`, which clang
  rejects) was needed with both toolchains tried; it is a workaround, not a fix.
- **The output stage (`xg22.rs`) is unvalidated on hardware.** Open issues: expander P2/P3 and PA6 purpose; whether the SPI switch tolerates the slower bit-banged clock; sense calibration; the real pulse width (2x question); behaviour between reset and `Xg22::new`; whether `critical()` (PRIMASK off for <= ~0.5 ms) disturbs the BLE controller; the flake does not yet test `owo-stim`.
- Idle power: nothing was measured.
- Placeholders: battery level fixed at 100 (Zephyr's BAS default); channel status always "all open";
  the Extra 2 record after 0x25 is from the app decode; the config packet is parsed and stored but
  has no effect; engine timing is an interpretation, not a copy (channel stagger 1 ms to leave room for bus and pulse time); `OWO_SERIAL` defaults to a dummy
  and which string the official app hashes is unconfirmed (the passkey is logged at boot).

## Flashing a real suit
Unknown and risky: whether the stock bootloader accepts a third-party GBL, and the OTP secure-boot
and debug-lock state, are unverified. A wrong move can brick the unit. Keep the stock image backed
up first (needs SWD access).
