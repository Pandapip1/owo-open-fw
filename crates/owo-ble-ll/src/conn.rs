//! Connection state of the peripheral role: event timing (§4.5.1–4.5.3), window widening (§4.2.2),
//! channel selection per event, sequence numbers (§4.5.9), instants (§5.1.1) and supervision.
//!
//! Pure state, no radio access: the driver in [`crate::peripheral`] feeds it timestamps.
//! Constants taken from zephyr/subsys/bluetooth/controller/ll_sw (cited per item).

use crate::chan::{self, ChannelMap};
use crate::llcp::Ctrl;
use crate::pdu::ConnectInd;

/// Connection interval unit, µs (lll.h CONN_INT_UNIT_US).
pub const UNIT_US: u32 = 1250;
/// transmitWindowDelay for a CONNECT_IND on the 1M PHY, µs (pdu.h WIN_DELAY_LEGACY).
pub const WIN_DELAY_LEGACY_US: u32 = 1250;
/// Active-clock jitter allowance each side of a window, µs (lll_vendor.h EVENT_JITTER_US).
pub const EVENT_JITTER_US: u32 = 16;
/// Central sleep-clock accuracy by SCA code, worst case ppm (Vol 6 Part B Table 2.1; same table as
/// zephyr lll_clock.c sca_ppm_lut).
pub const SCA_PPM: [u32; 8] = [500, 250, 150, 100, 75, 50, 30, 20];
/// Error codes (Vol 1 Part F).
pub mod err {
    /// Connection timeout
    pub const CONN_TIMEOUT: u8 = 0x08;
    /// Remote user terminated
    pub const REMOTE_TERMINATED: u8 = 0x13;
    /// Connection terminated by local host
    pub const LOCAL_TERMINATED: u8 = 0x16;
    /// Unsupported remote feature
    pub const UNSUPPORTED_REMOTE_FEATURE: u8 = 0x1A;
    /// Instant passed
    pub const INSTANT_PASSED: u8 = 0x28;
    /// Connection failed to be established
    pub const FAILED_TO_ESTABLISH: u8 = 0x3E;
}

/// What the radio should do for the next connection event.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EventPlan {
    /// Channel index for this event
    pub channel: u8,
    /// Start listening at (absolute µs)
    pub listen_from_us: u32,
    /// Length of the listening window
    pub window_us: u32,
    /// connEventCounter of this event
    pub counter: u16,
}

#[derive(Clone, Copy, Debug)]
struct PendingUpdate {
    win_size: u8,
    win_offset: u16,
    interval: u16,
    latency: u16,
    timeout: u16,
    instant: u16,
}

/// Is `instant` already in the past at `counter`? (§5.1.1: (instant - counter) mod 2^16 >= 32767.)
pub fn instant_passed(instant: u16, counter: u16) -> bool {
    instant.wrapping_sub(counter) >= 32767
}

/// Peripheral-side connection.
#[derive(Clone, Debug)]
pub struct Connection {
    /// Access address
    pub access_address: u32,
    /// CRC preset
    pub crc_init: u32,
    interval_us: u32,
    /// interval in force before the last connection update (grid for the instant event)
    prev_interval_us: u32,
    latency: u16,
    timeout_us: u32,
    map: ChannelMap,
    hop: u8,
    last_unmapped: u8,
    csa2: bool,
    chan_id: u16,
    ppm: u32,
    /// connEventCounter of the *next* event to plan
    next_counter: u16,
    /// µs timestamp of the last anchor and the counter it belongs to
    anchor_us: u32,
    anchor_counter: u16,
    /// Time of the most recent valid packet from the central (supervision reference)
    last_rx_us: u32,
    established: bool,
    first_window: Option<(u32, u32)>,
    // sequence numbers
    sn: bool,
    nesn: bool,
    pending_update: Option<PendingUpdate>,
    pending_map: Option<(ChannelMap, u16)>,
    /// Window offset to add at the instant event (µs), consumed by the next plan
    extra_offset_us: u32,
    skipped_since_chan: u16,
}

