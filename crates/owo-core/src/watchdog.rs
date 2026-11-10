//! The watchdog feed job's declaration against the hardware window (docs/watchdog.md).
//!
//! WDOG0's period and warning are fixed once it is locked at boot (src/watchdog.c): only the feed
//! job's declaration can change, and only within that window.

/// Whether a feed job released every `period_us`, delivered up to `jitter_us` late, starting
/// within `deadline_us` of each release and running at most `budget_us`, always feeds before the
/// warning at `warning_us` after the previous feed. The longest gap between two feeds is a feed
/// at the start of one release's window and the next at the end of the next one's: a period plus
/// the jitter, the start-by and a run.
pub fn feed_fits_window(
    period_us: u64,
    jitter_us: u64,
    deadline_us: u64,
    budget_us: u64,
    warning_us: u64,
) -> bool {
    period_us + jitter_us + deadline_us + budget_us < warning_us
}

#[cfg(test)]
mod tests {
    use super::*;

    // The product's window: 62.53 ms period, warning at 75 %.
    const WARNING_US: u64 = 46_898;

    #[test]
    fn product_feed_fits() {
        assert!(feed_fits_window(20_000, 103, 10_000, 200, WARNING_US));
    }

    #[test]
    fn longer_period_outside_window_refused() {
        assert!(!feed_fits_window(40_000, 103, 10_000, 200, WARNING_US));
    }

    #[test]
    fn window_edge_is_outside() {
        assert!(!feed_fits_window(WARNING_US - 300, 100, 100, 100, WARNING_US));
    }
}
