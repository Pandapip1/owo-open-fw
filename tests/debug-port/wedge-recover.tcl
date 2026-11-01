# SPDX-License-Identifier: Apache-2.0
#
# The RAC clock-retiming debug-port wedge and its recovery (docs/radio-tables.md
# 7.80, docs/debug-port-model.md "Debug-port wedge"), for a mem_ap target on
# AP0 (tests/debug-port/ro-mem-ap.cfg) and an adapter with a reset line.
#
#   wedge_debug_port: enables XORETIMECTRL and DIGCLKRETIMECTRL ENRETIME the
#     way the wedging image's sequencer did (radio bus clocks first), then
#     shows the AHB-AP refusing a read.
#   recover_wedge HANDLER: scratch/silicon-rx/recover2.sh "fpb", the procedure
#     that recovered the EK: nRESET held during init (the DP does not
#     answer), released; back to back, attach and arm DHCSR halt plus FPB
#     comparator 0 on the image's reset handler HANDLER; poll S_HALT.

proc wedge_debug_port {} {
	# CMU RADIOCLKCTRL.EN and CLKEN1.RAC through the SET aliases (RM 8.5)
	set CMU_RADIOCLKCTRL_SET 0x40009280
	set CMU_CLKEN1_SET 0x40009068
	set CMU_CLKEN1_RAC 0x20
	# RAC retiming controls, ENRETIME is bit 0 of each (public RAC_S model);
	# the secure and the non-secure alias, whichever the image left enabled
	set retime {0xa8020174 0xa802016c 0xb8020174 0xb802016c}

	write_memory $CMU_RADIOCLKCTRL_SET 32 1
	write_memory $CMU_CLKEN1_SET 32 $CMU_CLKEN1_RAC
	foreach addr $retime {
		catch {write_memory $addr 32 1}
	}
	if {[catch {read_memory 0x0fe08004 32 1} e]} {
		echo "WEDGED: AHB-AP refuses ($e)"
	} else {
		echo "NOT WEDGED"
	}
}

proc recover_wedge {handler} {
	set DHCSR 0xe000edf0
	set FP_CTRL 0xe0002000
	set FP_COMP0 0xe0002008
	set DBGKEY_C_HALT_C_DEBUGEN 0xa05f0003
	set FP_CTRL_KEY_ENABLE 3
	set FP_COMP_BE 1
	set S_HALT 0x20000

	reset_config srst_only srst_nogate connect_assert_srst
	if {[catch {init} e]} {
		echo "init with nRESET held: $e"
	}
	reset_config srst_only srst_nogate connect_deassert_srst
	adapter deassert srst
	set armed 0
	for {set i 0} {$i < 3000} {incr i} {
		if {[catch {
			dap init
			suit.mem arp_examine
			suit.mem write_memory $DHCSR 32 $DBGKEY_C_HALT_C_DEBUGEN
			suit.mem write_memory $FP_COMP0 32 [expr {$handler | $FP_COMP_BE}]
			suit.mem write_memory $FP_CTRL 32 $FP_CTRL_KEY_ENABLE
		} last]} { continue }
		set armed 1
		break
	}
	if {!$armed} {
		echo "ARM NEVER LANDED ($last)"
		return
	}
	echo "armed after $i tries"
	for {set j 0} {$j < 50} {incr j} {
		if {![catch {set d [suit.mem read_memory $DHCSR 32 1]}] &&
		    ($d & $S_HALT)} {
			echo [format "RECOVERED: halted, DHCSR 0x%08x" $d]
			return
		}
	}
	echo "NOT HALTED"
}
