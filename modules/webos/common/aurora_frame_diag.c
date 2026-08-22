#include "aurora_frame_diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <errno.h>

#define AURORA_FRAME_DIAG_BUF_SIZE (256 * 1024)
#define AURORA_FRAME_DIAG_FLUSH_NS (1000000000ULL)
/*
 * Ceiling for the whole log, in bytes. The file lives on /tmp, which is RAM on this TV, and
 * a 4K120 stream writes one ~200 byte record per frame — roughly 24 KB/s, ~85 MB an hour.
 * Since the log is only truncated once per process, an unbounded append would spend the
 * memory the diagnostic is supposed to be measuring. 16 MB buys about eleven minutes at
 * 4K120 and twice that at 60 fps; AURORA_FRAME_DIAG_MAX_BYTES raises it for a longer
 * reproduction, 0 removes the cap for a run that writes somewhere other than tmpfs.
 */
#define AURORA_FRAME_DIAG_DEFAULT_MAX_BYTES (16ULL * 1024ULL * 1024ULL)

static _Atomic int diag_enabled = -1; /* -1 unknown, 0 off, 1 on */
static pthread_mutex_t diag_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *diag_fp = NULL;
static char diag_buf[AURORA_FRAME_DIAG_BUF_SIZE];
static size_t diag_used = 0;
static uint64_t diag_last_flush_ns = 0;
static uint64_t diag_frame_number = 0;
static uint64_t diag_last_feed_wall_ns = 0;
static uint64_t diag_last_pts = 0;
static bool diag_have_last = false;
static char diag_backend[32] = "smp";
static uint64_t diag_session_number = 0;
static bool diag_file_started = false;
static uint64_t diag_bytes_total = 0;
static uint64_t diag_max_bytes = AURORA_FRAME_DIAG_DEFAULT_MAX_BYTES;
static bool diag_capped = false;

static uint64_t DiagNowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

static void DiagFlushLocked(uint64_t now_ns) {
    if (diag_fp == NULL || diag_used == 0) {
        return;
    }
    (void) fwrite(diag_buf, 1, diag_used, diag_fp);
    (void) fflush(diag_fp);
    diag_used = 0;
    diag_last_flush_ns = now_ns;
}

/*
 * Ends the log at the size cap. The last thing written is a record saying so: a reader has
 * to be able to tell "the recorder stopped here" from "the stream stopped producing
 * frames", and those two look identical in a file that just goes quiet. That closing record
 * is itself written past the limit, so the file ends a hundred-odd bytes over it -- the cap
 * bounds the log, it does not promise an exact size.
 */
static void DiagCapLocked(uint64_t now) {
    DiagFlushLocked(now);
    char line[192];
    int n = snprintf(line, sizeof(line),
                     "{\"type\":\"log_capped\",\"session\":%llu,\"frame\":%llu,\"bytes\":%llu,"
                     "\"limit_bytes\":%llu}\n",
                     (unsigned long long) diag_session_number,
                     (unsigned long long) diag_frame_number,
                     (unsigned long long) diag_bytes_total,
                     (unsigned long long) diag_max_bytes);
    if (n > 0 && (size_t) n < sizeof(line)) {
        memcpy(diag_buf, line, (size_t) n);
        diag_used = (size_t) n;
        diag_bytes_total += (uint64_t) n;
    }
    diag_capped = true;
    DiagFlushLocked(now);
    if (diag_fp != NULL) {
        fclose(diag_fp);
        diag_fp = NULL;
    }
}

static void DiagAppendLocked(const char *line, size_t len) {
    if (diag_capped) {
        return;
    }
    uint64_t now = DiagNowNs();
    if (diag_max_bytes != 0 && diag_bytes_total + len > diag_max_bytes) {
        DiagCapLocked(now);
        return;
    }
    if (diag_used + len + 1 > sizeof(diag_buf) ||
        (diag_last_flush_ns != 0 && now - diag_last_flush_ns >= AURORA_FRAME_DIAG_FLUSH_NS)) {
        DiagFlushLocked(now);
    }
    if (len + 1 > sizeof(diag_buf)) {
        return;
    }
    if (diag_used + len + 1 > sizeof(diag_buf)) {
        DiagFlushLocked(now);
    }
    memcpy(diag_buf + diag_used, line, len);
    diag_used += len;
    diag_bytes_total += (uint64_t) len;
    if (diag_last_flush_ns == 0) {
        diag_last_flush_ns = now;
    }
}

