# The product is installed through the unit's own Gecko Bootloader (Bluetooth OTA, docs/ota.md),
# not by flashing. The runner is declared so a debugger can attach; the device is the module's
# die as DEVINFO.PART reports it.
board_runner_args(jlink "--device=EFR32BG22C222F352GM40")
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
