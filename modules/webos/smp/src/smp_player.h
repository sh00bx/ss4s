#pragma once

#include "ss4s/modapi.h"
#include <pthread.h>
#include <stdint.h>

typedef enum PlayerState {
    SMP_STATE_UNLOADED,
    SMP_STATE_LOADED,
    SMP_STATE_PLAYING,
} PlayerState;

typedef enum FeedResult {
    SMP_FEED_OK,
    SMP_FEED_NOT_READY,
    SMP_FEED_BUFFER_FULL,
    SMP_FEED_ERROR = -1,
} FeedResult;

struct SS4S_PlayerContext {
    char *appId;
    pthread_mutex_t lock;
    SS4S_Player *player;

    SS4S_AudioInfo audioInfo;
    bool hasAudio;
    SS4S_VideoInfo videoInfo;
    bool hasVideo;

    PlayerState state;
    uint64_t openTime;
    int aspectRatio;
    bool hdr, shouldStop;

    bool waitAudioVideoReady;

    /* Smooth presentation pacing (virtual PTS grid, nanoseconds). */
    bool smoothPacing;
    bool smoothPtsInitialized;
    double smoothIntervalNs;
    double smoothMaxDriftNs;
    double smoothLastPts;

    /* Host presentationTimeUs → player PTS mapping. */
    bool hostPtsAnchored;
    int64_t hostPtsAnchorUs;
    double hostPtsPlayerAnchorNs;

    /* Scratch for the 5.1 PCM channel remap in smp_audio.c, owned for the whole audio
     * session so the feed path never has to allocate. Guarded by `lock` on every path that
     * touches it — open, feed and close — because the feed hands the buffer itself to the
     * decoder and must not have it resized or freed underneath. */
    int16_t *audioRemapBuffer;
    size_t audioRemapCapacity;
    bool audioRemapWarned;

    struct StarfishMediaAPIs_C *api;
    struct StarfishResource *res;

};
extern const SS4S_LibraryContext *StarfishLibContext;

typedef struct StarfishPlayer StarfishPlayer;

bool StarfishPlayerLoadInner(SS4S_PlayerContext *ctx);

bool StarfishPlayerUnloadInner(SS4S_PlayerContext *ctx);

FeedResult StarfishPlayerFeed(SS4S_PlayerContext *ctx, const unsigned char *data, size_t size, int esData);

/**
 * Same as StarfishPlayerFeed for esData != 1, but the caller holds the player lock. A caller
 * that hands over a buffer living in the player context needs the lock to span filling that
 * buffer and the feed that reads it, which it cannot do through the locking variant.
 */
FeedResult StarfishPlayerFeedLocked(SS4S_PlayerContext *ctx, const unsigned char *data, size_t size, int esData);

FeedResult StarfishPlayerFeedVideo(SS4S_PlayerContext *ctx, const unsigned char *data, size_t size, int64_t hostPtsUs);

uint64_t StarfishPlayerGetTime();

void StarfishPlayerConfigureSmoothPacing(SS4S_PlayerContext *ctx, int fpsNum, int fpsDen);

uint64_t StarfishPlayerNextVideoPts(SS4S_PlayerContext *ctx, int64_t hostPtsUs);

void StarfishPlayerLock(SS4S_PlayerContext *ctx);

void StarfishPlayerUnlock(SS4S_PlayerContext *ctx);
