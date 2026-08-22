#include <assert.h>
#include <stdlib.h>
#include "StarfishMediaAPIs_C.h"
#include "ss4s/modapi.h"
#include "smp_player.h"
#include "../../common/webos_pcm_51_remap.h"

/*
 * Grows the session-owned 5.1 remap scratch. Reserved in AudioOpen so the feed path only
 * ever hits the fast return; it still grows on demand because samplesPerFrame is what the
 * host announced, not a guarantee about the size of any single packet. Callers hold the
 * player lock: the realloc can move the buffer that a feed is about to read from, and
 * AudioClose frees it.
 */
static bool EnsureRemapBuffer(SS4S_PlayerContext *context, size_t size) {
    if (context->audioRemapCapacity >= size) {
        return true;
    }
    int16_t *grown = realloc(context->audioRemapBuffer, size);
    if (grown == NULL) {
        return false;
    }
    context->audioRemapBuffer = grown;
    context->audioRemapCapacity = size;
    return true;
}

const char *StarfishAudioCodecName(SS4S_AudioCodec codec) {
    switch (codec) {
        case SS4S_AUDIO_PCM_S16LE:
            return "PCM";
        case SS4S_AUDIO_OPUS:
            return "OPUS";
        case SS4S_AUDIO_AAC:
            return "AAC";
        case SS4S_AUDIO_AC3:
            return "AC3";
        default:
            return NULL;
    }
}

static bool GetAudioCapabilities(SS4S_AudioCapabilities *capabilities, SS4S_AudioCodec wantedCodecs) {
    (void) wantedCodecs;
    capabilities->codecs = SS4S_AUDIO_PCM_S16LE | SS4S_AUDIO_OPUS | SS4S_AUDIO_AAC | SS4S_AUDIO_AC3;
    capabilities->maxChannels = 6;
    return true;
}

static SS4S_AudioCodec GetPreferredCodecs(const SS4S_AudioInfo *info) {
    (void) info;
    return SS4S_AUDIO_PCM_S16LE;
}

static SS4S_AudioOpenResult AudioOpen(const SS4S_AudioInfo *info, SS4S_AudioInstance **instance,
                                      SS4S_PlayerContext *context) {
    if (context == NULL) {
        return SS4S_AUDIO_OPEN_ERROR;
    }
    StarfishPlayerLock(context);
    context->audioInfo = *info;
    context->hasAudio = true;
    *instance = (void *) context;
    if (info->codec == SS4S_AUDIO_PCM_S16LE && info->numOfChannels == 6) {
        context->audioRemapWarned = false;
        if (info->samplesPerFrame > 0 &&
            !EnsureRemapBuffer(context, (size_t) info->samplesPerFrame * 6 * sizeof(int16_t))) {
            StarfishLibContext->Log(SS4S_LogLevelWarn, "SMP", "Could not reserve the 5.1 remap buffer");
        }
    }
    if (!context->hasVideo && context->waitAudioVideoReady) {
        StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP", "AudioOpen: defer loading until video is ready");
        StarfishPlayerUnlock(context);
        return SS4S_AUDIO_OPEN_OK;
    }
    if (context->state != SMP_STATE_UNLOADED) {
        StarfishPlayerUnloadInner(context);
    }
    if (StarfishPlayerLoadInner(context)) {
        StarfishPlayerUnlock(context);
        return SS4S_AUDIO_OPEN_OK;
    }
    StarfishPlayerUnlock(context);
    return SS4S_AUDIO_OPEN_ERROR;
}

static void AudioClose(SS4S_AudioInstance *instance) {
    assert(instance != NULL);
    SS4S_PlayerContext *context = (SS4S_PlayerContext *) instance;
    StarfishPlayerLock(context);
    StarfishPlayerUnloadInner(context);
    context->hasAudio = false;
    free(context->audioRemapBuffer);
    context->audioRemapBuffer = NULL;
    context->audioRemapCapacity = 0;
    StarfishPlayerUnlock(context);
}

static SS4S_AudioFeedResult AudioFeed(SS4S_AudioInstance *instance, const unsigned char *data, size_t size) {
    SS4S_PlayerContext *context = (SS4S_PlayerContext *) instance;
    if (data == NULL || size == 0) {
        /* Lost-packet placeholder; this backend has no concealment frame to substitute. */
        return SS4S_AUDIO_FEED_NOT_READY;
    }
    /* The remap writes into context state that AudioOpen and AudioClose also own, and the
     * feed then hands that very buffer to the decoder, so both have to happen inside one
     * critical section — hence the locked feed variant rather than StarfishPlayerFeed. */
    StarfishPlayerLock(context);
    if (context->audioInfo.codec == SS4S_AUDIO_PCM_S16LE && context->audioInfo.numOfChannels == 6 &&
        size >= 6 * sizeof(int16_t) && (size % (6 * sizeof(int16_t))) == 0) {
        /* Same feed-side remap the NDL backend does: the decoder emits SDL/Vorbis order,
         * the 6-channel PCM sink wants the device order in webos_pcm_51_remap.h.
         *
         * Nothing reaches this branch in the shipped build. smp-webos is registered
         * FOR_VIDEO only, so ss4s_modules.ini gives it no `audio = true`, module selection
         * only ever picks an audio module with has_audio, and audio keeps going through
         * ndl-webos5 even when the decoder is forced to SMP — StarfishAudioDriver.Feed
         * never runs. The remap is here so the trap cannot bite the day SMP audio is
         * enabled: Starfish would get the same decoded buffer as NDL and put centre and
         * surround on the wrong speakers. Two things stay unestablished until someone does
         * enable it — nobody has measured this path, and the device channel order in
         * webos_pcm_51_remap.h was verified against the NDL 6-channel PCM sink, not against
         * the Starfish appsrc one.
         *
         * Only ever correct without surroundParams — sending those too would remap twice. */
        if (!EnsureRemapBuffer(context, size)) {
            /* Playing the frame unremapped would misplace channels without leaving a
             * trace. Reject it instead; the session counts and logs rejected audio feeds. */
            if (!context->audioRemapWarned) {
                context->audioRemapWarned = true;
                StarfishLibContext->Log(SS4S_LogLevelWarn, "SMP", "No 5.1 remap buffer for %u bytes, dropping frames",
                                        (unsigned) size);
            }
            StarfishPlayerUnlock(context);
            return SS4S_AUDIO_FEED_ERROR;
        }
        SS4S_WebOS_RemapPcm51ToDevice((const int16_t *) data, context->audioRemapBuffer,
                                      (int) (size / (6 * sizeof(int16_t))));
        data = (const unsigned char *) context->audioRemapBuffer;
    }
    FeedResult fed = StarfishPlayerFeedLocked(context, data, size, 2);
    StarfishPlayerUnlock(context);
    switch (fed) {
        case SMP_FEED_OK:
            return SS4S_AUDIO_FEED_OK;
        case SMP_FEED_NOT_READY:
            return SS4S_AUDIO_FEED_NOT_READY;
        case SMP_FEED_BUFFER_FULL:
            return SS4S_AUDIO_FEED_OVERFLOW;
        default:
            return SS4S_AUDIO_FEED_ERROR;
    }
}

const SS4S_AudioDriver StarfishAudioDriver = {
        .GetCapabilities = GetAudioCapabilities,
        .GetPreferredCodecs = GetPreferredCodecs,
        .Open = AudioOpen,
        .Feed = AudioFeed,
        .Close = AudioClose,
};