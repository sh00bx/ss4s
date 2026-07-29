#include "ndl_common.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "opus_empty.h"

static SS4S_PlayerContext *CreatePlayerContext(SS4S_Player *player);

static void DestroyPlayerContext(SS4S_PlayerContext *context);

static void PlayerSetWaitAudioVideoReady(SS4S_PlayerContext *context, bool option);

static int OpusFeedEmpty(void *arg, const unsigned char *data, size_t size);

const SS4S_PlayerDriver SS4S_NDL_webOS5_PlayerDriver = {
    .Create = CreatePlayerContext,
    .Destroy = DestroyPlayerContext,
    .SetWaitAudioVideoReady = PlayerSetWaitAudioVideoReady,
};

static int UnloadMedia(SS4S_PlayerContext *context);

static int LoadMedia(SS4S_PlayerContext *context);

static void LoadCallback(int type, long long numValue, const char *strValue);

static SS4S_PlayerContext *ActivatePlayerContext = NULL;

int SS4S_NDL_webOS5_ReloadMedia(SS4S_PlayerContext *context) {
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Reloading media");
    if (UnloadMedia(context) != 0) {
        return -1;
    }
    return LoadMedia(context);
}

int SS4S_NDL_webOS5_UnloadMedia(SS4S_PlayerContext *context) {
    return UnloadMedia(context);
}

uint64_t SS4S_NDL_webOS5_GetPts(const SS4S_PlayerContext *context) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t pts = (now.tv_sec * 1000) + (now.tv_nsec / 1000000) - context->mediaLoadedTime.tv_sec * 1000 -
                   context->mediaLoadedTime.tv_nsec / 1000000;
    return pts;
}

void SS4S_NDL_webOS5_ConfigureSmoothPacing(SS4S_PlayerContext *context, int fpsNum, int fpsDen) {
    if (!context) {
        return;
    }
    const char *env = getenv("SS4S_SMOOTH_PACING");
    if (env == NULL || env[0] == '\0') {
        env = getenv("SS4S_NDL_SMOOTH_PACING");
    }
    /* Default ON unless explicitly disabled with "0"/"false"/"off". */
    bool enabled = true;
    if (env != NULL && env[0] != '\0') {
        if (env[0] == '0' || strcmp(env, "false") == 0 || strcmp(env, "off") == 0 ||
            strcmp(env, "FALSE") == 0 || strcmp(env, "OFF") == 0) {
            enabled = false;
        }
    }
    context->smoothPacing = enabled;
    context->smoothPtsInitialized = false;
    context->smoothLastPts = 0;
    context->hostPtsAnchored = false;
    context->hostPtsAnchorUs = 0;
    context->hostPtsPlayerAnchorMs = 0;

    double intervalMs = 1000.0 / 60.0;
    const char *intervalEnv = getenv("SS4S_SMOOTH_PACING_INTERVAL_US");
    if (intervalEnv == NULL || intervalEnv[0] == '\0') {
        intervalEnv = getenv("SS4S_NDL_PACING_INTERVAL_US");
    }
    if (intervalEnv != NULL && intervalEnv[0] != '\0') {
        long us = strtol(intervalEnv, NULL, 10);
        if (us > 1000 && us < 100000) {
            intervalMs = (double) us / 1000.0;
        }
    } else if (fpsNum > 0 && fpsDen > 0) {
        intervalMs = 1000.0 * (double) fpsDen / (double) fpsNum;
    }
    if (intervalMs < 1.0) {
        intervalMs = 1.0;
    }
    context->smoothIntervalMs = intervalMs;
    // Env-tunable max pacing drift (frames). A/B TEST BUILD: default flipped to 0.5 to match
    // upstream 2e6584d / aurora-tv v1.1.7 (tighter pacing, lower latency, more judder risk).
    // Baseline was 2.0; env SS4S_SMOOTH_PACING_MAX_DRIFT_FRAMES still overrides. Range clamp per upstream.
    double driftFrames = 0.5;
    const char *driftEnv = getenv("SS4S_SMOOTH_PACING_MAX_DRIFT_FRAMES");
    if (driftEnv != NULL && driftEnv[0] != '\0') {
        double d = strtod(driftEnv, NULL);
        if (d >= 0.15 && d <= 4.0) {
            driftFrames = d;
        }
    }
    context->smoothMaxDriftMs = intervalMs * driftFrames;

    if (enabled) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                            "Smooth pacing enabled interval=%.2fms maxDrift=%.2fms (%.2f frames)",
                            context->smoothIntervalMs, context->smoothMaxDriftMs, driftFrames);
    } else {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Smooth pacing disabled (wall-clock PTS)");
    }
}

