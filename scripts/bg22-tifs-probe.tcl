# EFR32BG22 radio event timestamps over the debugger (docs/radio-tables.md
# SS7.71). PRS routes three MODEM events into TIMER1 input capture while the
# firmware runs untouched:
#   CC0 = MODEMH.EOF (end of a received frame) reloads and starts TIMER1,
#   CC1 = MODEMH.SYNCSENT (sync word transmitted) and
#   CC2 = MODEM.FRAMESENT (frame transmitted) capture its count.
# CC2 - CC1 is the transmitted PDU+CRC airtime, which identifies the packet
# and calibrates the tick rate; CC1 after a received frame is the RX-end to
# TX-sync interval of an RX->TX turnaround.
#
# Usage (TIMER1, PRS async channels 9-11 and their TIMER1 consumers must be
# unused by the firmware under test):
#   openocd -f interface/jlink.cfg -c 'adapter serial 000440200548' -c 'transport select swd' \
#     -f target/silabs/xg21.cfg -f scripts/bg22-tifs-probe.tcl \
#     -c init -c tifs_setup -c 'tifs_watch 30000' -c shutdown
# Each "C <cc1> <cc2>" line is a new capture pair, in TIMER1 ticks. TIMER1
# runs from EM01GRPACLK: read CMU.EM01GRPACLKCTRL (0x50008120) to know which.
#
# Offsets and encodings are from the EFR32BG22 device headers
# (efr32bg22_timer.h, efr32bg22_prs.h, efr32bg22_prs_signals.h,
# efr32bg22_cmu.h).

set CMU_CLKEN0_SET      0x50009064
set CMU_CLKEN0_TIMER1   [expr {1 << 5}]
set CMU_CLKEN0_PRS      [expr {1 << 27}]

set PRS_BASE            0x50038000
set PRS_ASYNC_CH0_CTRL  0x018
set PRS_ASYNC_CH_STRIDE 4
set PRS_FNSEL_A         [expr {0xC << 16}]
set PRS_SOURCESEL_SHIFT 8
set PRS_SRC_MODEM       0x2B
set PRS_SRC_MODEMH      0x2C
set PRS_SIG_MODEM_FRAMESENT 0
set PRS_SIG_MODEMH_SYNCSENT 2
set PRS_SIG_MODEMH_EOF  5
set PRS_CONSUMER_TIMER1_CC0 0x124
set PRS_CONSUMER_TIMER1_CC1 0x128
set PRS_CONSUMER_TIMER1_CC2 0x12C
set PROBE_CH_EOF        9
set PROBE_CH_SYNCSENT   10
set PROBE_CH_FRAMESENT  11

set TIMER1_BASE         0x5004C000
set TIMER_CFG           0x004
set TIMER_CTRL          0x008
set TIMER_TOP           0x01C
set TIMER_EN            0x030
set TIMER_CC0_CFG       0x060
set TIMER_CC1_CFG       0x080
set TIMER_CC2_CFG       0x0A0
set TIMER_CC1_ICOF      0x098
set TIMER_CC2_ICOF      0x0B8
set TIMER_CFG_OSMEN     [expr {1 << 4}]
set TIMER_CC_CFG_MODE_INPUTCAPTURE 1
# The MODEM events are pulses: PRSASYNCLEVEL captures none of them, and
# PRSSYNC selects the synchronous channels (SPRSSEL), not these.
set TIMER_CC_CFG_INSEL_PRSASYNCPULSE [expr {6 << 16}]
set TIMER_CTRL_RISEA_RELOADSTART 3
set TIMER_TOP_16BIT     0xFFFF

proc tifs_w {address value} { mww [format 0x%08x $address] [format 0x%08x $value] }
proc tifs_r {address} { return [read_memory [format 0x%08x $address] 32 1] }