/// Result of processing a packet from the central.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RxOutcome {
    /// The payload is new data (deliver it)
    pub new_data: bool,
    /// The central acknowledged our last transmitted PDU (send the next one)
    pub acked: bool,
}

impl Connection {
    /// Start a connection from CONNECT_IND. `rx_end_us` is when the last bit of CONNECT_IND left
    /// the central; `local_ppm` is our own sleep-clock accuracy (a board parameter!). Returns
    /// the connection and the plan for its first event.
    pub fn from_connect_ind(c: &ConnectInd, rx_end_us: u32, local_ppm: u32, use_csa2: bool) -> (Connection, EventPlan) {
        let start = rx_end_us.wrapping_add(WIN_DELAY_LEGACY_US + c.win_offset as u32 * UNIT_US);
        let win = c.win_size as u32 * UNIT_US;
        let map = ChannelMap(c.chm);
        let mut conn = Connection {
            access_address: c.access_address,
            crc_init: c.crc_init,
            interval_us: c.interval as u32 * UNIT_US,
            prev_interval_us: c.interval as u32 * UNIT_US,
            latency: c.latency,
            timeout_us: c.timeout as u32 * 10_000,
            map,
            hop: c.hop,
            last_unmapped: 0,
            csa2: use_csa2,
            chan_id: chan::channel_id(c.access_address),
            ppm: local_ppm + SCA_PPM[(c.sca & 7) as usize],
            next_counter: 0,
            anchor_us: start,
            anchor_counter: 0,
            last_rx_us: rx_end_us,
            established: false,
            first_window: Some((start.wrapping_sub(EVENT_JITTER_US), win + 2 * EVENT_JITTER_US)),
            sn: false,
            nesn: false,
            pending_update: None,
            pending_map: None,
            extra_offset_us: 0,
            skipped_since_chan: 0,
        };
        let plan = conn.plan_next();
        (conn, plan)
    }

    /// Our current SN.
    pub fn sn(&self) -> bool {
        self.sn
    }

    /// Our current NESN.
    pub fn nesn(&self) -> bool {
        self.nesn
    }

    /// connInterval in µs.
    pub fn interval_us(&self) -> u32 {
        self.interval_us
    }

    /// Has the first packet been received (§4.5.5: the connection is then "established")?
    pub fn is_established(&self) -> bool {
        self.established
    }

    fn channel_for(&mut self, counter: u16) -> u8 {
        if self.csa2 {
            chan::csa2(counter, self.chan_id, &self.map)
        } else {
            let (u, used) = chan::csa1(self.last_unmapped, self.hop, self.skipped_since_chan, &self.map);
            self.last_unmapped = u;
            self.skipped_since_chan = 0;
            used
        }
    }

    /// Apply any procedure whose instant is the event about to be planned. Returns
    /// `Err(INSTANT_PASSED)` if an instant was missed.
    fn apply_instants(&mut self, counter: u16) -> Result<(), u8> {
        if let Some((map, inst)) = self.pending_map {
            if inst == counter {
                self.map = map;
                self.pending_map = None;
            } else if instant_passed(inst, counter) {
                return Err(err::INSTANT_PASSED);
            }
        }
        if let Some(u) = self.pending_update {
            if u.instant == counter {
                self.prev_interval_us = self.interval_us;
                self.interval_us = u.interval as u32 * UNIT_US;
                self.latency = u.latency;
                self.timeout_us = u.timeout as u32 * 10_000;
                self.extra_offset_us = u.win_offset as u32 * UNIT_US;
                self.first_window = Some((0, u.win_size as u32 * UNIT_US));
                self.pending_update = None;
            } else if instant_passed(u.instant, counter) {
                return Err(err::INSTANT_PASSED);
            }
        }
        Ok(())
    }