void SS4S_NDL_webOS5_ConfigureAudioPacing(SS4S_PlayerContext *context, int sampleRate, int samplesPerFrame,
                                          int bytesPerSample) {
    if (!context) {
        return;
    }
    /* Default ON unless explicitly disabled with "0"/"false"/"off". */
    bool enabled = true;
    const char *env = getenv("SS4S_AUDIO_PACING");
    if (env != NULL && env[0] != '\0') {
        if (env[0] == '0' || strcmp(env, "false") == 0 || strcmp(env, "off") == 0 ||
            strcmp(env, "FALSE") == 0 || strcmp(env, "OFF") == 0) {
            enabled = false;
        }
    }
    context->audioPacing = enabled;
    context->audioPtsInitialized = false;
    context->audioNextPtsMs = 0;
    context->audioSampleRate = sampleRate > 0 ? (double) sampleRate : 48000.0;
    context->audioBytesPerSample = bytesPerSample > 0 ? bytesPerSample : 0;
    context->audioFrameMs = samplesPerFrame > 0 ? (double) samplesPerFrame * 1000.0 / context->audioSampleRate : 5.0;
    context->audioStatsLastLogMs = 0;
    context->audioFedPackets = 0;
    context->audioReanchors = 0;
    context->audioConcealed = 0;
    context->audioLeadMinMs = 0;
    context->audioLeadMaxMs = 0;
    context->audioMaxExcursionMs = 0;

    /*
     * Arrival jitter above this band is treated as a genuine timeline break (long stall, whole
     * FEC block lost, clock drift) and the virtual timeline snaps back to the wall clock.
     */
    double maxDriftMs = 45.0;
    const char *driftEnv = getenv("SS4S_AUDIO_PACING_MAX_DRIFT_MS");
    if (driftEnv != NULL && driftEnv[0] != '\0') {
        double d = strtod(driftEnv, NULL);
        if (d >= 5.0 && d <= 500.0) {
            maxDriftMs = d;
        }
    }
    context->audioMaxDriftMs = maxDriftMs;

    if (enabled) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                            "Audio pacing enabled frame=%.2fms bytesPerSample=%d maxDrift=%.1fms",
                            context->audioFrameMs, context->audioBytesPerSample, context->audioMaxDriftMs);
    } else {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Audio pacing disabled (wall-clock PTS)");
    }
}

uint64_t SS4S_NDL_webOS5_NextAudioPts(SS4S_PlayerContext *context, size_t size) {
    uint64_t wall = SS4S_NDL_webOS5_GetPts(context);
    if (!context->audioPacing) {
        return wall;
    }

    /* PCM carries its own duration; compressed packets fall back to the configured frame size. */
    double durationMs = context->audioFrameMs;
    if (context->audioBytesPerSample > 0 && size >= (size_t) context->audioBytesPerSample) {
        durationMs = (double) (size / (size_t) context->audioBytesPerSample) * 1000.0 / context->audioSampleRate;
    }
    if (durationMs <= 0) {
        durationMs = context->audioFrameMs;
    }

    if (!context->audioPtsInitialized) {
        context->audioNextPtsMs = (double) wall;
        context->audioPtsInitialized = true;
        context->audioStatsLastLogMs = wall;
    }

    double pts = context->audioNextPtsMs;
    double lead = pts - (double) wall;
    if (lead < -context->audioMaxDriftMs || lead > context->audioMaxDriftMs) {
        /* Sample the excursion before it is discarded, otherwise the lead min/max below
         * only ever record in-band values and the log understates what went wrong. */
        double excursion = lead < 0 ? -lead : lead;
        if (excursion > context->audioMaxExcursionMs) {
            context->audioMaxExcursionMs = excursion;
        }
        context->audioReanchors++;
        pts = (double) wall;
        lead = 0;
    }
    context->audioNextPtsMs = pts + durationMs;

    if (context->audioFedPackets == 0 || lead < context->audioLeadMinMs) {
        context->audioLeadMinMs = lead;
    }
    if (context->audioFedPackets == 0 || lead > context->audioLeadMaxMs) {
        context->audioLeadMaxMs = lead;
    }
    context->audioFedPackets++;

    if (wall - context->audioStatsLastLogMs >= 30000) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                            "Audio pacing: %u packets, jitter absorbed %.1f..%.1fms, %u re-anchors "
                            "(max excursion %.1fms), %u concealed",
                            context->audioFedPackets, context->audioLeadMinMs, context->audioLeadMaxMs,
                            context->audioReanchors, context->audioMaxExcursionMs, context->audioConcealed);
        context->audioStatsLastLogMs = wall;
        context->audioFedPackets = 0;
        context->audioReanchors = 0;
        context->audioConcealed = 0;
        context->audioMaxExcursionMs = 0;
    }

    return (uint64_t) (pts + 0.5);
}

