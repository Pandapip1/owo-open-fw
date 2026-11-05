{
  description = "OWO suit open firmware (proof of concept): Rust on Zephyr, Silicon Labs Series 2";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # nixpkgs unstable dropped Intel Darwin in 26.11. Keep that host usable
    # from the same flake while it remains supported by the 26.05 branch.
    nixpkgs-darwin-x86.url = "github:NixOS/nixpkgs/nixpkgs-26.05-darwin";
    flake-parts.url = "github:hercules-ci/flake-parts";
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];

      perSystem =
        { pkgs, system, ... }:
        let
          # Cortex-M33 with FPU (thumbv8m.main-none-eabihf) is what zephyr-lang-rust picks
          # for the Series 2 boards; the soft-float target is listed for configs without FPU.
          # native_sim (tests/native-sim) builds for the host's own bare-metal target.
          rustToolchain = pkgs.rust-bin.stable.latest.default.override {
            targets = [
              "thumbv8m.main-none-eabihf"
              "thumbv8m.main-none-eabi"
              "${pkgs.stdenv.hostPlatform.parsed.cpu.name}-unknown-none"
            ];
            extensions = [
              "rust-src"
              "clippy"
              "rustfmt"
            ];
          };

          # Pinned to the 13 series: that is the family the build was validated with.
          # (gcc-arm-embedded 15.3 from nixpkgs segfaulted when run in the sandbox this
          # was developed in, so it is untested.)
          armGcc = pkgs.gcc-arm-embedded-13;

          # OpenOCD master contains the Silicon Labs Series-2 DCI/MSC support
          # needed to erase and program the BGM220. Keep this in the flake so
          # Linux and Darwin hosts use the same audited debugger revision.
          openocd = pkgs.openocd.overrideAttrs (old: {
            version = "0.12.0-unstable-2026-10-24";
            src = pkgs.fetchFromGitHub {
              owner = "openocd-org";
              repo = "openocd";
              rev = "cb0c224e936091156ecf3025ba05657408ed64c8";
              hash = "sha256-RKDOs7CaTAc61Ae6TPaG9oG8zHYfL62DvZXIxvXmGgA=";
            };
            # Project fixes, each written to go upstream (see the patch headers).
            patches = (old.patches or [ ]) ++ [
              ./nix/openocd/0001-target-cortex_m-debug-entry-for-a-core-found-halted-from-unknown.patch
            ];
            nativeBuildInputs = (old.nativeBuildInputs or [ ]) ++ [
              pkgs.autoconf
              pkgs.automake
              pkgs.libtool
              pkgs.which
            ];
            preConfigure = ''
              ./bootstrap nosubmodule
            '';
            meta = old.meta // {
              platforms = [
                "x86_64-linux"
                "aarch64-linux"
                "x86_64-darwin"
                "aarch64-darwin"
              ];
            };
          });

          # Python modules west and Zephyr's build scripts import: scripts/requirements-base.txt
          # minus the compliance-only entries (reuse, spdx-python-model).
          python = pkgs.python3.withPackages (
            ps: with ps; [
              west
              pyelftools
              pyyaml
              pykwalify
              jsonschema
              canopen
              cryptography
              # QEMU's configure wrapper uses distlib to provision its
              # non-isolated Meson helper environment.
              distlib
              packaging
              patool
              psutil
              pylink-square
              pyserial
              requests
              semver
              tqdm
              anytree
              tabulate
              intelhex
              # Twister (scripts/twister): what scripts/requirements-build-test.txt and
              # requirements-run-test.txt list for building and running tests on
              # native_sim and QEMU.
              colorama
              natsort
              ply
              junitparser
              python-dotenv
              pytest
            ]
          );
        in
        {
          _module.args.pkgs = import (
            if system == "x86_64-darwin" then
              inputs.nixpkgs-darwin-x86
            else
              inputs.nixpkgs
          ) {
            inherit system;
            overlays = [ inputs.rust-overlay.overlays.default ];
          };

          devShells.default = pkgs.mkShell {
            packages = [
              rustToolchain
              python
              armGcc
              openocd
              pkgs.cmake
              pkgs.ninja
              # Host dependencies for the pinned QEMU submodule.  Keeping
              # these here makes the simulator build reproducible through
              # `nix develop`, alongside the firmware toolchain.
              pkgs.meson
              pkgs.pkg-config
              pkgs.glib
              pkgs.pixman
              pkgs.zlib
              pkgs.libffi
              pkgs.libslirp
              pkgs.dtc
              pkgs.gperf
              pkgs.git
              pkgs.llvmPackages.libclang
            ];

            # The host compiler wrapper would add -D_FORTIFY_SOURCE to every native_sim
            # build. Zephyr's native simulator runner is compiled without optimization,
            # where glibc warns that fortification needs -O; twister builds with warnings
            # as errors, so every native_sim scenario failed to link.
            hardeningDisable = [ "fortify" ];

            env = {
              # Zephyr: use a plain GNU Arm Embedded toolchain instead of the Zephyr SDK.
              ZEPHYR_TOOLCHAIN_VARIANT = "gnuarmemb";
              GNUARMEMB_TOOLCHAIN_PATH = "${armGcc}";

              # zephyr-sys and this crate's build.rs run bindgen, which needs libclang.
              LIBCLANG_PATH = "${pkgs.llvmPackages.libclang.lib}/lib";

              # Workaround, still needed with this shell's 13.3 toolchain: the Zephyr build passes
              # the GCC include directory to bindgen's clang with -I, and CMSIS 6's
              # cmsis_clang.h (what clang selects) includes <arm_acle.h>, so clang parses GCC's
              # copy and rejects its coprocessor intrinsics (build.rs fails without this).
              # Skipping that header leaves __rbit/__clz/__isb undeclared in clang's view; they
              # are only used inside inline functions that bindgen does not bind.
              BINDGEN_EXTRA_CLANG_ARGS = "-D_GCC_ARM_ACLE_H -Wno-error=implicit-function-declaration";
            };

            shellHook = ''
              ${pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isDarwin ''
                # QEMU's macOS post-link step invokes Rez through xcrun.  The
                # Nix xcrun wrapper otherwise points at its SDK, which does
                # not carry that Apple-only resource compiler.
                export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
              ''}
              # West wants the Zephyr tree; `west init -l . && west update` puts it next to this repo.
              echo "OWO open firmware dev shell"
              echo "  first time:  west init -l . && west update && west blobs fetch hal_silabs"
              echo "  build:       west build -b bg22_ek4108a ."
              echo "  QEMU:        mkdir -p build/qemu && (cd build/qemu && ../../vendor/qemu/configure --target-list=arm-softmmu --disable-docs --disable-capstone --enable-slirp)"
              echo "               ninja -C build/qemu qemu-system-arm"
              echo "  twister:     ZEPHYR_BASE=\$PWD/vendor/zephyr QEMU_BIN_PATH=\$PWD/build/qemu vendor/zephyr/scripts/twister \\"
              echo "                 -x=ZEPHYR_MODULES=\$PWD/vendor/cmsis_6 \\"
              echo "                 -p native_sim/native/64 -p qemu_cortex_m3 -p mps2/an385 -T vendor/zephyr/tests/kernel/sched/deadline"
              echo "               (no west workspace)"
              echo "  host tests:  cargo test --manifest-path crates/owo-core/Cargo.toml"
              echo "               cargo test --manifest-path modules/owo-stim/rust/Cargo.toml"
            '';
          };

          packages.openocd = openocd;

          # Standalone ARMv6-M firmware for the BG22 radio sequencer core.
          # This deliberately has its own output rather than being linked as
          # M33 code: hardware boots it from private SEQRAM at local address 0.
          packages.sequencer-probe = pkgs.stdenvNoCC.mkDerivation {
            pname = "owo-bg22-sequencer-probe";
            version = "0.1.0";
            src = pkgs.lib.cleanSourceWith {
              src = ./tests/sequencer-probe;
              filter = path: type:
                let name = baseNameOf path;
                in !(type == "directory" && pkgs.lib.hasPrefix "build" name);
            };
            nativeBuildInputs = [ armGcc ];
            dontConfigure = true;
            buildPhase = ''
              runHook preBuild
              mkdir build
              arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -nostdlib \
                -Wa,-defsym,STOP_EXCEPTION=0 \
                -Wl,--build-id=none -Wl,-T,$src/link.ld \
                $src/probe.S -o build/sequencer.elf
              arm-none-eabi-objcopy -O binary \
                build/sequencer.elf build/sequencer.bin
              runHook postBuild
            '';
            installPhase = ''
              runHook preInstall
              mkdir -p $out/share/owo-bg22-sequencer
              cp build/sequencer.elf build/sequencer.bin \
                $out/share/owo-bg22-sequencer/
              runHook postInstall
            '';
          };

          # `nix flake check` runs the portable core's tests on the host.
          checks.owo-core = pkgs.rustPlatform.buildRustPackage {
            pname = "owo-core";
            version = "0.1.0";
            src = ./crates/owo-core;
            cargoLock.lockFile = ./crates/owo-core/Cargo.lock;
            doCheck = true;
          };
          # The whole workspace (supervisor and the per-part crates).
          checks.owo-stim = pkgs.rustPlatform.buildRustPackage {
            pname = "owo-stim";
            version = "0.1.0";
            src = ./modules/owo-stim/rust;
            cargoLock.lockFile = ./modules/owo-stim/rust/Cargo.lock;
            doCheck = true;
          };
        };
    };
}
