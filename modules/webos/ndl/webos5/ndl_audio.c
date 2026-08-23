#include <stddef.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "ndl_common.h"
#include "opus_empty.h"
#include "opus_fix.h"
#include "../../common/webos_pcm_51_remap.h"

static bool IsOpusPassthroughSupported(const OpusConfig *config);

static bool ParseOpusConfig(const unsigned char *codecData, size_t codecDataLen, OpusConfig *config);

static int SupportsPCM6Channel = 0;

/*
 * Grows the session-owned 5.1 remap scratch. Reserved in OpenAudio so the feed path only
 * ever hits the fast return; it still grows on demand because samplesPerFrame is what the
 * host announced, not a guarantee about the size of any single packet.
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

static int DriverInit(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    /*
     * webOS 7.0 introduced PCM 6-channel support as well as NDL_DirectAudioRegisterCallback.
     * We can use this to determine if the platform supports 6-channel PCM. 
     */
    SupportsPCM6Channel = dlsym(RTLD_DEFAULT, "NDL_DirectAudioRegisterCallback") != 0;
    return 0;
}

static bool GetCapabilities(SS4S_AudioCapabilities *capabilities, SS4S_AudioCodec wantedCodecs) {
    enum SS4S_AudioCodec opusCodec = SS4S_AUDIO_OPUS;
    SS4S_AudioCodec matchedCodecs = wantedCodecs & (SS4S_AUDIO_PCM_S16LE | opusCodec);
    if (matchedCodecs == 0) {
        return false;
    }
    capabilities->codecs = SS4S_AUDIO_PCM_S16LE | opusCodec;
    /*
     * Don't check for system settings to determine the number of channels, just provide 6 channels.
     * webOS should be able to down-mix that anyway.
     */
    capabilities->maxChannels = 6;
    return true;
}

static SS4S_AudioCodec GetPreferredCodecs(const SS4S_AudioInfo *info) {
    /*
     * Prefer PCM for 5.1 wherever the platform can take it (webOS 7+). NDL's Opus
     * passthrough only accepts the channel mapping {0,1,4,5,2,3}; anything else lands in
     * the opus_fix re-encode path, which decodes and re-encodes every single frame and
     * drops out audibly while doing it. On the PCM path that whole class of mismatch
     * cannot arise, at the cost of decoding client-side. Stereo already preferred PCM.
     */
    if (info->numOfChannels == 6 && SupportsPCM6Channel) {
        return SS4S_AUDIO_PCM_S16LE;
    }
    if (info->numOfChannels == 6) {
        return SS4S_AUDIO_OPUS;
    }
    return SS4S_AUDIO_PCM_S16LE;
}

