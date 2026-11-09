# Out-of-tree lower link layer ("vendor") for Zephyr's split BLE controller on the EFR32BG22.
# Runs on silicon: advertising, scanning and encrypted connections on a BG22 kit
# (docs/radio-tables.md §7.84, docs/hardware-bringup.md).
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

  set(EFR32_RADIO_PROPRIETARY_SEQ_IMAGE "" CACHE FILEPATH
      "Diagnostic-only stage-2.5 radio-coprocessor image; never use for stage 3")

  # Build the private radio Cortex-M0+ firmware from reviewed source and embed
  # that exact binary in the M33 application.  SEQRAM is retention RAM, so a
  # separately flashed or previously booted image is never a valid dependency.
  set(SEQ_SRC ${CMAKE_CURRENT_LIST_DIR}/sequencer/radio_seq.S)
  set(SEQ_LD ${CMAKE_CURRENT_LIST_DIR}/sequencer/link.ld)
  set(SEQ_ELF ${GEN}/sequencer/radio-sequencer.elf)
  set(SEQ_BIN ${GEN}/sequencer/radio-sequencer.bin)
  set(SEQ_C ${GEN}/sequencer/radio-sequencer-image.c)
  if(EFR32_RADIO_PROPRIETARY_SEQ_IMAGE)
    if(NOT IS_ABSOLUTE "${EFR32_RADIO_PROPRIETARY_SEQ_IMAGE}" OR
       NOT EXISTS "${EFR32_RADIO_PROPRIETARY_SEQ_IMAGE}")
      message(FATAL_ERROR
        "EFR32_RADIO_PROPRIETARY_SEQ_IMAGE must name an existing absolute path")
    endif()
    message(WARNING
      "Stage 2.5 diagnostic build: embedding proprietary radio-coprocessor image")
    add_custom_command(
      OUTPUT ${SEQ_C}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}/sequencer
      COMMAND ${PYTHON_EXECUTABLE}
              ${CMAKE_CURRENT_LIST_DIR}/sequencer/embed_binary.py
              ${EFR32_RADIO_PROPRIETARY_SEQ_IMAGE} ${SEQ_C} proprietary
      DEPENDS ${EFR32_RADIO_PROPRIETARY_SEQ_IMAGE}
              ${CMAKE_CURRENT_LIST_DIR}/sequencer/embed_binary.py
      VERBATIM
      COMMENT "Embedding diagnostic proprietary EFR32 radio-coprocessor image"
    )
  else()
    set(SEQ_DEFS "")
    if(CONFIG_EFR32_SEQ_WAKE_PROBE)
      list(APPEND SEQ_DEFS -DEFR32_SEQ_WAKE_PROBE)
    endif()
    add_custom_command(
      OUTPUT ${SEQ_C}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}/sequencer
      COMMAND ${CMAKE_C_COMPILER} -mcpu=cortex-m0plus -mthumb -nostdlib ${SEQ_DEFS}
              -I${CMAKE_CURRENT_LIST_DIR}/include
              -Wl,--build-id=none -Wl,-T,${SEQ_LD}
              ${SEQ_SRC} -o ${SEQ_ELF}
      COMMAND ${CMAKE_OBJCOPY} -O binary ${SEQ_ELF} ${SEQ_BIN}
      COMMAND ${PYTHON_EXECUTABLE}
              ${CMAKE_CURRENT_LIST_DIR}/sequencer/embed_binary.py
              ${SEQ_BIN} ${SEQ_C} open
      DEPENDS ${SEQ_SRC} ${SEQ_LD}
              ${CMAKE_CURRENT_LIST_DIR}/include/hal/efr32/seq_abi.h
              ${CMAKE_CURRENT_LIST_DIR}/sequencer/embed_binary.py
      VERBATIM
      COMMENT "Building and embedding open EFR32 radio-sequencer firmware"
    )
  endif()

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

  # Connection replies staged before the frame ends (docs/radio-tables.md 7.82): the sequencer
  # answers a received frame at T_IFS by lll_conn.c's own rules, and isr_rx() commits the same
  # outcome afterwards. lll/lll_conn_stage.c is built into lll_conn.c (it reads its state) and
  # these hooks call it; with EFR32_RADIO_CONN_STAGED off they reduce to the original code.
  efr32_radio_patch(lll/lll_conn.c "static struct pdu_data *empty_tx_enqueue(struct lll_conn *lll);\n"
            "static struct pdu_data *empty_tx_enqueue(struct lll_conn *lll);\nstatic int efr32_conn_commit_begin(struct lll_conn *lll);\nstatic bool efr32_conn_rx_free(void);\nstatic void efr32_conn_tx_prep(struct lll_conn *lll, struct pdu_data **pdu_data_tx);\nstatic void efr32_conn_reply_abort(void *param);\nstatic void efr32_conn_stage_next(struct lll_conn *lll);\n"
            "staged replies: declarations")
  efr32_radio_patch(lll/lll_conn.c "\t\treturn;\n\t}\n\n\ttrx_cnt++;\n"
            "\t\treturn;\n\t}\n\n\ttrx_cnt++;\n\n\t/* Staged reply the sequencer took no decision on: nothing was sent, the event ends. */\n\tif (efr32_conn_commit_begin(lll) != 0) {\n\t\tradio_isr_set(isr_done, param);\n\t\tradio_disable();\n\n\t\treturn;\n\t}\n"
            "staged replies: commit the sequencer's outcome")
  efr32_radio_patch(lll/lll_conn.c "\t\tif (err) {\n\t\t\tgoto lll_conn_isr_rx_exit;\n\t\t}\n"
            "\t\tif (err) {\n\t\t\t/* MIC failure: a staged reply already went out; end the event now. */\n\t\t\tefr32_conn_reply_abort(param);\n\t\t\tgoto lll_conn_isr_rx_exit;\n\t\t}\n"
            "staged replies: MIC failure")
  efr32_radio_patch(lll/lll_conn.c "\t    (ull_pdu_rx_alloc_peek(3) != 0)) {\n"
            "\t    efr32_conn_rx_free()) {\n"
            "staged replies: the RX buffer state the reply was staged with")
  efr32_radio_patch(lll/lll_conn.c "\tis_empty_pdu_tx_retry = lll->empty;\n\tlll_conn_pdu_tx_prep(lll, &pdu_data_tx);\n"
            "\tis_empty_pdu_tx_retry = lll->empty;\n\tefr32_conn_tx_prep(lll, &pdu_data_tx);\n"
            "staged replies: the reply the sequencer sent")
  efr32_radio_patch(lll/lll_conn.c "\tif (is_ull_rx) {\n\t\tull_rx_sched();\n\t}\n"
            "\tefr32_conn_stage_next(lll);\n\n\tif (is_ull_rx) {\n\t\tull_rx_sched();\n\t}\n"
            "staged replies: stage the next receive's")
  file(APPEND ${GEN}/lll/lll_conn.c "\n#include \"${CMAKE_CURRENT_LIST_DIR}/lll/lll_conn_stage.c\"\n")
  # The advertiser's SCAN_RSP, staged the same way (lll/lll_adv_stage.c).
  efr32_radio_patch(lll/lll_adv.c "static void isr_tx(void *param);\n"
            "static void isr_tx(void *param);\nstatic void efr32_adv_stage(struct lll_adv *lll);\n"
            "staged scan response: declaration")
  efr32_radio_patch(lll/lll_adv.c "\tradio_pkt_rx_set(radio_pkt_scratch_get());\n"
            "\tradio_pkt_rx_set(radio_pkt_scratch_get());\n\tefr32_adv_stage(param);\n"
            "staged scan response: stage it with the receive")
  file(APPEND ${GEN}/lll/lll_adv.c "\n#include \"${CMAKE_CURRENT_LIST_DIR}/lll/lll_adv_stage.c\"\n")

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

  # OpenISA's preempt timeout assumes every LLL context shares one IRQ priority, so the ticker
  # callback that enqueues preempt() runs it inline. Here the LLL runs above the ULL worker
  # (hal/ll_threads.c), so preempt() runs later in the LLL context, and an LLL-priority radio job
  # due earlier can finish an event first: the pipeline slot the ticker named may then be gone or
  # reused, and a preempt timeout may already be scheduled for the next prepare. Name the
  # preemptor by its LLL context, as the Nordic LLL does, and accept an already scheduled timeout
  # when restarting it for the next prepare.
  efr32_radio_patch(lll/lll.c "\t\t\t   preempt_ticker_cb, evt,\n"
            "\t\t\t   preempt_ticker_cb, p->param,\n"
            "preempt timeout named by pipeline slot")
  efr32_radio_patch(lll/lll.c "\tif (next != param) {\n"
            "\tif (next->prepare_param.param != param) {\n"
            "preemptor compared by pipeline slot")
  efr32_radio_patch(lll/lll.c "\t\tret = preempt_ticker_start(next, ticker_start_next_op_cb);\n"
            "\t\tret = preempt_ticker_start(next, ticker_start_op_cb);\n"
            "preempt timeout restart asserts it was not already scheduled")

  # OpenISA's scanner arms an RX->TX turnaround for every receive window, a passive scanner's
  # included; efr32_scan_switch() (lll/lll_scan_stage.c) arms it only where a response can follow,
  # an active scanner's SCAN_REQ or an initiator's CONNECT_IND, and stages that response: the
  # sequencer answers at T_IFS by isr_rx_pdu()'s rules and isr_rx_pdu() commits the same answer
  # (docs/radio-tables.md 7.87). With EFR32_RADIO_CONN_STAGED off only the turnaround is armed.
  efr32_radio_patch(lll/lll_scan.c "static void isr_race(void *param);\n"
            "static void isr_race(void *param);\n#include \"hal/efr32/radio_conn.h\"\n#include \"hal/efr32/ll_deadlines.h\"\nstatic void efr32_scan_switch(struct lll_scan *lll);\nstatic bool efr32_scan_declined(bool declines);\nstatic void efr32_scan_stage_next(struct lll_scan *lll);\n"
            "staged scanner replies: declarations")
  efr32_radio_patch(lll/lll_scan.c "\tradio_isr_set(isr_rx, lll);\n\n\t/* setup tIFS switching */\n\tradio_tmr_tifs_set(EVENT_IFS_US);\n\tradio_switch_complete_and_tx(PHY_LEGACY, PHY_FLAGS_UNUSED, PHY_LEGACY, PHY_FLAGS_UNUSED);\n"
            "\tradio_isr_set(isr_rx, lll);\n\n\t/* setup tIFS switching */\n\tradio_tmr_tifs_set(EVENT_IFS_US);\n\tefr32_scan_switch(lll);\n"
            "passive scanner: no RX->TX turnaround at prepare")
  efr32_radio_patch(lll/lll_scan.c "\tradio_switch_complete_and_tx(PHY_LEGACY, PHY_FLAGS_UNUSED, PHY_LEGACY, PHY_FLAGS_UNUSED);\n\n\tnode_rx = ull_pdu_rx_alloc_peek(1);\n\tLL_ASSERT(node_rx);\n\tradio_pkt_rx_set(node_rx->pdu);\n\n#if defined(CONFIG_BT_CTLR_PRIVACY)"
            "\tefr32_scan_switch(param);\n\n\tnode_rx = ull_pdu_rx_alloc_peek(1);\n\tLL_ASSERT(node_rx);\n\tradio_pkt_rx_set(node_rx->pdu);\n\n#if defined(CONFIG_BT_CTLR_PRIVACY)"
            "passive scanner: no RX->TX turnaround after a closed exchange")
  efr32_radio_patch(lll/lll_scan.c "\t\tuint32_t err;\n\n\t\t/* setup tIFS switching */\n\t\tradio_tmr_tifs_set(EVENT_IFS_US);\n\t\tradio_switch_complete_and_rx(0);\n"
            "\t\tuint32_t err;\n\n\t\t/* A staged SCAN_REQ: what the sequencer sent, or nothing. */\n\t\tif (efr32_scan_declined(false)) {\n\t\t\treturn 1;\n\t\t}\n\n\t\t/* setup tIFS switching */\n\t\tradio_tmr_tifs_set(EVENT_IFS_US);\n\t\tradio_switch_complete_and_rx(0);\n"
            "staged scanner replies: the sequencer's SCAN_REQ decision")
  efr32_radio_patch(lll/lll_scan.c "\t\tlll->state = 1U;\n\t\tradio_isr_set(isr_tx, lll);\n"
            "\t\tlll->state = 1U;\n\t\tradio_isr_set(isr_tx, lll);\n\t\tefr32_scan_stage_next(lll);\n"
            "staged scanner replies: stage the SCAN_RSP receive")
  if(CONFIG_BT_CENTRAL)
    efr32_radio_patch(lll/lll_scan.c "\t\tif (pdu_end_us > (HAL_TICKER_TICKS_TO_US(ull->ticks_slot) -\n\t\t\t\t  EVENT_IFS_US - 352 - EVENT_OVERHEAD_START_US -\n\t\t\t\t  EVENT_TICKER_RES_MARGIN_US)) {\n"
              "\t\t/* A staged CONNECT_IND: what the sequencer sent, or nothing. */\n\t\tif (efr32_scan_declined(pdu_end_us > (HAL_TICKER_TICKS_TO_US(ull->ticks_slot) -\n\t\t\t\t  EVENT_IFS_US - 352 - EVENT_OVERHEAD_START_US -\n\t\t\t\t  EVENT_TICKER_RES_MARGIN_US))) {\n"
              "staged initiator replies: the sequencer's CONNECT_IND decision")
    # WinOffset: hal/efr32/ll_deadlines.h EFR32_DL_CONNECT_IND_WIN_OFFSET.
    efr32_radio_patch(lll/lll_scan.c "\t\t\tconn_space_us = conn_offset_us;\n\t\t\tpdu_tx->connect_ind.win_offset = sys_cpu_to_le16(0);\n"
              "\t\t\tconn_space_us = conn_offset_us + EFR32_DL_CONNECT_IND_WIN_OFFSET *\n\t\t\t\t\t\t\t BT_TRANSMIT_WINDOW_UNIT_US;\n\t\t\tpdu_tx->connect_ind.win_offset =\n\t\t\t\tsys_cpu_to_le16(EFR32_DL_CONNECT_IND_WIN_OFFSET);\n"
              "initiator: the WinOffset the connection setup needs")
  endif()
  file(APPEND ${GEN}/lll/lll_scan.c "\n#include \"${CMAKE_CURRENT_LIST_DIR}/lll/lll_scan_stage.c\"\n")

  # Connection events tell the radio HAL what their jobs serve (hal/efr32/radio_conn.h): the
  # connection interval, whose next event the event's closing work serves, and, for a Peripheral,
  # how late its receive window may open by the specification (docs/radio-tables.md 7.79).
  if(CONFIG_BT_PERIPHERAL)
    efr32_radio_patch(lll/lll_peripheral.c "#include \"hal/radio.h\"\n"
              "#include \"hal/radio.h\"\n#include \"hal/efr32/radio_conn.h\"\n"
              "connection timing hooks")
    efr32_radio_patch(lll/lll_peripheral.c
              "\tremainder = p->remainder;\n\tremainder_us = radio_tmr_start(0, ticks_at_start, remainder);\n"
              "\tremainder = p->remainder;\n\tefr32_radio_conn_event_set(lll->interval * CONN_INT_UNIT_US);\n\tefr32_radio_rx_window_set(EVENT_TICKER_RES_MARGIN_US);\n\tefr32_conn_stage_prepare(lll);\n\tremainder_us = radio_tmr_start(0, ticks_at_start, remainder);\n"
              "Peripheral receive window and connection interval")
  endif()
  if(CONFIG_BT_CENTRAL)
    efr32_radio_patch(lll/lll_central.c "#include \"hal/radio.h\"\n"
              "#include \"hal/radio.h\"\n#include \"hal/efr32/radio_conn.h\"\n"
              "connection timing hooks")
    efr32_radio_patch(lll/lll_central.c
              "\tremainder_us = radio_tmr_start(1, ticks_at_start, remainder);\n"
              "\tefr32_radio_conn_event_set(lll->interval * CONN_INT_UNIT_US);\n\tefr32_conn_stage_prepare(lll);\n\tremainder_us = radio_tmr_start(1, ticks_at_start, remainder);\n"
              "Central connection interval")
  endif()

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

  # Transmit power (hal/tx_power.c, docs/tx-power.md): OpenISA's fixed 0 dBm default becomes the
  # BT_CTLR_TX_PWR choice, and with dynamic control each role's own level.
  if(CONFIG_EFR32_RADIO_TX_POWER)
    efr32_radio_patch(hal/efr32/radio/radio.h "#define RADIO_TXP_DEFAULT 0\n"
              "#define RADIO_TXP_DEFAULT CONFIG_BT_CTLR_TX_PWR_DBM\n"
              "default transmit power from Kconfig")
  endif()
  if(CONFIG_BT_CTLR_TX_PWR_DYNAMIC_CONTROL)
    if(NOT CONFIG_EFR32_RADIO_TX_POWER)
      message(FATAL_ERROR "BT_CTLR_TX_PWR_DYNAMIC_CONTROL needs EFR32_RADIO_TX_POWER")
    endif()
    foreach(n lll_adv lll_scan lll_central lll_peripheral)
      efr32_radio_patch(lll/${n}.c "\tradio_tx_power_set(RADIO_TXP_DEFAULT);\n"
                "\tradio_tx_power_set(lll->tx_pwr_lvl);\n"
                "per-role transmit power")
    endforeach()
  endif()
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
    ${SEQ_C}
    hal/ll_threads.c
    ${GEN}/hal/efr32/ticker.c
    hal/cntr.c
    hal/ecb.c
    hal/radio.c
    hal/timer_state.c
  )
  zephyr_library_sources_ifdef(CONFIG_EFR32_RADIO_TX_POWER hal/tx_power.c)

  # Amateur operation (docs/amateur-radio.md): never in a build without the option.
  if(CONFIG_EFR32_RADIO_AMATEUR)
    # ull_vendor.h for the controller's user extensions (its ticker nodes only).
    zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/include/user_ext)
    zephyr_library_sources(hal/amateur.c hal/amateur_rules.c)
  endif()
endif()

add_subdirectory(crypto)