static bool DiagFlagFilePresent(void) {
    /* WebOS Dev Manager / launcher cannot pass env vars — touch this file instead. */
    FILE *fp = fopen("/tmp/aurora_frame_diag.enable", "r");
    if (fp == NULL) {
        return false;
    }
    fclose(fp);
    return true;
}

bool AuroraFrameDiagEnabled(void) {
    int v = atomic_load(&diag_enabled);
    if (v >= 0) {
        return v != 0;
    }
    const char *env = getenv("AURORA_FRAME_DIAG");
    int on = (env != NULL && env[0] == '1' && env[1] == '\0') || DiagFlagFilePresent();
    atomic_store(&diag_enabled, on ? 1 : 0);
    return on != 0;
}

void AuroraFrameDiagBeginSession(const char *backend) {
    if (!AuroraFrameDiagEnabled()) {
        return;
    }
    pthread_mutex_lock(&diag_lock);
    if (diag_fp != NULL) {
        DiagFlushLocked(DiagNowNs());
        fclose(diag_fp);
        diag_fp = NULL;
    }
    if (diag_capped) {
        /* Log already closed at the size cap; leaving diag_fp NULL keeps every later record
         * on the cheap early-return path instead of quietly reopening the file. */
        pthread_mutex_unlock(&diag_lock);
        return;
    }
    const char *path = getenv("AURORA_FRAME_DIAG_PATH");
    if (path == NULL || path[0] == '\0') {
        path = "/tmp/aurora_frame_diag.ndjson";
    }
    /*
     * Truncate on the first session of the process, append for every session after it.
     * BeginSession runs from LoadMedia, and LoadMedia runs on every mid-stream ReloadMedia
     * — aspect change, HDR re-init after a decoder-error, audio reopen — so reopening with
     * "w" here would delete the records leading up to exactly the events the log exists to
     * explain. Nor is the stream boundary safe: a decoder-error ends the stream and
     * auto-resume immediately opens a new player, so truncating there would wipe the log of
     * the session that just failed. The file therefore grows across the whole app run and
     * is bounded by the size cap instead. Sessions stay separable through the session
     * counter, which every record carries; frame numbers restart with each session.
     */
    if (!diag_file_started) {
        const char *limit = getenv("AURORA_FRAME_DIAG_MAX_BYTES");
        if (limit != NULL && limit[0] != '\0') {
            diag_max_bytes = strtoull(limit, NULL, 10);
        }
    }
    diag_fp = fopen(path, diag_file_started ? "a" : "w");
    diag_file_started = true;
    diag_session_number++;
    diag_used = 0;
    diag_last_flush_ns = 0;
    diag_frame_number = 0;
    diag_have_last = false;
    if (backend != NULL && backend[0] != '\0') {
        snprintf(diag_backend, sizeof(diag_backend), "%s", backend);
    }
    if (diag_fp != NULL) {
        char line[320];
        int n = snprintf(line, sizeof(line),
                         "{\"type\":\"session_begin\",\"backend\":\"%s\",\"session\":%llu,"
                         "\"limit_bytes\":%llu,\"path\":\"%s\"}\n",
                         diag_backend, (unsigned long long) diag_session_number,
                         (unsigned long long) diag_max_bytes, path);
        if (n > 0 && (size_t) n < sizeof(line)) {
            DiagAppendLocked(line, (size_t) n);
        }
    }
    pthread_mutex_unlock(&diag_lock);
}