static SS4S_AudioOpenResult OpenAudio(const SS4S_AudioInfo *info, SS4S_AudioInstance **instance,
                                      SS4S_PlayerContext *context) {
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "OpenAudio called");
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    SS4S_AudioOpenResult result;
    switch (info->codec) {
        case SS4S_AUDIO_PCM_S16LE: {
            const char *mode = "stereo";
            if (info->numOfChannels == 1) {
                mode = "mono";
            } else if (info->numOfChannels == 6) {
                if (SupportsPCM6Channel) {
                    mode = "6-channel";
                    context->audioRemapWarned = false;
                    if (info->samplesPerFrame > 0 &&
                        !EnsureRemapBuffer(context, (size_t) info->samplesPerFrame * 6 * sizeof(int16_t))) {
                        SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "Could not reserve the 5.1 remap buffer");
                    }
                } else {
                    SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "6-channel PCM is not supported, "
                                                                  "falling back to stereo");
                }
            }
            NDL_DIRECTAUDIO_PCM_INFO_T pcmInfo = {
                    .type = NDL_AUDIO_TYPE_PCM,
                    .format = NDL_DIRECTMEDIA_AUDIO_PCM_FORMAT_S16LE,
                    .channelMode = mode,
                    .sampleRate = NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_OF(info->sampleRate),
            };
            context->mediaInfo.audio.pcm = pcmInfo;
            SS4S_NDL_webOS5_ConfigureAudioPacing(context, info->sampleRate, info->samplesPerFrame,
                                                 (int) (info->numOfChannels * sizeof(int16_t)));
            break;
        }
        case SS4S_AUDIO_OPUS: {
            NDL_DIRECTMEDIA_AUDIO_OPUS_INFO_T opusInfo = {
                    .type = NDL_AUDIO_TYPE_OPUS,
                    .channels = info->numOfChannels,
                    .sampleRate = info->sampleRate / 1000.0,
            };
            if (info->codecData && info->codecDataLen) {
                OpusConfig opusConfig;
                if (!ParseOpusConfig(info->codecData, info->codecDataLen, &opusConfig)) {
                    result = SS4S_AUDIO_OPEN_UNSUPPORTED_CODEC;
                    goto finish;
                }
                // Feed one empty frame to the decoder to ensure it is ready
                context->opusEmpty = SS4S_OpusEmptyCreate(opusConfig.channels, opusConfig.streamCount,
                                                          opusConfig.coupledCount);
                if (!context->opusEmpty) {
                    result = SS4S_AUDIO_OPEN_ERROR;
                    goto finish;
                }
                if (opusConfig.channels == 6 && !IsOpusPassthroughSupported(&opusConfig)) {
                    SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL",
                                        "Channel config is not supported, enabling re-encoding. "
                                        "This will introduce audio latency");
                    context->opusFix = SS4S_NDLOpusFixCreate(&opusConfig);
                    if (!context->opusFix) {
                        result = SS4S_AUDIO_OPEN_ERROR;
                        goto finish;
                    }
                }
            }
            context->mediaInfo.audio.opus = opusInfo;
            /* Compressed packets carry no byte-derivable duration, so pace on samplesPerFrame. */
            SS4S_NDL_webOS5_ConfigureAudioPacing(context, info->sampleRate, info->samplesPerFrame, 0);
            break;
        }
        default: {
            result = SS4S_AUDIO_OPEN_UNSUPPORTED_CODEC;
            goto finish;
        }
    }
    if (SS4S_NDL_webOS5_ReloadMedia(context) != 0) {
        result = SS4S_AUDIO_OPEN_ERROR;
        goto finish;
    }
    *instance = (SS4S_AudioInstance *) context;
    result = SS4S_AUDIO_OPEN_OK;

    finish:
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "OpenAudio returned %d", result);
    return result;
}

static SS4S_AudioFeedResult FeedAudio(SS4S_AudioInstance *instance, const unsigned char *data, size_t size) {
    SS4S_PlayerContext *context = (void *) instance;
    /*
     * Cheap early-out. This read can go stale, so it is not the authoritative check -- that
     * one happens again under the lock, just before the frame reaches NDL. It only spares us
     * a transcode for a player that is already gone.
     */
    if (!context->mediaLoaded) {
        return SS4S_AUDIO_FEED_NOT_READY;
    }
    if (data == NULL || size == 0) {
        /*
         * Lost-packet placeholder. On the Opus passthrough path NDL owns the decoder, so
         * libopus concealment is out of reach and feeding nothing leaves a hole in the
         * timeline. Substitute a silent frame instead. This one does touch NDL, so it keeps
         * the lock.
         */
        pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
        if (!context->mediaLoaded) {
            pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
            return SS4S_AUDIO_FEED_NOT_READY;
        }
        bool concealed = SS4S_NDL_webOS5_ConcealAudioFrame(context);
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        return concealed ? SS4S_AUDIO_FEED_OK : SS4S_AUDIO_FEED_NOT_READY;
    }
    /*
     * Everything from here to the lock is this instance's own work on this instance's own
     * buffers: the Opus re-encode, or the 5.1 channel remap. It used to run with
     * SS4S_NDL_webOS5_Lock held, which blocked the video feed for the whole duration of an
     * encode -- a 200 Hz re-encode on a SoC already saturated by 4K receive is not a short
     * hold. Limelight joins the audio threads before the player is closed, so the context and
     * its buffers stay alive across this window without the lock.
     */
    if (context->opusFix) {
        int fixedSize = SS4S_NDLOpusFixProcess(context->opusFix, data, size);
        if (fixedSize < 0) {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "SS4S_NDLOpusFixProcess returned %d", fixedSize);
            return SS4S_AUDIO_FEED_ERROR;
        }
        data = SS4S_NDLOpusFixGetBuffer(context->opusFix);
        size = fixedSize;
    } else if (context->mediaInfo.audio.type == NDL_AUDIO_TYPE_PCM &&
               context->mediaInfo.audio.pcm.channelMode != NULL &&
               strncmp(context->mediaInfo.audio.pcm.channelMode, "6-channel", 10) == 0 &&
               size >= 6 * sizeof(int16_t) && (size % (6 * sizeof(int16_t))) == 0) {
        /* Decoder emits SDL/Vorbis order; the 6-channel PCM sink wants the
         * device order in webos_pcm_51_remap.h (validated on-device upstream,
         * issue #60). Only ever active without surroundParams — sending those
         * too would remap twice. */
        if (!EnsureRemapBuffer(context, size)) {
            /* Playing the frame unremapped would put centre and surround on the wrong
             * speakers for exactly this frame and leave no trace of it. Reject it instead;
             * the session counts and logs rejected audio feeds. */
            if (!context->audioRemapWarned) {
                context->audioRemapWarned = true;
                SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "No 5.1 remap buffer for %u bytes, dropping frames",
                                    (unsigned) size);
            }
            return SS4S_AUDIO_FEED_ERROR;
        }
        int frames = (int) (size / (6 * sizeof(int16_t)));
        SS4S_WebOS_RemapPcm51ToDevice((const int16_t *) data, context->audioRemapBuffer, frames);
        data = (const unsigned char *) context->audioRemapBuffer;
    }
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    /* Authoritative check: the player may have been unloaded while we were transcoding. */
    if (!context->mediaLoaded) {
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        return SS4S_AUDIO_FEED_NOT_READY;
    }
    uint64_t pts = SS4S_NDL_webOS5_NextAudioPts(context, size);
    int rc = NDL_DirectAudioPlay((void *) data, size, (long long) pts);
    if (rc != 0) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "NDL_DirectAudioPlay returned %d: %s", rc,
                            NDL_DirectMediaGetError());
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        return SS4S_AUDIO_FEED_ERROR;
    }
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
    return SS4S_AUDIO_FEED_OK;
}

