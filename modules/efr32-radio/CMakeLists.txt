# Out-of-tree lower link layer ("vendor") for Zephyr's split BLE controller on the EFR32BG22.
# UNVALIDATED ON HARDWARE; the radio hooks are stubs (docs/radio-tables.md).
# SPDX-License-Identifier: Apache-2.0
#
# Zephyr only knows the Nordic and OpenISA lower link layers (subsys/bluetooth/controller/
# CMakeLists.txt, BT_LLL_VENDOR_*). The OpenISA one is a complete, software-TIFS, legacy-feature LLL
# with all its hardware access behind hal/*; this module reuses its LLL sources unchanged (derived at
# configure time into the build tree, with only the "hal/RV32M1/" include prefix rewritten, so no
# Zephyr code is copied into this repository) and supplies its own hal/ implementation.
if(CONFIG_BT_LLL_VENDOR_EFR32)
  set(OI ${ZEPHYR_BASE}/subsys/bluetooth/controller/ll_sw/openisa)
  set(GEN ${CMAKE_CURRENT_BINARY_DIR}/derived)

  function(efr32_radio_derive src dst)
    file(READ ${src} _c)
    string(REPLACE "hal/RV32M1/" "hal/efr32/" _c "${_c}")
    get_filename_component(_d ${dst} DIRECTORY)
    file(MAKE_DIRECTORY ${_d})
    file(WRITE ${dst} "${_c}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${src})
  endfunction()

  # LLL sources and headers (all but lll_vendor.h, which is ours: event timing constants).
  file(GLOB _lll ${OI}/lll/*.c ${OI}/lll/*.h)
  foreach(f ${_lll})
    get_filename_component(n ${f} NAME)
    if(NOT n STREQUAL "lll_vendor.h")
      efr32_radio_derive(${f} ${GEN}/lll/${n})
    endif()
  endforeach()
  # Small source patches applied to the derived copies; each must match exactly once or configure fails
  # (they document where Zephyr's OpenISA LLL no longer builds outside its own SoC).
  function(efr32_radio_patch file old new why)
    file(READ ${GEN}/${file} _c)
    string(FIND "${_c}" "${old}" _pos)
    if(_pos EQUAL -1)
      message(FATAL_ERROR "efr32_radio: ${file}: patch target not found (${why}); Zephyr changed?")
    endif()
    string(REPLACE "${old}" "${new}" _c "${_c}")
    file(WRITE ${GEN}/${file} "${_c}")
  endfunction()

  efr32_radio_patch(lll/lll_conn.c "\tp->resv = 0U;\n" "\t/* p->resv does not exist in current pdu.h */\n"
            "struct pdu_data has no resv member")

  # OpenISA's lll_adv.c declares its advertising-PDU pool, stale-PDU FIFO,
  # and semaphore, but its init_reset() is an empty stub.  Consequently the
  # first lll_adv_data_init() sees an empty free list; ull_adv.c does not
  # propagate that allocation failure before init_set() dereferences pdu[0].
  # The current Nordic LLL initializes these same three objects in its
  # init_reset().  Keep this small, exact patch beside the other upstream
  # compatibility patches rather than carrying a fork of the LLL source.
  efr32_radio_patch(lll/lll_adv.c "static int init_reset(void)\n{\n\treturn 0;\n}"
            "static int init_reset(void)\n{\n\tmem_init(mem_pdu.pool, PDU_MEM_SIZE,\n\t\t (sizeof(mem_pdu.pool) / PDU_MEM_SIZE), &mem_pdu.free);\n\tMFIFO_INIT(pdu_free);\n\tk_sem_init(&sem_pdu_free, 0, PDU_MEM_FIFO_COUNT);\n\n\treturn 0;\n}"
            "OpenISA advertising PDU pool is declared but never initialized")

  # The one RV32M1-specific spot in the LLL: the tick-counter ISR reads the LPTMR1 compare flag.
  # Replaced by a flag set from the counter alarm callback (hal/cntr.c).
  file(READ ${GEN}/lll/lll.c _c)
  string(REPLACE "\tif (LPTMR1->CSR & LPTMR_CSR_TCF(1)) {\n\t\tLPTMR1->CSR |= LPTMR_CSR_TCF(1);\n"
                 "\tif (cntr_cmp_take()) {\n" _n "${_c}")
  if(_n STREQUAL _c)
    message(FATAL_ERROR "efr32_radio: Zephyr's openisa lll.c changed (LPTMR1 block not found)")
  endif()
  string(REPLACE "#include \"hal/swi.h\""
                 "#include \"hal/swi.h\"\n#include \"hal/efr32/cntr_efr32.h\"\n#include \"hal/efr32/ll_threads.h\""
                 _n "${_n}")
  # The worker and LLL contexts run in threads (hal/ll_threads.c). Their software interrupts are
  # connected to doorbells that only make the thread runnable; the bodies are bound to the threads.
  set(_irq "\tIRQ_CONNECT(LL_RTC0_IRQn, CONFIG_BT_CTLR_ULL_HIGH_PRIO,\n\t\t    rtc0_rv32m1_isr, NULL, 0);\n\tIRQ_CONNECT(HAL_SWI_RADIO_IRQ, CONFIG_BT_CTLR_LLL_PRIO,\n\t\t    swi_lll_rv32m1_isr, NULL, 0);\n")
  string(FIND "${_n}" "${_irq}" _pos)
  if(_pos EQUAL -1)
    message(FATAL_ERROR "efr32_radio: Zephyr's openisa lll.c changed (SWI IRQ_CONNECT block not found)")
  endif()
  string(REPLACE "${_irq}"
                 "\tefr32_ll_threads_bind(rtc0_rv32m1_isr, swi_lll_rv32m1_isr);\n\tIRQ_CONNECT(LL_RTC0_IRQn, CONFIG_BT_CTLR_ULL_HIGH_PRIO,\n\t\t    efr32_ll_worker_doorbell_isr, NULL, 0);\n\tIRQ_CONNECT(HAL_SWI_RADIO_IRQ, CONFIG_BT_CTLR_LLL_PRIO,\n\t\t    efr32_ll_lll_doorbell_isr, NULL, 0);\n"
                 _n "${_n}")
  file(WRITE ${GEN}/lll/lll.c "${_n}")
  efr32_radio_derive(${CMAKE_CURRENT_LIST_DIR}/lll/lll_vendor.h ${GEN}/lll/lll_vendor.h)

  # hal/*_vendor_hal.h, the headers that are generic, the HCI vendor header and ticker glue.
  foreach(n cntr cpu debug radio swi ticker)
    efr32_radio_derive(${OI}/hal/${n}_vendor_hal.h ${GEN}/hal/${n}_vendor_hal.h)
  endforeach()
  foreach(n cntr.h cpu.h debug.h ticker.h radio/radio.h)
    efr32_radio_derive(${OI}/hal/RV32M1/${n} ${GEN}/hal/efr32/${n})
  endforeach()
  efr32_radio_derive(${OI}/hci/hci_vendor.h ${GEN}/hci_vendor.h)
  efr32_radio_derive(${OI}/hal/RV32M1/ticker.c ${GEN}/hal/efr32/ticker.c)

  zephyr_library_named(efr32_radio)
  zephyr_library_link_libraries(subsys__bluetooth)
  # The controller library (ull.c, hci.c, ...) finds hal/*.h, lll/*_vendor.h and hci_vendor.h here.
  zephyr_include_directories(
    ${GEN}
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${ZEPHYR_BASE}/subsys/bluetooth/controller
    ${ZEPHYR_BASE}/subsys/bluetooth/controller/ll_sw
    ${ZEPHYR_BASE}/subsys/bluetooth/controller/include
  )

  zephyr_library_sources(${GEN}/lll/lll.c)
  zephyr_library_sources_ifdef(CONFIG_BT_BROADCASTER ${GEN}/lll/lll_adv.c)
  zephyr_library_sources_ifdef(CONFIG_BT_OBSERVER ${GEN}/lll/lll_scan.c)
  if(CONFIG_BT_CONN)
    zephyr_library_sources(${GEN}/lll/lll_clock.c ${GEN}/lll/lll_conn.c)
    zephyr_library_sources_ifdef(CONFIG_BT_PERIPHERAL ${GEN}/lll/lll_peripheral.c)
    zephyr_library_sources_ifdef(CONFIG_BT_CENTRAL ${GEN}/lll/lll_central.c)
  endif()
  zephyr_library_sources_ifdef(CONFIG_BT_CTLR_PROFILE_ISR ${GEN}/lll/lll_prof.c)
  zephyr_library_sources_ifdef(CONFIG_BT_CTLR_DTM ${GEN}/lll/lll_test.c)

  zephyr_library_sources(
    hal/ll_threads.c
    ${GEN}/hal/efr32/ticker.c
    hal/cntr.c
    hal/ecb.c
    hal/radio.c
    hal/timer_state.c
  )
endif()