typedef struct {
    SS4S_PlayerContext *context;
    bool fed;
} OpusConcealArg;

static int OpusFeedConcealed(void *arg, const unsigned char *data, size_t size) {
    OpusConcealArg *conceal = arg;
    /*
     * Route the silent frame through the paced clock rather than the wall clock. The virtual
     * timeline only advances on packets we actually feed, so skipping a lost packet would pull
     * every later PTS one frame earlier and let audio creep ahead of video until a re-anchor
     * snaps it back.
     */
    uint64_t pts = SS4S_NDL_webOS5_NextAudioPts(conceal->context, size);
    int rc = NDL_DirectAudioPlay((void *) data, size, (long long) pts);
    if (rc == 0) {
        conceal->fed = true;
    }
    return rc;
}

bool SS4S_NDL_webOS5_ConcealAudioFrame(SS4S_PlayerContext *context) {
    if (context->opusEmpty == NULL) {
        return false;
    }
    /* SS4S_OpusEmptyFeed returns 0 both on success and when the channel layout has no
     * pre-baked frame, so track the actual feed through the callback. */
    OpusConcealArg conceal = {.context = context, .fed = false};
    SS4S_OpusEmptyFeed(context->opusEmpty, OpusFeedConcealed, &conceal);
    if (!conceal.fed) {
        return false;
    }
    context->audioConcealed++;
    return true;
}

uint64_t SS4S_NDL_webOS5_NextVideoPts(SS4S_PlayerContext *context, int64_t hostPtsUs) {
    uint64_t wall = SS4S_NDL_webOS5_GetPts(context);
    uint64_t base = wall;
    if (context->smoothPacing && hostPtsUs >= 0) {
        if (!context->hostPtsAnchored) {
            context->hostPtsAnchorUs = hostPtsUs;
            context->hostPtsPlayerAnchorMs = (double) wall;
            context->hostPtsAnchored = true;
            SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                                "Host PTS anchored hostUs=%lld playerMs=%llu",
                                (long long) hostPtsUs, (unsigned long long) wall);
            base = wall;
        } else {
            double mapped = context->hostPtsPlayerAnchorMs +
                            (double) (hostPtsUs - context->hostPtsAnchorUs) / 1000.0;
            if (mapped < 0) {
                mapped = 0;
            }
            base = (uint64_t) (mapped + 0.5);
        }
    }
    if (!context->smoothPacing || context->smoothIntervalMs <= 0) {
        return base;
    }
    if (!context->smoothPtsInitialized) {
        context->smoothLastPts = (double) base;
        context->smoothPtsInitialized = true;
        return base;
    }
    double ideal = context->smoothLastPts + context->smoothIntervalMs;
    double minPts = (double) base - context->smoothMaxDriftMs;
    double maxPts = (double) base + context->smoothMaxDriftMs;
    double pts = ideal;
    if (pts < minPts) {
        pts = minPts;
    } else if (pts > maxPts) {
        pts = maxPts;
    }
    /* Monotonic: at least +1 ms from last video PTS. */
    if (pts < context->smoothLastPts + 1.0) {
        pts = context->smoothLastPts + 1.0;
    }
    context->smoothLastPts = pts;
    return (uint64_t) (pts + 0.5);
}

static SS4S_PlayerContext *CreatePlayerContext(SS4S_Player *player) {
    assert(ActivatePlayerContext == NULL);
    SS4S_PlayerContext *created = calloc(1, sizeof(SS4S_PlayerContext));
    created->player = player;
    ActivatePlayerContext = created;
    return created;
}