static void CloseAudio(SS4S_AudioInstance *instance) {
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "CloseAudio called");
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    SS4S_PlayerContext *context = (void *) instance;
    context->mediaInfo.audio.type = 0;
    if (context->opusFix) {
        SS4S_NDLOpusFixDestroy(context->opusFix);
        context->opusFix = NULL;
    }
    if (context->opusEmpty) {
        SS4S_OpusEmptyDestroy(context->opusEmpty);
        context->opusEmpty = NULL;
    }
    free(context->audioRemapBuffer);
    context->audioRemapBuffer = NULL;
    context->audioRemapCapacity = 0;
    SS4S_NDL_webOS5_UnloadMedia(context);
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
}


bool IsOpusPassthroughSupported(const OpusConfig *config) {
    static const uint8_t wantedMapping[6] = {0, 1, 4, 5, 2, 3};
    return config->streamCount == 4 && config->coupledCount == 2 && memcmp(config->mapping, wantedMapping, 6) == 0;
}

bool ParseOpusConfig(const unsigned char *codecData, size_t codecDataLen, OpusConfig *config) {
    if (codecDataLen < 20 || memcmp(codecData, "OpusHead", 8) != 0) {
        return false;
    }
    memcpy(&config->sampleRate, codecData + 12, 4);
    config->channels = codecData[9];
    config->streamCount = codecData[19];
    config->coupledCount = codecData[20];
    if (config->channels > sizeof(config->mapping)) {
        // We can only represent up to sizeof(mapping) channels (5.1 layout).
        // Anything wider (e.g. 7.1 surround = 8 channels) would overflow the
        // fixed-size mapping array on the memcpy below.
        return false;
    }
    if ((int) codecDataLen >= 21 + config->channels) {
        memcpy(config->mapping, codecData + 21, config->channels);
    } else {
        config->mapping[0] = 0;
        config->mapping[1] = 0;
    }
    return true;
}

const SS4S_AudioDriver SS4S_NDL_webOS5_AudioDriver = {
        .Base = {
                .Init = DriverInit,
                .PostInit = SS4S_NDL_webOS5_Driver_PostInit,
                .Quit = SS4S_NDL_webOS5_Driver_Quit,
        },
        .GetCapabilities = GetCapabilities,
        .GetPreferredCodecs = GetPreferredCodecs,
        .Open = OpenAudio,
        .Feed = FeedAudio,
        .Close = CloseAudio,
};