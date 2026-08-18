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

static void DiagAppendLocked(const char *line, size_t len) {
    uint64_t now = DiagNowNs();
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
    const char *path = getenv("AURORA_FRAME_DIAG_PATH");
    if (path == NULL || path[0] == '\0') {
        path = "/tmp/aurora_frame_diag.ndjson";
    }
    diag_fp = fopen(path, "w");
    diag_used = 0;
    diag_last_flush_ns = 0;
    diag_frame_number = 0;
    diag_have_last = false;
    if (backend != NULL && backend[0] != '\0') {
        snprintf(diag_backend, sizeof(diag_backend), "%s", backend);
    }
    if (diag_fp != NULL) {
        char line[256];
        int n = snprintf(line, sizeof(line),
                         "{\"type\":\"session_begin\",\"backend\":\"%s\",\"path\":\"%s\"}\n",
                         diag_backend, path);
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
                         "{\"type\":\"session_end\",\"frames\":%llu}\n",
                         (unsigned long long) diag_frame_number);
        if (n > 0 && (size_t) n < sizeof(line)) {
            DiagAppendLocked(line, (size_t) n);
        }
        DiagFlushLocked(DiagNowNs());
        fclose(diag_fp);
        diag_fp = NULL;
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
                     "{\"type\":\"feed\",\"backend\":\"%s\",\"frame\":%llu,"
                     "\"feed_wallclock_ns\":%llu,\"pts\":%llu,\"render_queue_length\":%d,"
                     "\"delta_feed_wall_ns\":%lld,\"delta_pts\":%lld,"
                     "\"submit_us\":%llu,\"bytes\":%u}\n",
                     diag_backend,
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
                     "{\"type\":\"smp_event\",\"backend\":\"%s\",\"event_type\":%d,"
                     "\"event_wallclock_ns\":%llu,\"numValue\":%lld,\"strValue\":\"%s\"}\n",
                     diag_backend, event_type,
                     (unsigned long long) wall,
                     (long long) num_value,
                     escaped);
    if (n > 0 && (size_t) n < sizeof(line)) {
        DiagAppendLocked(line, (size_t) n);
    }
    pthread_mutex_unlock(&diag_lock);
}
