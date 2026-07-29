#pragma once

#include <pthread.h>
#include <stdint.h>
#include <NDL_directmedia_v2.h>

#include "ss4s/modapi.h"
#include "ndl_logging.h"

extern bool SS4S_NDL_webOS5_Initialized;
extern pthread_mutex_t SS4S_NDL_webOS5_Lock;
extern const SS4S_LibraryContext *SS4S_NDL_webOS5_Lib;

struct SS4S_PlayerContext {
    SS4S_Player *player;
    uint64_t lastFrameTime;
    NDL_DIRECTMEDIA_DATA_INFO_T mediaInfo;
    struct SS4S_OpusEmpty *opusEmpty;
    struct SS4S_NDLOpusFix *opusFix;
    bool mediaLoaded;
    struct timespec mediaLoadedTime;
    bool waitAudioVideoReady;
    int aspectRatio;
    bool hasHdrInfo;
    /* Smooth presentation pacing (virtual PTS grid). */
    bool smoothPacing;
    bool smoothPtsInitialized;
    double smoothIntervalMs;
    double smoothMaxDriftMs;
    double smoothLastPts;
    /* Host presentationTimeUs → player PTS mapping (ms). */
    bool hostPtsAnchored;
    int64_t hostPtsAnchorUs;
    double hostPtsPlayerAnchorMs;
    /* Sample-clock audio pacing (virtual audio timeline). */
    bool audioPacing;
    bool audioPtsInitialized;
    double audioNextPtsMs;
    double audioMaxDriftMs;
    double audioSampleRate;
    double audioFrameMs;
    int audioBytesPerSample;
    /* Audio pacing telemetry, logged periodically. */
    uint64_t audioStatsLastLogMs;
    uint32_t audioFedPackets;
    uint32_t audioReanchors;
    uint32_t audioConcealed;
    double audioLeadMinMs;
    double audioLeadMaxMs;
    /* Largest out-of-band lead seen since the last log, sampled before the re-anchor
     * resets it. Without this the lead min/max only ever show in-band values, so the
     * excursions that actually trigger re-anchors are invisible. */
    double audioMaxExcursionMs;
};

extern const SS4S_PlayerDriver SS4S_NDL_webOS5_PlayerDriver;
extern const SS4S_AudioDriver SS4S_NDL_webOS5_AudioDriver;
extern const SS4S_VideoDriver SS4S_NDL_webOS5_VideoDriver;

int SS4S_NDL_webOS5_ReloadMedia(SS4S_PlayerContext *context);

int SS4S_NDL_webOS5_UnloadMedia(SS4S_PlayerContext *context);

uint64_t SS4S_NDL_webOS5_GetPts(const SS4S_PlayerContext *context);

/** Wall-clock / host-mapped PTS, optionally smoothed on a virtual grid. */
uint64_t SS4S_NDL_webOS5_NextVideoPts(SS4S_PlayerContext *context, int64_t hostPtsUs);

void SS4S_NDL_webOS5_ConfigureSmoothPacing(SS4S_PlayerContext *context, int fpsNum, int fpsDen);

/** Configure the virtual audio timeline. bytesPerSample is 0 for compressed codecs. */
void SS4S_NDL_webOS5_ConfigureAudioPacing(SS4S_PlayerContext *context, int sampleRate, int samplesPerFrame,
                                          int bytesPerSample);

/** Audio PTS advanced by the fed sample count instead of packet arrival time. */
uint64_t SS4S_NDL_webOS5_NextAudioPts(SS4S_PlayerContext *context, size_t size);

/* Substitutes one silent Opus frame for a packet the network lost. Returns true if a
 * frame was fed. */
bool SS4S_NDL_webOS5_ConcealAudioFrame(SS4S_PlayerContext *context);

int SS4S_NDL_webOS5_Driver_PostInit(int argc, char *argv[]);

void SS4S_NDL_webOS5_Driver_Quit();
