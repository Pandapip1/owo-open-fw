{
  description = "OWO suit open firmware (proof of concept): Rust on Zephyr, Silicon Labs Series 2";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
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
          rustToolchain = pkgs.rust-bin.stable.latest.default.override {
            targets = [
              "thumbv8m.main-none-eabihf"
              "thumbv8m.main-none-eabi"
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
            ]
          );
        in
        {
          _module.args.pkgs = import inputs.nixpkgs {
            inherit system;
            overlays = [ inputs.rust-overlay.overlays.default ];
          };

          devShells.default = pkgs.mkShell {
            packages = [
              rustToolchain
              python
              armGcc
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

            env = {
              # Zephyr: use a plain GNU Arm Embedded toolchain instead of the Zephyr SDK.
              ZEPHYR_TOOLCHAIN_VARIANT = "gnuarmemb";
              GNUARMEMB_TOOLCHAIN_PATH = "${armGcc}";

              # zephyr-sys and this crate's build.rs run bindgen, which needs libclang.
              LIBCLANG_PATH = "${pkgs.llvmPackages.libclang.lib}/lib";

              # Workaround, verified only with Ubuntu's gcc-arm-none-eabi 13.2.1 (not with
              # this shell's 13.3 toolchain until the build below was run): the Zephyr build passes the GCC include directory to
              # bindgen's clang with -I, so clang parses GCC's <arm_acle.h> and rejects its
              # coprocessor intrinsics. Skipping that header leaves __rbit/__clz/__isb
              # undeclared in clang's view; they are only used inside inline functions that
              # bindgen does not bind.
              # Remove this if your toolchain does not hit the error.
              BINDGEN_EXTRA_CLANG_ARGS = "-D_GCC_ARM_ACLE_H -Wno-error=implicit-function-declaration";
            };

            shellHook = ''
              # West wants the Zephyr tree; `west init -l . && west update` puts it next to this repo.
              echo "OWO open firmware dev shell"
              echo "  first time:  west init -l . && west update && west blobs fetch hal_silabs"
              echo "  build:       west build -b bg22_ek4108a ."
              echo "  QEMU:        mkdir -p build/qemu && (cd build/qemu && ../../vendor/qemu/configure --target-list=arm-softmmu --disable-docs --disable-capstone --enable-slirp)"
              echo "               ninja -C build/qemu qemu-system-arm"
              echo "  host tests:  cargo test --manifest-path crates/owo-core/Cargo.toml"
              echo "               cargo test --manifest-path modules/owo-stim/rust/Cargo.toml"
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
