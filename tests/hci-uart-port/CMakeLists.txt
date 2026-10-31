cmake_minimum_required(VERSION 3.20.0)
# Zephyr's H4 UART HCI controller (samples/bluetooth/hci_uart) on our open
# link layer: Zephyr's split controller over modules/efr32-radio.  USART1, the
# kit's VCOM, carries H4; under QEMU it is handed a host device such as
# /dev/vhci (sim/README.md, "Host devices").  UNVALIDATED ON HARDWARE.
# Build from the repository root:
#   nix develop . -c cmake -G Ninja -S tests/hci-uart-port -B build/hci-uart-port-qemu \
#     -DBOARD=bg22_ek4108a -DZEPHYR_BASE="$PWD/vendor/zephyr" \
#     -DZephyr_DIR="$PWD/vendor/zephyr/share/zephyr-package/cmake" \
#     -DZEPHYR_MODULES="$PWD/vendor/cmsis_6;$PWD/vendor/hal_silabs;$PWD/vendor/mbedtls;$PWD/vendor/tf-psa-crypto"
#   nix develop . -c cmake --build build/hci-uart-port-qemu
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../modules/efr32-radio)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(efr32_hci_uart_port)
target_sources(app PRIVATE ${ZEPHYR_BASE}/samples/bluetooth/hci_uart/src/main.c)
