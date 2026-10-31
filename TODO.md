# TODO

Open work for the open EFR32BG22 radio stack and its QEMU model. Evidence
lives in `docs/radio-tables.md` (section numbers below). Evidence bar for
every timing or behaviour: a direct silicon measurement with a calibrated
timebase and a range. Where it is not yet met, the code uses a named
placeholder marked unmeasured.

## Standing requests

- QEMU models everything it is supposed to model, at least as a named
  constant for now. Timing is in scope; RF propagation and physics (path
  loss, fading, interference, noise-driven bit errors) are not.
- Every timing, in firmware and in the model, is backed by strong silicon
  evidence (calibrated timestamps, repeated, with a range), ideally
  differential against stock where hidden delays are involved.
- Timers: arming, disarming, and every latency modelled as silicon does.
- Enable-to-running delays: CMU clock enable to peripheral live,
  oscillator enable to ready (HFXO, DPLL, LFXO, LFRCO), timer EN/START to
  first count, LF-domain write synchronization.
- No magic numbers; proprietary-derived material only in the baselines
  directory; understand disproven hypotheses before moving on.

## QEMU model

- [x] SEQIEN-enabled state events hold RAC transitions; SHUTDOWN latches its
      event; all 14 state IRQs wired (§7.69)
- [x] PA ramp completion survives the post-PARAMP clear (§7.69)
- [x] Receive side and RX/TX turnarounds: TX2RX, RXFRAME, RX2TX, RX2RX,
      RXPD, STIMER (§7.70)
- [ ] RXWARM (state 1). On silicon the open flow never enters it (IRQ1 count
      stays 0); stock's routes into it are not yet characterised (in progress)
- [ ] Disables (TXDIS/RXDIS/FORCEDISABLE) in each busy state, as silicon
      does (in progress)
- [ ] `RAC.STATUS` bits: DEMODENS, SEQSLEEPING, and SEQACTIVE/TXENS/RXENS in
      every state (in progress)
- [ ] Clock gating: an access with the bus clock off is a bus fault (measured
      for TIMER1, PRS, PROTIMER); RDMAILBOX0/1, RDSCRATCHPAD, PRORTC, SYSCFG
      not yet gated (in progress)
- [ ] Frame airtime = bits / 1 Mbps (measured exactly, §7.71) instead of the
      synthetic 150 us (in progress)
- [ ] State-transition latencies measured on silicon: TXWARM, RXWARM, TX2RX,
      RX2TX, TXPD, RXPD, synth lock, PA ramp (in progress)
- [ ] RX2TX → TX start reproduces silicon's turnaround for the open image
      (EOF → SCAN_RSP sync 180.5 us, §7.71)
- [ ] Trailer/RSSI status fields and LO-counter/VCO calibration readback as
      named constants
- [ ] Timers: BURTC, TIMER0-4, PROTIMER, RTCC: rates, write sync, compare
      arm lead, match-to-IRQ, stop/disarm latency, enable-to-running (in
      progress, separate worktree branch `timer-fidelity`)
- [ ] Oscillators: HFXO/DPLL/LFXO/LFRCO enable-to-ready and STATUS bits
      (in progress, same branch)
- [ ] The two cores run at half the icount rate each when both are busy,
      so the M33 vs M0+ speed ratio differs from silicon
- [ ] `run-qemu-*-e2e.sh` match stale traces (`RAC state 9 -> 0`, 8-byte
      SCAN_RSP) and need `xxd`

## Firmware timing evidence

- [x] T_IFS matched to the stock stack on silicon: RX2TX hold 135.6 us,
      EOF → SCAN_RSP sync 180.5 us vs stock 180.8 us (§7.71)
- [x] TX+RX chain delay sum ~9.2-9.7 us, from two independent peers
      (stock stack, laptop controller) (§7.71)
- [ ] `radio_tx_ready_delay_get`, `radio_rx_ready_delay_get`,
      `radio_tx_chain_delay_get`, `radio_rx_chain_delay_get` all return 0.
      Measured so far: RAC ACTIVE↑ → ADV_IND SYNCSENT 131.7 us. The TX/RX
      chain-delay split needs an external receiver; otherwise it is [I]
- [ ] `efr32_seq_delay(n)`: actual duration on silicon (it reproduces
      stock's loop; never measured). It sets every analog settle wait:
      PA 6/2/8/3/1 us, RX LDO 7 us, DC cal 16/8/1 us
- [ ] `RAC.STIMER` rate (~4.8 ticks/us, from BURTC): calibrate against a
      calibrated timer
- [ ] RX2TX decision wait (120 us): justify from measured M33 decision
      latency
- [ ] Bounded polls (TXWARM, ramp, LO count): measure typical loop counts
      against their bounds
- [ ] `CONFIG_CLOCK_EVENT_SCHED_LATE_TOLERANCE_NS` (61036) and the TIMER0
      clock-event minimum lead (2000 ns): evidence

## Radio prerequisites (which blocks and timers must be on)

- [x] §4: BUFC and PROTIMER must be enabled before RAC with the vendor
      sequencer. Not true for the open image: no PROTIMER.EN at all works (§7.71)
- [x] TX and RX do not need the PROTIMER counter running: stage 3 runs with
      CTRL=0 and its counter at 0 (§7.71)
- [x] TX and RX keep working after PROTIMER.EN is cleared, or its bus clock
      gated, at runtime (§7.71)
- [x] PRORTC, RTCC, SYSCFG, RDMAILBOX0/1 and RDSCRATCHPAD clocks are not
      needed by the open image (all off in stage 3)
- [x] Boot-time order re-tested: a build that never enables PROTIMER works
- [ ] Meaning of RAC.STATUS bit 28 (0x10400000 vs 0x00400000 before the
      first TXEN), which differs without PROTIMER.EN
- [ ] Minimal clock set for TX and for RX, by knock-out of each remaining
      CLKEN bit

## Hardware and tooling

- [x] `scripts/bg22-tifs-probe.tcl`: PRS → TIMER1 event timestamps (§7.71)
- [ ] Absolute T_IFS against an external receiver (SDR or sniffer); today it
      is matched to stock and cross-checked against the host controller
- [ ] Scanner (`radio-rx-port`) still needs `QEMU_TIFS`: the driver only arms
      RX→TX after a TX→RX turnaround
- [ ] `vendor/zephyr-lang-rust` was pointed at an unpushed commit before
      `63e9afd`; check that other submodule bumps always land after a push
