/* See audio_wav.h's own comment -- the one real WAV decoder every
 * phi_audio.h backend shares. */
#include "audio_wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t rd_u32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16le(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Linear-interpolation resample from (src, src_frames, src_channels) at
 * src_rate to real interleaved STEREO float32 at target_rate -- upmixes
 * mono by reading the same source channel for both output channels.
 * Real, if basic (see audio_wav.h's own comment): no anti-aliasing
 * filter, just a direct lerp between neighboring source frames, correct
 * for this codebase's actual need (short SFX/music clips, not
 * professional-grade resampling). */
static float *resample_to_stereo(const int16_t *src, int src_frames, int src_channels,
                                  int src_rate, int target_rate, int *out_frames) {
    if (src_frames <= 0) { *out_frames = 0; return NULL; }
    double ratio = (double)src_rate / (double)target_rate;
    int dst_frames = (int)((double)src_frames / ratio + 0.5);
    if (dst_frames < 1) dst_frames = 1;
    float *dst = (float *)malloc((size_t)dst_frames * 2 * sizeof(float));
    if (!dst) { *out_frames = 0; return NULL; }

    for (int i = 0; i < dst_frames; i++) {
        double src_pos = (double)i * ratio;
        int i0 = (int)src_pos;
        if (i0 >= src_frames - 1) i0 = src_frames - 2 < 0 ? 0 : src_frames - 2;
        int i1 = i0 + 1;
        if (i1 >= src_frames) i1 = src_frames - 1;
        float frac = (float)(src_pos - (double)i0);

        for (int ch = 0; ch < 2; ch++) {
            int src_ch = (src_channels == 1) ? 0 : ch;
            float s0 = src[i0 * src_channels + src_ch] / 32768.0f;
            float s1 = src[i1 * src_channels + src_ch] / 32768.0f;
            dst[i * 2 + ch] = s0 + (s1 - s0) * frac;
        }
    }
    *out_frames = dst_frames;
    return dst;
}

int phi_wav_load_resampled(const char *path, int target_rate, float **out_samples, int *out_frame_count) {
    *out_samples = NULL;
    *out_frame_count = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return 0;
    }

    int have_fmt = 0, channels = 0, sample_rate = 0, bits_per_sample = 0;
    int16_t *pcm = NULL;
    int pcm_frames = 0;

    for (;;) {
        unsigned char chunk_hdr[8];
        if (fread(chunk_hdr, 1, 8, f) != 8) break;   /* real EOF -- normal end of the chunk list */
        uint32_t chunk_size = rd_u32le(chunk_hdr + 4);

        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            if (chunk_size < 16 || fread(fmt, 1, 16, f) != 16) { fclose(f); free(pcm); return 0; }
            uint16_t audio_format = rd_u16le(fmt + 0);
            channels = rd_u16le(fmt + 2);
            sample_rate = (int)rd_u32le(fmt + 4);
            bits_per_sample = rd_u16le(fmt + 14);
            /* audio_format 1 = PCM. Anything else (e.g. 3 = IEEE float,
             * 0xFFFE = extensible) is a real, honestly-unsupported case
             * -- see this file's own top comment on scope. */
            if (audio_format != 1 || bits_per_sample != 16 || (channels != 1 && channels != 2)) {
                fclose(f); free(pcm); return 0;
            }
            have_fmt = 1;
            long extra = (long)chunk_size - 16;
            if (extra > 0) fseek(f, extra, SEEK_CUR);
        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            if (!have_fmt) { fclose(f); return 0; }   /* "data" before "fmt " -- malformed, real refusal not a guess */
            pcm_frames = (int)(chunk_size / (uint32_t)(channels * 2));
            pcm = (int16_t *)malloc((size_t)pcm_frames * (size_t)channels * sizeof(int16_t));
            if (!pcm) { fclose(f); return 0; }
            size_t want = (size_t)pcm_frames * (size_t)channels;
            if (fread(pcm, sizeof(int16_t), want, f) != want) { fclose(f); free(pcm); return 0; }
            if (chunk_size & 1) fseek(f, 1, SEEK_CUR);   /* RIFF chunks are word-aligned */
        } else {
            fseek(f, (long)chunk_size + (chunk_size & 1), SEEK_CUR);
        }
    }
    fclose(f);

    if (!have_fmt || !pcm || pcm_frames <= 0) { free(pcm); return 0; }

    int out_frames;
    float *resampled = resample_to_stereo(pcm, pcm_frames, channels, sample_rate, target_rate, &out_frames);
    free(pcm);
    if (!resampled) return 0;

    *out_samples = resampled;
    *out_frame_count = out_frames;
    return 1;
}
