# OWO suit open firmware - proof of concept (Rust on Zephyr)

A Rust application on Zephyr that speaks the suit's GATT protocol, as
reverse-engineered from the 7.9.07 OTA image and the two Android apps (see
`myowo-ble-protocol.md` and `owo-suit-firmware-analysis.md`).

**It cannot stimulate anything.** The only output backend is `src/hal.rs`
(`DryRun`), which logs. The suit's electrode hardware (pins, drivers, boost
stage, current limits) is unidentified. Do not write a real backend until the
board is identified and measured on a dummy load.

## Layout
| Path | What |
|---|---|
| `crates/owo-core/` | `no_std`, no-alloc, `forbid(unsafe_code)`: packet parsing (`proto.rs`) and the tickless pulse scheduler (`engine.rs`). Host-testable. |
| `src/lib.rs` | Shared state, application hooks, and the pump loop (sleeps on a semaphore until the engine's next wake-up; no periodic timer) |
| `src/ble.rs` | GATT table built at run time, pairing (passkey), advertising, connection handling |
| `src/hal.rs` | Output abstraction + dry-run backend |
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
OWO_SERIAL=<your serial> west build -b xg24_dk2601b .
cargo test --manifest-path crates/owo-core/Cargo.toml     # or: nix flake check
```
`flake.lock` is not included (see "Not validated"); `nix` creates it on first use.
`xg24_dk2601b` is a stand-in dev kit: the suit's exact part is unknown (Series 2
per the firmware analysis).

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
- `owo-core`: 13 host tests pass (the vectors from the firmware analysis: frame/config/power/
  widths/pairing, plus scheduler behaviour: 100 wake-ups for a 500 ms, 100 Hz, 2-channel frame
  where a 1 kHz tick would need 500). Also via `nix flake check`.
- Compile and link only: Zephyr `a915b2f` (4.5.0-rc2), board `xg24_dk2601b`, Rust 1.99.1, clippy
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
- Idle power: nothing was measured.
- Placeholders: battery level fixed at 100 (Zephyr's BAS default); channel status always "all open";
  the Extra 2 record after 0x25 is from the app decode; the config packet is parsed and stored but
  has no effect; engine timing is an interpretation, not a copy; `OWO_SERIAL` defaults to a dummy
  and which string the official app hashes is unconfirmed (the passkey is logged at boot).

## Flashing a real suit
Unknown and risky: whether the stock bootloader accepts a third-party GBL, and the OTP secure-boot
and debug-lock state, are unverified. A wrong move can brick the unit. Keep the stock image backed
up first (needs SWD access).