void AuroraFrameDiagEndSession(void) {
    if (atomic_load(&diag_enabled) != 1) {
        return;
    }
    pthread_mutex_lock(&diag_lock);
    if (diag_fp != NULL) {
        char line[128];
        int n = snprintf(line, sizeof(line),
                         "{\"type\":\"session_end\",\"session\":%llu,\"frames\":%llu}\n",
                         (unsigned long long) diag_session_number,
                         (unsigned long long) diag_frame_number);
        if (n > 0 && (size_t) n < sizeof(line)) {
            DiagAppendLocked(line, (size_t) n);
        }
        DiagFlushLocked(DiagNowNs());
        /* Re-check: this very record can be the one that trips the size cap, and
         * the cap closes the file itself. Reading the handle from before the
         * append would hand fclose a NULL. */
        if (diag_fp != NULL) {
            fclose(diag_fp);
            diag_fp = NULL;
        }
    }
    pthread_mutex_unlock(&diag_lock);
}

uint64_t AuroraFrameDiagNowNs(void) {
    return DiagNowNs();
}

void AuroraFrameDiagLogFeed(uint64_t pts, int render_queue_length) {
    AuroraFrameDiagLogFeedAt(pts, render_queue_length, 0, 0, 0);
}

void AuroraFrameDiagLogFeedAt(uint64_t pts, int render_queue_length, uint64_t feed_wall_ns,
                              uint64_t submit_us, uint32_t bytes) {
    if (!AuroraFrameDiagEnabled()) {
        return;
    }
    uint64_t wall = feed_wall_ns != 0 ? feed_wall_ns : DiagNowNs();
    pthread_mutex_lock(&diag_lock);
    if (diag_fp == NULL) {
        pthread_mutex_unlock(&diag_lock);
        return;
    }
    diag_frame_number++;
    int64_t d_wall = 0;
    int64_t d_pts = 0;
    if (diag_have_last) {
        d_wall = (int64_t) (wall - diag_last_feed_wall_ns);
        d_pts = (int64_t) (pts - diag_last_pts);
    }
    char line[448];
    int n = snprintf(line, sizeof(line),
                     "{\"type\":\"feed\",\"backend\":\"%s\",\"session\":%llu,\"frame\":%llu,"
                     "\"feed_wallclock_ns\":%llu,\"pts\":%llu,\"render_queue_length\":%d,"
                     "\"delta_feed_wall_ns\":%lld,\"delta_pts\":%lld,"
                     "\"submit_us\":%llu,\"bytes\":%u}\n",
                     diag_backend,
                     (unsigned long long) diag_session_number,
                     (unsigned long long) diag_frame_number,
                     (unsigned long long) wall,
                     (unsigned long long) pts,
                     render_queue_length,
                     (long long) d_wall,
                     (long long) d_pts,
                     (unsigned long long) submit_us,
                     bytes);
    if (n > 0 && (size_t) n < sizeof(line)) {
        DiagAppendLocked(line, (size_t) n);
    }
    diag_last_feed_wall_ns = wall;
    diag_last_pts = pts;
    diag_have_last = true;
    pthread_mutex_unlock(&diag_lock);
}

void AuroraFrameDiagLogEvent(int event_type, int64_t num_value, const char *str_value) {
    if (!AuroraFrameDiagEnabled()) {
        return;
    }
    uint64_t wall = DiagNowNs();
    pthread_mutex_lock(&diag_lock);
    if (diag_fp == NULL) {
        pthread_mutex_unlock(&diag_lock);
        return;
    }
    char escaped[128];
    escaped[0] = '\0';
    if (str_value != NULL) {
        size_t o = 0;
        for (size_t i = 0; str_value[i] != '\0' && o + 2 < sizeof(escaped); i++) {
            char c = str_value[i];
            if (c == '"' || c == '\\') {
                escaped[o++] = '\\';
            }
            if ((unsigned char) c < 0x20) {
                continue;
            }
            escaped[o++] = c;
        }
        escaped[o] = '\0';
    }
    char line[384];
    int n = snprintf(line, sizeof(line),
                     "{\"type\":\"smp_event\",\"backend\":\"%s\",\"session\":%llu,"
                     "\"event_type\":%d,\"event_wallclock_ns\":%llu,\"numValue\":%lld,"
                     "\"strValue\":\"%s\"}\n",
                     diag_backend, (unsigned long long) diag_session_number, event_type,
                     (unsigned long long) wall,
                     (long long) num_value,
                     escaped);
    if (n > 0 && (size_t) n < sizeof(line)) {
        DiagAppendLocked(line, (size_t) n);
    }
    pthread_mutex_unlock(&diag_lock);
}