proc tifs_setup {} {
    global CMU_CLKEN0_SET CMU_CLKEN0_TIMER1 CMU_CLKEN0_PRS PRS_BASE PRS_ASYNC_CH0_CTRL \
        PRS_ASYNC_CH_STRIDE PRS_FNSEL_A PRS_SOURCESEL_SHIFT PRS_SRC_MODEM PRS_SRC_MODEMH \
        PRS_SIG_MODEM_FRAMESENT PRS_SIG_MODEMH_SYNCSENT PRS_SIG_MODEMH_EOF \
        PRS_CONSUMER_TIMER1_CC0 PRS_CONSUMER_TIMER1_CC1 PRS_CONSUMER_TIMER1_CC2 \
        PROBE_CH_EOF PROBE_CH_SYNCSENT PROBE_CH_FRAMESENT TIMER1_BASE TIMER_CFG TIMER_CTRL \
        TIMER_TOP TIMER_EN TIMER_CC0_CFG TIMER_CC1_CFG TIMER_CC2_CFG TIMER_CFG_OSMEN \
        TIMER_CC_CFG_MODE_INPUTCAPTURE TIMER_CC_CFG_INSEL_PRSASYNCPULSE \
        TIMER_CTRL_RISEA_RELOADSTART TIMER_TOP_16BIT

    # Both blocks fault on access while their bus clock is off.
    tifs_w $CMU_CLKEN0_SET [expr {$CMU_CLKEN0_TIMER1 | $CMU_CLKEN0_PRS}]
    foreach {ch src sig} [list \
            $PROBE_CH_EOF $PRS_SRC_MODEMH $PRS_SIG_MODEMH_EOF \
            $PROBE_CH_SYNCSENT $PRS_SRC_MODEMH $PRS_SIG_MODEMH_SYNCSENT \
            $PROBE_CH_FRAMESENT $PRS_SRC_MODEM $PRS_SIG_MODEM_FRAMESENT] {
        set ctrl [expr {$PRS_BASE + $PRS_ASYNC_CH0_CTRL + $PRS_ASYNC_CH_STRIDE * $ch}]
        echo [format "PRS ch%d was 0x%08x" $ch [tifs_r $ctrl]]
        tifs_w $ctrl [expr {$PRS_FNSEL_A | ($src << $PRS_SOURCESEL_SHIFT) | $sig}]
    }
    tifs_w [expr {$PRS_BASE + $PRS_CONSUMER_TIMER1_CC0}] $PROBE_CH_EOF
    tifs_w [expr {$PRS_BASE + $PRS_CONSUMER_TIMER1_CC1}] $PROBE_CH_SYNCSENT
    tifs_w [expr {$PRS_BASE + $PRS_CONSUMER_TIMER1_CC2}] $PROBE_CH_FRAMESENT

    # CFG and CCn.CFG are writable only while the timer is disabled. One-shot:
    # the counter stops at TOP, so a capture long after EOF reads TOP.
    tifs_w [expr {$TIMER1_BASE + $TIMER_EN}] 0
    tifs_w [expr {$TIMER1_BASE + $TIMER_CFG}] $TIMER_CFG_OSMEN
    foreach cfg [list $TIMER_CC0_CFG $TIMER_CC1_CFG $TIMER_CC2_CFG] {
        tifs_w [expr {$TIMER1_BASE + $cfg}] \
            [expr {$TIMER_CC_CFG_MODE_INPUTCAPTURE | $TIMER_CC_CFG_INSEL_PRSASYNCPULSE}]
    }
    tifs_w [expr {$TIMER1_BASE + $TIMER_EN}] 1
    tifs_w [expr {$TIMER1_BASE + $TIMER_TOP}] $TIMER_TOP_16BIT
    tifs_w [expr {$TIMER1_BASE + $TIMER_CTRL}] $TIMER_CTRL_RISEA_RELOADSTART
}

# Debugger reads of CCn.ICF do not pop the capture FIFO, so once it is full
# every new capture lands in CCn.ICOF. Print each change of the CC1/CC2 pair.
proc tifs_watch {ms} {
    global TIMER1_BASE TIMER_CC1_ICOF TIMER_CC2_ICOF
    set previous_sync -1
    set previous_frame -1
    set end [expr {[clock milliseconds] + $ms}]
    while {[clock milliseconds] < $end} {
        set sync [tifs_r [expr {$TIMER1_BASE + $TIMER_CC1_ICOF}]]
        set frame [tifs_r [expr {$TIMER1_BASE + $TIMER_CC2_ICOF}]]
        if {$sync != $previous_sync || $frame != $previous_frame} {
            echo "C [expr {$sync}] [expr {$frame}]"
            set previous_sync $sync
            set previous_frame $frame
        }
    }
}
