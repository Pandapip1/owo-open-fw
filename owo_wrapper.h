/* Header for this crate's own bindgen run: the parts of Zephyr's Bluetooth API that
 * zephyr-sys does not bind (it only includes <zephyr/bluetooth/bluetooth.h>).
 * The preamble is copied from zephyr-sys/wrapper.h (zephyr-lang-rust) so the same
 * headers parse the same way under clang. */
#undef KERNEL

#ifdef RUST_BINDGEN
/* errno is coming from somewhere in Zephyr's build.  Add the symbol when running bindgen so that it
 * is defined here.
 */
extern int errno;
#endif

/*
 * Pull in <stdint.h> before any Zephyr header. Zephyr's mm.h has file-scope
 * #if directives that reference CONFIG_SRAM_BASE_ADDRESS, which on some
 * boards expands via __UINT32_C(...). These underscored variants are normally
 * provided by the compiler as predefines so that Zephyr's minimal libc
 * <stdint.h> (which only redirects UINT32_C → __UINT32_C for GCC/clang) works.
 * Ubuntu's libclang-15 as used by bindgen does not surface those predefines,
 * so we define them ourselves with #ifndef guards in case a future toolchain
 * supplies them.
 */
#include <stdint.h>

#ifndef __INT8_C
#define __INT8_C(x)    x
#endif
#ifndef __INT16_C
#define __INT16_C(x)   x
#endif
#ifndef __INT32_C
#define __INT32_C(x)   x
#endif
#ifndef __INT64_C
#define __INT64_C(x)   x ## LL
#endif
#ifndef __INTMAX_C
#define __INTMAX_C(x)  x ## LL
#endif
#ifndef __UINT8_C
#define __UINT8_C(x)   x ## U
#endif
#ifndef __UINT16_C
#define __UINT16_C(x)  x ## U
#endif
#ifndef __UINT32_C
#define __UINT32_C(x)  x ## U
#endif
#ifndef __UINT64_C
#define __UINT64_C(x)  x ## ULL
#endif
#ifndef __UINTMAX_C
#define __UINTMAX_C(x) x ## ULL
#endif

/* First, make sure we have all of our config settings. */
#include <zephyr/autoconf.h>

/* Gcc defines __SOFT_FP__ when the target uses software floating point, and the CMSIS headers get
 * confused without this.
 */
#if defined(CONFIG_CPU_CORTEX_M)
#if !defined(CONFIG_FP_HARDABI) && !defined(FORCE_FP_HARDABI) && !defined(__SOFTFP__)
#define __SOFTFP__
#endif
#endif


#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
