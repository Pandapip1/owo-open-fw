# SPDX-License-Identifier: Apache-2.0
# cmake -DIN=<tramp.bin> -DOUT=<tramp-image.c> -P embed.cmake: the M0+ entry as const data
# (tramp_image, tramp_image_size) for main.c.
file(READ ${IN} hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE ${OUT}
  "/* Generated from tramp.c by embed.cmake; do not edit. */\n"
  "#include <stddef.h>\n#include <stdint.h>\n\n"
  "const uint8_t tramp_image[] __attribute__((aligned(4))) = {${bytes}};\n"
  "const size_t tramp_image_size = sizeof(tramp_image);\n")
