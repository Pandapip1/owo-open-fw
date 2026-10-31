cmake_minimum_required(VERSION 3.20.0)
# A connectable GATT peripheral on our open link layer: one primary service
# with a readable and writable characteristic, for host GATT clients (bleak,
# bluetoothctl) driving it over the simulated RF medium.  UNVALIDATED ON
# HARDWARE.  Build from the repository root:
#   nix develop . -c cmake -G Ninja -S tests/gatt-peripheral-port -B build/gatt-peripheral-port-qemu \
#     -DBOARD=bg22_ek4108a -DZEPHYR_BASE="$PWD/vendor/zephyr" \
#     -DZephyr_DIR="$PWD/vendor/zephyr/share/zephyr-package/cmake" \
#     -DZEPHYR_MODULES="$PWD/vendor/cmsis_6;$PWD/vendor/hal_silabs;$PWD/vendor/mbedtls;$PWD/vendor/tf-psa-crypto"
#   nix develop . -c cmake --build build/gatt-peripheral-port-qemu
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../modules/efr32-radio)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(efr32_gatt_peripheral_port)
target_sources(app PRIVATE src/main.c)
