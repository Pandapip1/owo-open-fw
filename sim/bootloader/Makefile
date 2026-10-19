ROOT:=$(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
GECKO_SDK?=$(ROOT)/../silabs-gecko-sdk
G=$(GECKO_SDK)/platform
B=$(G)/bootloader
CC=arm-none-eabi-gcc
DEV=EFR32BG22C224F512IM40
CFLAGS=-MMD -mcmse -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -Os -g -ffunction-sections -fdata-sections -std=gnu99 -Wall \
 -D$(DEV) -DBOOTLOADER_ENABLE=1 -DSL_RAMFUNC_DISABLE=1 -DBOOTLOADER_SECOND_STAGE=1 -DBOOTLOADER_SUPPORT_STORAGE=1 \
 -DBOOTLOADER_SUPPORT_INTERNAL_STORAGE=1 -DSL_APP_PROPERTIES -DHAL_CONFIG=0 -DSL_LEGACY_LINKER -D__START=main -D__STACK_SIZE=0x800 -D__HEAP_SIZE=0 \
 -Icfg -iquote cfg -I$(GECKO_SDK)/util/third_party/trusted-firmware-m/config -I$(G)/../../util/third_party/mbedtls/include -I$(GECKO_SDK)/util/third_party/mbedtls/include -I$(GECKO_SDK)/util/third_party/trusted-firmware-m/lib/fih/inc -I$(G)/security/sl_component/sl_cryptoacc_library/include -DMBEDTLS_CONFIG_FILE='"btl_mbedtls_config.h"' -I$(B) -I$(B)/config -I$(B)/config/s2 -I$(B)/config/s2/device_series_2 -I$(B)/api -I$(B)/core -I$(B)/core/flash \
 -I$(B)/driver -I$(B)/security -I$(B)/storage -I$(B)/parser/gbl -I$(B)/debug -I$(B)/app_properties -I$(B)/app_properties/config \
 -I$(G)/Device/SiliconLabs/EFR32BG22/Include -I$(G)/CMSIS/Core/Include -I$(G)/common/inc -I$(G)/emlib/inc -I$(G)/security/sl_component/sl_cryptoacc_library/include -I$(G)/service/device_init/inc
SRC=$(B)/driver/btl_driver_util.c $(B)/core/btl_bootload.c $(B)/core/btl_core.c $(B)/core/btl_main.c $(B)/core/flash/btl_internal_flash.c \
 $(B)/core/btl_parse.c $(B)/core/btl_reset.c $(B)/security/btl_crc16.c $(B)/security/btl_crc32.c $(B)/security/btl_security_tokens.c \
 $(B)/debug/btl_debug.c $(B)/storage/btl_storage.c $(B)/storage/btl_storage_library.c $(B)/storage/bootloadinfo/btl_storage_bootloadinfo_single.c \
 $(B)/storage/internal_flash/btl_storage_internal_flash.c $(B)/parser/gbl/btl_gbl_custom_tags.c $(B)/parser/gbl/btl_gbl_format.c \
 $(B)/parser/gbl/btl_gbl_parser.c $(B)/security/btl_security_aes.c $(B)/security/btl_security_sha256.c $(B)/security/btl_security_ecdsa.c \
 $(B)/security/sha/btl_sha256.c
SRC+= $(G)/Device/SiliconLabs/EFR32BG22/Source/startup_efr32bg22.c $(G)/Device/SiliconLabs/EFR32BG22/Source/system_efr32bg22.c $(G)/emlib/src/em_cmu.c $(G)/emlib/src/em_gpio.c $(G)/emlib/src/em_system.c $(G)/emlib/src/em_msc.c $(G)/emlib/src/em_emu.c $(G)/emlib/src/em_rmu.c
SRC+= $(PWD)/cfg/hw_shims.c $(GECKO_SDK)/util/third_party/mbedtls/library/aes.c $(GECKO_SDK)/util/third_party/mbedtls/library/platform_util.c
OBJS=$(patsubst %.c,obj2/%.o,$(notdir $(SRC)))
vpath %.c $(sort $(dir $(SRC)))
PWD:=$(shell pwd)
obj2/%.o: %.c Makefile
	$(CC) $(CFLAGS) -c $< -o $@
all: $(OBJS)

btl.elf: $(OBJS) btl.ld
	$(CC) $(CFLAGS) -nostartfiles -Wl,--gc-sections -Wl,-Map=btl.map -Tbtl.ld $(OBJS) -o $@ -lc -lnosys 2>&1 | head -50
-include $(OBJS:.o=.d)