static void DestroyPlayerContext(SS4S_PlayerContext *context) {
    UnloadMedia(context);
    free(context);
    assert(context == ActivatePlayerContext);
    ActivatePlayerContext = NULL;
}

static void PlayerSetWaitAudioVideoReady(SS4S_PlayerContext *context, bool option) {
    context->waitAudioVideoReady = option;
}

static int UnloadMedia(SS4S_PlayerContext *context) {
    int ret = 0;
    if (context->mediaLoaded) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Unloading media");
        context->mediaLoaded = false;
        ret = NDL_DirectMediaUnload();
    }
    return ret;
}

static int LoadMedia(SS4S_PlayerContext *context) {
    int ret;
    if (!SS4S_NDL_webOS5_Initialized) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Initializing NDL");
        if ((ret = NDL_DirectMediaInit(getenv("APPID"), NULL)) != 0) {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelError, "NDL", "Failed to init: ret=%d, error=%s", ret,
                                NDL_DirectMediaGetError());
            return ret;
        }
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "NDL_DirectMediaInit succeeded");
        SS4S_NDL_webOS5_Initialized = true;
    }
    assert(SS4S_NDL_webOS5_Initialized);
    assert(!context->mediaLoaded);
    NDL_DIRECTMEDIA_DATA_INFO_T info = context->mediaInfo;
    if (info.video.type == 0 && info.audio.type == 0) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "LoadMedia but audio and video has no type");
        return -1;
    } else if (context->waitAudioVideoReady && (info.video.type == 0 || info.audio.type == 0)) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Defer LoadMedia because audio or video has no type");
        return 0;
    }
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "NDL_DirectMediaLoad(video=%u (%d*%d), atype=%u)", info.video.type,
                        info.video.width, info.video.height, info.audio.type);
    if ((ret = NDL_DirectMediaLoad(&info, LoadCallback)) != 0) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "NDL_DirectMediaLoad returned %d: %s", ret,
                            NDL_DirectMediaGetError());
        return ret;
    }
    if (context->mediaInfo.audio.type == NDL_AUDIO_TYPE_PCM) {
        unsigned short empty_buf[8] = {0};
        int numChannels = 2;
        if (strncmp(context->mediaInfo.audio.pcm.channelMode, "mono", 4) == 0) {
            numChannels = 1;
        } else if (strncmp(context->mediaInfo.audio.pcm.channelMode, "6-channel", 10) == 0) {
            numChannels = 6;
        }
        size_t size = numChannels * sizeof(unsigned short);
        NDL_DirectAudioPlay(empty_buf, size, (long long) SS4S_NDL_webOS5_GetPts(context));
    } else if (context->opusEmpty != NULL) {
        SS4S_OpusEmptyFeed(context->opusEmpty, OpusFeedEmpty, context);
    }

    context->mediaLoaded = true;
    clock_gettime(CLOCK_MONOTONIC, &context->mediaLoadedTime);
    context->smoothPtsInitialized = false;
    context->smoothLastPts = 0;
    context->hostPtsAnchored = false;
    context->hostPtsAnchorUs = 0;
    context->hostPtsPlayerAnchorMs = 0;
    context->lastFrameTime = 0;
    /* mediaLoadedTime is the PTS epoch, so the audio timeline restarts with it. */
    context->audioPtsInitialized = false;
    context->audioNextPtsMs = 0;
    return ret;
}

static void LoadCallback(int type, long long numValue, const char *strValue) {
    switch (type) {
        case 0x16: {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "%s STATE_UPDATE_LOADCOMPLETED: %s", __FUNCTION__, strValue);
            break;
        }
        case 0x17: {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "%s STATE_UPDATE_UNLOADCOMPLETED: %s", __FUNCTION__,
                                strValue);
            break;
        }
        case 0x1a: {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "%s STATE_UPDATE_PLAYING: %s", __FUNCTION__, strValue);
            break;
        }
        default: {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "%s type=0x%02x, numValue=0x%llx, strValue=%p", __FUNCTION__,
                                type, numValue, strValue);
            break;
        }
    }
}

static int OpusFeedEmpty(void *arg, const unsigned char *data, size_t size) {
    return NDL_DirectAudioPlay((void *) data, size, (long long) SS4S_NDL_webOS5_GetPts(arg));
}