    /// Plan the next event and advance the event counter. Call once per event (including events
    /// missed because the previous anchor was lost: the widening then grows with the elapsed
    /// time since the last anchor).
    pub fn plan_next(&mut self) -> EventPlan {
        let counter = self.next_counter;
        // An instant error is reported through `check_instant`; planning continues so the
        // caller can still terminate cleanly.
        let _ = self.apply_instants(counter);
        let events = counter.wrapping_sub(self.anchor_counter) as u32;
        let channel = self.channel_for(counter);
        self.next_counter = counter.wrapping_add(1);
        self.skipped_since_chan = 0;
        if let Some((from, win)) = self.first_window.take() {
            if from != 0 {
                return EventPlan { channel, listen_from_us: from, window_us: win, counter };
            }
            // connection update: window starts after the old-interval grid plus the offset
            let expected = self
                .anchor_us
                .wrapping_add(self.old_grid_us(events))
                .wrapping_add(self.extra_offset_us);
            self.extra_offset_us = 0;
            return EventPlan { channel, listen_from_us: expected.wrapping_sub(EVENT_JITTER_US), window_us: win + 2 * EVENT_JITTER_US, counter };
        }
        let elapsed = events * self.interval_us;
        let expected = self.anchor_us.wrapping_add(elapsed);
        let mut widening = (self.ppm * elapsed).div_ceil(1_000_000);
        let cap = (self.interval_us / 2).saturating_sub(crate::radio::T_IFS_US);
        widening = widening.min(cap);
        EventPlan {
            channel,
            listen_from_us: expected.wrapping_sub(widening + EVENT_JITTER_US),
            window_us: 2 * (widening + EVENT_JITTER_US),
            counter,
        }
    }

    /// Time from the last anchor to the instant event on the old interval grid.
    fn old_grid_us(&self, events: u32) -> u32 {
        events * self.prev_interval_us
    }

    /// Record the anchor: the access-address start of the first valid packet of event `counter`.
    pub fn set_anchor(&mut self, counter: u16, start_us: u32) {
        self.anchor_us = start_us;
        self.anchor_counter = counter;
        self.last_rx_us = start_us;
        self.established = true;
    }

    /// Process the header of a packet received with a good CRC. Implements the acknowledgement
    /// rules of §4.5.9.
    pub fn on_header(&mut self, sn: bool, nesn: bool) -> RxOutcome {
        let new_data = sn == self.nesn;
        if new_data {
            self.nesn = !self.nesn;
        }
        let acked = nesn != self.sn;
        if acked {
            self.sn = !self.sn;
        }
        RxOutcome { new_data, acked }
    }

    /// Has the supervision timeout expired at `now`? Before establishment the limit is six
    /// connection intervals (§4.5.2).
    pub fn supervision_expired(&self, now_us: u32) -> bool {
        let limit = if self.established { self.timeout_us } else { 6 * self.interval_us };
        now_us.wrapping_sub(self.last_rx_us) > limit
    }

    /// Handle a control PDU that changes connection state. Returns an error code if the
    /// procedure must be rejected (instant already passed).
    pub fn on_ctrl(&mut self, c: &Ctrl) -> Result<(), u8> {
        match *c {
            Ctrl::ConnUpdate { win_size, win_offset, interval, latency, timeout, instant } => {
                if instant_passed(instant, self.next_counter) {
                    return Err(err::INSTANT_PASSED);
                }
                self.pending_update = Some(PendingUpdate { win_size, win_offset, interval, latency, timeout, instant });
            }
            Ctrl::ChanMap { map, instant } => {
                if instant_passed(instant, self.next_counter) {
                    return Err(err::INSTANT_PASSED);
                }
                self.pending_map = Some((ChannelMap(map), instant));
            }
            _ => {}
        }
        Ok(())
    }

    /// Peripheral latency in events.
    pub fn latency(&self) -> u16 {
        self.latency
    }
}
