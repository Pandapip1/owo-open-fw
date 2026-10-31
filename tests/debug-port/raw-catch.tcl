# SPDX-License-Identifier: Apache-2.0
#
# The suit procedure of 2026-10-31, by raw AHB-AP accesses only (a mem_ap
# target as in scratch/suit/raw.cfg, so OpenOCD never halts, resets or
# steps the core itself):
#   1. request a halt with DHCSR = DBGKEY | C_HALT | C_DEBUGEN and read
#      DHCSR back;
#   2. arm FPB comparator 0 on the reset handler and pulse SYSRESETREQ;
#   3. read DHCSR, then PC and SP through DCRSR/DCRDR;
#   4. resume, enable the DWT cycle counter and sample CYCCNT and PCSR twice.
# On the suit (stock firmware) step 1 read 0x01100001 (no halt), step 3 read
# 0x03130003 with PC at the bootloader reset handler and SP the initial MSP,
# and PCSR sampled the running core.

proc rd {name addr} {
	if {[catch {set v [read_memory $addr 32 1]} e]} {
		echo "$name: unreadable ($e)"
		return
	}
	echo [format "%s: 0x%08x" $name $v]
}

proc raw_catch {handler} {
	# Debug Control Block and FPB registers (Armv8-M ARM D1.2)
	set DHCSR 0xe000edf0
	set DCRSR 0xe000edf4
	set DCRDR 0xe000edf8
	set DEMCR 0xe000edfc
	set AIRCR 0xe000ed0c
	set FP_CTRL 0xe0002000
	set FP_COMP0 0xe0002008
	set DWT_CTRL 0xe0001000
	set DWT_CYCCNT 0xe0001004
	set DWT_PCSR 0xe000101c
	set DBGKEY_C_DEBUGEN 0xa05f0001
	set DBGKEY_C_HALT_C_DEBUGEN 0xa05f0003
	set FP_CTRL_KEY_ENABLE 3
	set FP_CTRL_KEY 2
	set FP_COMP_BE 1
	set AIRCR_VECTKEY_SYSRESETREQ 0x05fa0004
	set REGSEL_SP 13
	set REGSEL_DEBUGRETURNADDRESS 15
	set DEMCR_TRCENA 0x01000000
	set DWT_CTRL_CYCCNTENA 1
	set VECTOR_RESET 0x4
	set THUMB_BIT 1

	# "vector": catch the reset handler named by the vector table
	if {$handler eq "vector"} {
		set handler [expr {[read_memory $VECTOR_RESET 32 1] & ~$THUMB_BIT}]
	}
	rd DHCSR-before $DHCSR
	write_memory $DHCSR 32 $DBGKEY_C_HALT_C_DEBUGEN
	after 50
	rd DHCSR-after-C_HALT $DHCSR
	write_memory $DHCSR 32 $DBGKEY_C_DEBUGEN
	write_memory $FP_COMP0 32 [expr {$handler | $FP_COMP_BE}]
	write_memory $FP_CTRL 32 $FP_CTRL_KEY_ENABLE
	catch {write_memory $AIRCR 32 $AIRCR_VECTKEY_SYSRESETREQ}
	after 100
	rd DHCSR-after-reset $DHCSR
	write_memory $DCRSR 32 $REGSEL_DEBUGRETURNADDRESS
	rd PC $DCRDR
	write_memory $DCRSR 32 $REGSEL_SP
	rd SP $DCRDR
	rd vector-MSP 0x0
	rd vector-reset 0x4
	write_memory $FP_CTRL 32 $FP_CTRL_KEY
	write_memory $DEMCR 32 $DEMCR_TRCENA
	write_memory $DWT_CTRL 32 $DWT_CTRL_CYCCNTENA
	write_memory $DHCSR 32 $DBGKEY_C_DEBUGEN
	after 20
	rd CYCCNT $DWT_CYCCNT
	rd PCSR $DWT_PCSR
	after 20
	rd CYCCNT $DWT_CYCCNT
	rd PCSR $DWT_PCSR
}
