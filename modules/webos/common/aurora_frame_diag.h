#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Frame pacing / presentation diagnostic (observation only).
 *
 * Enable at runtime: AURORA_FRAME_DIAG=1
 * Log path:         AURORA_FRAME_DIAG_PATH=/tmp/aurora_frame_diag.ndjson  (default)
 * Size cap:         AURORA_FRAME_DIAG_MAX_BYTES=16777216  (default, 0 = unbounded)
 *
 * The file is truncated once, at the first session of the process, and appended to from
 * then on: media reloads and stream restarts are the events the log has to explain, so
 * nothing that happens during a run may drop what was recorded before it. What bounds the
 * file is the size cap — /tmp is RAM on this TV — and reaching it is written into the log
 * as a `log_capped` record rather than the file simply going quiet.
 *
 * Every record carries `session`, which counts sessions within the process; `frame`
 * restarts at 0 with each session and is only meaningful together with it.
 *
 * When disabled, all calls are cheap no-ops (one atomic load).
 * Does not change PTS pacing, pauseAtDecodeTime, or feed cadence logic.
 */
bool AuroraFrameDiagEnabled(void);

void AuroraFrameDiagBeginSession(const char *backend);

void AuroraFrameDiagEndSession(void);

/** Monotonic clock shared with the log records, so callers can timestamp before submitting. */
uint64_t AuroraFrameDiagNowNs(void);

/** Call immediately before Feed(), with the PTS about to be submitted (ns or backend units). */
void AuroraFrameDiagLogFeed(uint64_t pts, int render_queue_length);

/**
 * Same record plus decoder-submit cost. `feed_wall_ns` is taken before handing the frame to the
 * decoder; `submit_us` is how long that handoff blocked, which is the only visible sign of
 * backpressure on backends that expose no queue depth. Both `submit_us` and `bytes` are 0 when
 * the caller does not measure them.
 */
void AuroraFrameDiagLogFeedAt(uint64_t pts, int render_queue_length, uint64_t feed_wall_ns,
                              uint64_t submit_us, uint32_t bytes);

/** SMP Load() callback tap for backpressure / drop events. */
void AuroraFrameDiagLogEvent(int event_type, int64_t num_value, const char *str_value);

#ifdef __cplusplus
}
#endif
