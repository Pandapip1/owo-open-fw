# SPDX-License-Identifier: Apache-2.0
# cmake -DIN=<image.bin> -DOUT=<image.c> -P embed.cmake: the M0+ image as const data
# (kern0_image, kern0_image_size) for main.c.
file(READ ${IN} hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE ${OUT}
  "/* Generated from kern0.c by embed.cmake; do not edit. */\n"
  "#include <stddef.h>\n#include <stdint.h>\n\n"
  "const uint8_t kern0_image[] __attribute__((aligned(4))) = {${bytes}};\n"
  "const size_t kern0_image_size = sizeof(kern0_image);\n")
