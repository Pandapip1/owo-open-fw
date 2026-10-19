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
| `west.yml` | Pins Zephyr + `zephyr-lang-rust` and imports only the modules needed |
| `flake.nix` | flake-parts dev shell (+ a `checks.owo-core` host-test check) |

## Build
```
nix develop                              # Rust (thumbv8m.main targets), west, Arm GCC 13, cmake, ninja, dtc, libclang
git init -q && git add -A                # flakes only see tracked files; skip if this is already in a repo
west init -l . && west update            # workspace goes in the parent directory
west blobs fetch hal_silabs              # Silicon Labs' radio libraries
OWO_SERIAL=<your serial> west build -b xg24_dk2601b .
cargo test --manifest-path crates/owo-core/Cargo.toml     # or: nix flake check
```
`flake.lock` is not included (see "Not validated"); `nix` creates it on first use.
`xg24_dk2601b` is a stand-in dev kit: the suit's exact part is unknown (Series 2
per the firmware analysis).

## IMPORTANT: this build gives up deep sleep
`zephyr-lang-rust` refuses system clocks whose frequency is read at run time
(`RUST_SUPPORTED` needs `!TIMER_READS_ITS_FREQUENCY_AT_RUNTIME`; the crate also has a
`compile_error!` for it), and Zephyr's Silicon Labs sleeptimer driver selects exactly
that. So `prj.conf` switches the system timer to the Cortex-M SysTick
(`SILABS_SLEEPTIMER_TIMER=n`, `CORTEX_M_SYSTICK=y`). Silicon Labs' power manager needs
the sleeptimer (the link fails with undefined `sl_sleeptimer_*` otherwise), so
`CONFIG_PM=n` and the core idles in WFI only. **This works against the idle-power goal**
that motivated the project; the firmware stays tickless (no periodic wake-ups) but EM2/EM3
sleep is not used. Not measured. The alternative that keeps the sleeptimer is Rust logic
behind a thin C BLE shim (the `owo-core` crate is reusable for that).

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
  clean under Zephyr's `-D warnings` and mandatory `// SAFETY:` comments, ~126 KB flash / ~26 KB RAM.
  Done twice: with Ubuntu's gcc-arm-none-eabi 13.2.1, and from a fresh `west init -l` + `west
  update` workspace using this `west.yml`, inside `nix develop` (Arm GNU 13.3.Rel1).
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
