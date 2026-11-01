# AES on the BG22's RADIOAES accelerator, and its PSA Crypto transparent driver.
# SPDX-License-Identifier: Apache-2.0
#
# TF-PSA-Crypto calls accelerator drivers from its dispatch layer (psa_crypto_driver_wrappers.h),
# and Zephyr builds the one named by CONFIG_TF_PSA_CRYPTO_DISPATCH_DIR. As with the LLL sources
# (../CMakeLists.txt), no TF-PSA-Crypto code is copied into this repository: the dispatch is
# derived at configure time from vendor/tf-psa-crypto's own, with five exact insertions that call
# this driver before the software implementation. A changed upstream fails the configure step.
if(CONFIG_EFR32_RADIOAES)
  zephyr_library_named(efr32_radioaes)
  zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../include)
  zephyr_library_sources(radioaes.c)

  if(CONFIG_EFR32_RADIOAES_PSA)
    zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/include)
    zephyr_library_sources(radioaes_psa.c)
    zephyr_library_link_libraries_ifdef(CONFIG_MBEDTLS_BUILTIN mbedtls_iface)

    set(_up ${CONFIG_TF_PSA_CRYPTO_MODULE_DIR}/dispatch)
    set(_dd ${CMAKE_BINARY_DIR}/efr32_psa_dispatch)
    get_filename_component(_have "${CONFIG_TF_PSA_CRYPTO_DISPATCH_DIR}" ABSOLUTE)
    if(NOT _have STREQUAL _dd)
      message(FATAL_ERROR "efr32_radioaes: TF_PSA_CRYPTO_DISPATCH_DIR is ${_have}, expected the "
                          "derived ${_dd} (Kconfig default in modules/efr32-radio/Kconfig)")
    endif()

    foreach(f psa_crypto_driver_wrappers_no_static.c psa_crypto_driver_wrappers_no_static.h
              include/psa/crypto_driver_contexts_primitives.h
              include/psa/crypto_driver_contexts_composites.h)
      configure_file(${_up}/${f} ${_dd}/${f} COPYONLY)
    endforeach()

    file(READ ${_up}/psa_crypto_driver_wrappers.h _w)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_up}/psa_crypto_driver_wrappers.h)
    function(efr32_radioaes_dispatch_insert anchor block why)
      string(FIND "${_w}" "${anchor}" _first)
      string(FIND "${_w}" "${anchor}" _last REVERSE)
      if(_first EQUAL -1 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "efr32_radioaes: dispatch anchor for ${why} not found exactly once; "
                            "TF-PSA-Crypto's dispatch changed?")
      endif()
      string(REPLACE "${anchor}" "${block}${anchor}" _w "${_w}")
      set(_w "${_w}" PARENT_SCOPE)
    endfunction()

    efr32_radioaes_dispatch_insert("/* END-driver headers */"
      "/* efr32-radio: RADIOAES transparent driver (modules/efr32-radio/crypto) */\n#if defined(PSA_CRYPTO_DRIVER_EFR32_RADIOAES)\n#ifndef PSA_CRYPTO_DRIVER_PRESENT\n#define PSA_CRYPTO_DRIVER_PRESENT\n#endif\n#ifndef PSA_CRYPTO_ACCELERATOR_DRIVER_PRESENT\n#define PSA_CRYPTO_ACCELERATOR_DRIVER_PRESENT\n#endif\n#include \"efr32_radioaes_psa.h\"\n#endif /* PSA_CRYPTO_DRIVER_EFR32_RADIOAES */\n\n"
      "driver header")

    # Each call returns unless the driver declines (PSA_ERROR_NOT_SUPPORTED), the fallback
    # convention of the dispatch's own test driver.
    set(_try "#if defined(PSA_CRYPTO_DRIVER_EFR32_RADIOAES)\n            status = efr32_radioaes_@CALL@;\n            if( status != PSA_ERROR_NOT_SUPPORTED )\n                return( status );\n#endif /* PSA_CRYPTO_DRIVER_EFR32_RADIOAES */\n")

    string(REPLACE "@CALL@" "cipher_encrypt( attributes, key_buffer, key_buffer_size, alg, iv, iv_length, input, input_length, output, output_size, output_length )" _b "${_try}")
    efr32_radioaes_dispatch_insert("#if defined(MBEDTLS_PSA_BUILTIN_CIPHER)\n            return( mbedtls_psa_cipher_encrypt(" "${_b}" "cipher_encrypt")
    string(REPLACE "@CALL@" "cipher_decrypt( attributes, key_buffer, key_buffer_size, alg, input, input_length, output, output_size, output_length )" _b "${_try}")
    efr32_radioaes_dispatch_insert("#if defined(MBEDTLS_PSA_BUILTIN_CIPHER)\n            return( mbedtls_psa_cipher_decrypt(" "${_b}" "cipher_decrypt")
    string(REPLACE "@CALL@" "mac_compute( attributes, key_buffer, key_buffer_size, alg, input, input_length, mac, mac_size, mac_length )" _b "${_try}")
    efr32_radioaes_dispatch_insert("#if defined(MBEDTLS_PSA_BUILTIN_MAC)\n            /* Fell through, meaning no accelerator supports this operation */\n            status = mbedtls_psa_mac_compute(" "${_b}" "mac_compute")
    string(REPLACE "@CALL@" "aead_encrypt( attributes, key_buffer, key_buffer_size, alg, nonce, nonce_length, additional_data, additional_data_length, plaintext, plaintext_length, ciphertext, ciphertext_size, ciphertext_length )" _b "${_try}")
    efr32_radioaes_dispatch_insert("#if defined(MBEDTLS_PSA_BUILTIN_AEAD)\n            return( mbedtls_psa_aead_encrypt(" "${_b}" "aead_encrypt")
    string(REPLACE "@CALL@" "aead_decrypt( attributes, key_buffer, key_buffer_size, alg, nonce, nonce_length, additional_data, additional_data_length, ciphertext, ciphertext_length, plaintext, plaintext_size, plaintext_length )" _b "${_try}")
    efr32_radioaes_dispatch_insert("#if defined(MBEDTLS_PSA_BUILTIN_AEAD)\n            return( mbedtls_psa_aead_decrypt(" "${_b}" "aead_decrypt")

    file(WRITE ${_dd}/psa_crypto_driver_wrappers.h.tmp "${_w}")
    configure_file(${_dd}/psa_crypto_driver_wrappers.h.tmp ${_dd}/psa_crypto_driver_wrappers.h COPYONLY)
  endif()
endif()
