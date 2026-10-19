//! Turns accepted frames into a schedule of pulses.
//!
//! Time-source agnostic: the caller passes a monotonic microsecond clock and
//! asks what to do next. There is no periodic tick; when nothing is playing
//! the engine says [`Next::Idle`] so the platform can sleep indefinitely.
//!
//! IMPORTANT: this is an INTERPRETATION of the frame fields, not a copy of the
//! stock stimulation code, which has not been decompiled.

use crate::proto::{Frame, Width, NUM_CHANNELS};

pub const QUEUE_DEPTH: usize = 8;

/// Hard ceiling on any level the engine will ever emit. 80 is the cap the
/// 1.6.82 app applies to every channel before sending a frame.
pub const LEVEL_CAP: u8 = 80;

/// Spacing between channels inside one pulse period (unverified; the stock
/// firmware's channel sequencing was not traced).
pub const CHANNEL_STAGGER_US: u64 = 1000;

/// Frame timing fields are in 100 ms units.
const UNITS_US: u64 = 100_000;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Pulse {
    pub at_us: u64,
    /// Wire-order index 0..9.
    pub channel: u8,
    /// 1..=LEVEL_CAP, after ramp scaling and limits.
    pub level: u8,
    pub width: Width,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Next {
    /// Emit this pulse now, then call `next` again.
    Pulse(Pulse),
    /// Nothing due; call `next` again at this absolute time (or earlier if new input arrives).
    Wake(u64),
    /// Nothing queued or playing; sleep until new input arrives.
    Idle,
}

#[derive(Debug)]
pub struct Engine {
    widths: [Width; NUM_CHANNELS],
    limit: [u8; NUM_CHANNELS],
    queue: [Frame; QUEUE_DEPTH],
    q_head: usize,
    q_count: usize,
    playing: bool,
    cur: Frame,
    cur_start_us: u64,
    /// Where the next frame would start if chained.
    cursor_us: u64,
    /// Pulse index within the current frame.
    k: u32,
    /// Channel index within the current pulse period.
    c: usize,
}

impl Default for Engine {
    fn default() -> Self {
        Self::new()
    }
}

impl Engine {
    pub const fn new() -> Engine {
        Engine {
            // power-on widths: all 50/50/0 (decode of 0x00) until the client writes
            widths: [Width::decode(0x00); NUM_CHANNELS],
            limit: [LEVEL_CAP; NUM_CHANNELS],
            queue: [Frame::ZERO; QUEUE_DEPTH],
            q_head: 0,
            q_count: 0,
            playing: false,
            cur: Frame::ZERO,
            cur_start_us: 0,
            cursor_us: 0,
            k: 0,
            c: 0,
        }
    }

    pub fn set_widths(&mut self, w: &[Width; NUM_CHANNELS]) {
        self.widths = *w;
    }

    /// Per-channel ceilings; values above [`LEVEL_CAP`] are lowered to it.
    pub fn set_limits(&mut self, lim: &[u8; NUM_CHANNELS]) {
        for (o, l) in self.limit.iter_mut().zip(lim) {
            *o = (*l).min(LEVEL_CAP);
        }
    }

    pub fn stop(&mut self) {
        self.q_head = 0;
        self.q_count = 0;
        self.playing = false;
    }

    /// A frame with `first` set flushes everything before it. `Err(())` when the queue is full.
    pub fn push(&mut self, f: &Frame, now_us: u64) -> Result<(), ()> {
        if f.first {
            self.stop();
            self.cursor_us = now_us;
        }
        if self.q_count == QUEUE_DEPTH {
            return Err(());
        }
        self.queue[(self.q_head + self.q_count) % QUEUE_DEPTH] = *f;
        self.q_count += 1;
        if !self.playing && self.cursor_us < now_us {
            self.cursor_us = now_us; // idle: start now, do not replay the past
        }
        Ok(())
    }

    pub fn is_idle(&self) -> bool {
        !self.playing && self.q_count == 0
    }

    fn pop_frame(&mut self) -> bool {
        if self.q_count == 0 {
            return false;
        }
        self.cur = self.queue[self.q_head];
        self.q_head = (self.q_head + 1) % QUEUE_DEPTH;
        self.q_count -= 1;
        self.cur_start_us = self.cursor_us;
        self.k = 0;
        self.c = 0;
        self.playing = true;
        true
    }

    pub fn next(&mut self, now_us: u64) -> Next {
        loop {
            if !self.playing && !self.pop_frame() {
                return Next::Idle;
            }
            let f = self.cur;
            let len = f.total as u64 * UNITS_US;
            let hz = f.freq_hz.max(1) as u64;
            let t_k = self.k as u64 * 1_000_000 / hz;

            if t_k >= len {
                // frame finished: chain the next one gaplessly
                self.cursor_us = self.cur_start_us + len;
                self.playing = false;
                continue;
            }
            if self.c >= NUM_CHANNELS {
                self.c = 0;
                self.k += 1;
                continue;
            }
            if f.level[self.c] == 0 {
                // silent channel: no time cost
                self.c += 1;
                continue;
            }

            let at = self.cur_start_us + t_k + self.c as u64 * CHANNEL_STAGGER_US;
            if at > now_us {
                return Next::Wake(at);
            }

            match level_at(&f, self.c, self.limit[self.c], t_k) {
                Level::Rest { next_cycle_us } => {
                    // rest phase: jump to the first pulse of the next cycle
                    self.k = ((next_cycle_us * hz + 999_999) / 1_000_000) as u32;
                    self.c = 0;
                }
                Level::Active(0) => self.c += 1,
                Level::Active(lvl) => {
                    let p = Pulse {
                        at_us: at,
                        channel: self.c as u8,
                        level: lvl,
                        width: self.widths[self.c],
                    };
                    self.c += 1;
                    return Next::Pulse(p);
                }
            }
        }
    }
}

enum Level {
    Active(u8),
    /// Frame-relative start of the next work window.
    Rest { next_cycle_us: u64 },
}

/// Level of channel `c` at frame-relative time `t_us`, or `Rest` in a rest phase.
fn level_at(f: &Frame, c: usize, limit: u8, t_us: u64) -> Level {
    let work = f.work as u64 * UNITS_US;
    let cyc = (f.work as u64 + f.rest as u64) * UNITS_US;
    let pos = if cyc != 0 { t_us % cyc } else { t_us };

    if cyc != 0 && pos >= work {
        return Level::Rest { next_cycle_us: t_us - pos + cyc };
    }

    let up = f.ramp_up as u64 * UNITS_US;
    let down = f.ramp_down as u64 * UNITS_US;
    let mut q: u64 = 1024; // Q10 factor

    if up != 0 && pos < up {
        q = pos * 1024 / up;
    }
    if down != 0 && work > down && pos > work - down {
        q = q.min((work - pos) * 1024 / down);
    }

    let lvl = f.level[c].min(limit) as u64;
    Level::Active(((lvl * q + 512) / 1024) as u8)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct Run {
        n: [u32; NUM_CHANNELS],
        total: u32,
        last_at: u64,
        max_level: u8,
        wakeups: u32,
        probe: Option<u8>,
    }

    /// Drive the engine like the platform layer would: jump the clock straight
    /// to each wake-up, never poll.
    fn drain(e: &mut Engine, t0: u64, probe_ch0_ms: Option<u64>) -> Run {
        let mut r = Run::default();
        let mut now = t0;
        loop {
            match e.next(now) {
                Next::Pulse(p) => {
                    r.n[p.channel as usize] += 1;
                    r.total += 1;
                    r.max_level = r.max_level.max(p.level);
                    r.last_at = p.at_us;
                    if p.channel == 0 && Some((p.at_us - t0) / 1000) == probe_ch0_ms && (p.at_us - t0) % 1000 == 0 {
                        r.probe = Some(p.level);
                    }
                }
                Next::Wake(w) => {
                    now = w;
                    r.wakeups += 1;
                }
                Next::Idle => break,
            }
        }
        r
    }

    fn base() -> Frame {
        Frame { freq_hz: 100, total: 5, work: 5, rest: 0, first: true, ..Default::default() }
    }

    #[test]
    fn hundred_hz_two_channels() {
        let mut e = Engine::new();
        let mut f = base();
        f.level[4] = 60;
        f.level[8] = 40;
        e.push(&f, 1_000_000).unwrap();
        let r = drain(&mut e, 1_000_000, None);
        assert!(r.n[4] == 50 && r.n[8] == 50 && r.total == 100);
        assert_eq!(r.max_level, 60);
        assert!(r.last_at < 1_000_000 + 500_000 + 4_000);
        assert!(e.is_idle());
        // wake-ups are per pulse, not per millisecond: a 1 kHz tick would need 500
        assert!(r.wakeups <= 110, "{}", r.wakeups);
    }

    #[test]
    fn rest_phase() {
        let mut e = Engine::new();
        let mut g = Frame { freq_hz: 100, total: 5, work: 2, rest: 3, first: true, ..Default::default() };
        g.level[0] = 50;
        e.push(&g, 0).unwrap();
        assert_eq!(drain(&mut e, 0, None).n[0], 20);

        // two cycles: work 1 rest 1, total 4 -> 10 + 10 pulses
        let mut e = Engine::new();
        let mut h = Frame { freq_hz: 100, total: 4, work: 1, rest: 1, first: true, ..Default::default() };
        h.level[0] = 50;
        e.push(&h, 0).unwrap();
        assert_eq!(drain(&mut e, 0, None).n[0], 20);
    }

    #[test]
    fn ramp_up() {
        // ramp up over 200 ms: level halves at 100 ms, full at 200 ms+
        let mut e = Engine::new();
        let mut ru = Frame { freq_hz: 100, total: 5, work: 5, ramp_up: 2, first: true, ..Default::default() };
        ru.level[0] = 80;
        e.push(&ru, 0).unwrap();
        let r = drain(&mut e, 0, Some(100));
        let l = r.probe.expect("pulse at 100 ms");
        assert!((39..=41).contains(&l), "{l}");
        assert_eq!(r.max_level, 80);
    }

    #[test]
    fn never_above_cap() {
        let mut e = Engine::new();
        let mut hi = Frame { freq_hz: 10, total: 1, work: 1, first: true, ..Default::default() };
        hi.level[3] = 255;
        e.push(&hi, 0).unwrap();
        let r = drain(&mut e, 0, None);
        assert!(r.max_level == LEVEL_CAP && r.n[3] == 1);
    }

    #[test]
    fn chaining_and_flush() {
        let mut e = Engine::new();
        let mut a = Frame { freq_hz: 100, total: 1, work: 1, first: true, ..Default::default() };
        a.level[0] = 30;
        let mut b = a;
        b.first = false;
        b.level[0] = 70;
        e.push(&a, 0).unwrap();
        e.push(&b, 0).unwrap();
        let r = drain(&mut e, 0, None);
        assert!(r.n[0] == 20 && r.max_level == 70);
        e.push(&b, 0).unwrap();
        e.push(&a, 0).unwrap(); // a.first flushes b
        let r = drain(&mut e, 0, None);
        assert!(r.n[0] == 10 && r.max_level == 30);
    }

    #[test]
    fn stop_is_idle() {
        let mut e = Engine::new();
        e.push(&base(), 0).unwrap();
        e.stop();
        assert_eq!(e.next(1000), Next::Idle);
        assert!(e.is_idle());
    }

    #[test]
    fn queue_overflow() {
        let mut e = Engine::new();
        let mut q = Frame { freq_hz: 1, total: 1, work: 1, ..Default::default() };
        q.level[0] = 1;
        let ok = (0..QUEUE_DEPTH + 3).filter(|_| e.push(&q, 0).is_ok()).count();
        assert_eq!(ok, QUEUE_DEPTH);
    }

    #[test]
    fn per_channel_ceiling_only_lowers() {
        let mut e = Engine::new();
        let mut lim = [80u8; NUM_CHANNELS];
        lim[0] = 20;
        e.set_limits(&lim);
        let mut lf = Frame { freq_hz: 10, total: 1, work: 1, first: true, ..Default::default() };
        lf.level[0] = 60;
        lf.level[1] = 60;
        e.push(&lf, 0).unwrap();
        let (mut s0, mut s1, mut now) = (0, 0, 0);
        loop {
            match e.next(now) {
                Next::Pulse(p) => match p.channel {
                    0 => { assert_eq!(p.level, 20); s0 += 1 }
                    1 => { assert_eq!(p.level, 60); s1 += 1 }
                    _ => unreachable!(),
                },
                Next::Wake(w) => now = w,
                Next::Idle => break,
            }
        }
        assert!(s0 == 1 && s1 == 1);
    }
}
