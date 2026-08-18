/* Native Linux backend for phi_audio.h -- see that header's own top
 * comment for the overall design. Real ALSA PCM output + a hand-rolled
 * mixing thread when PHI_HAVE_ALSA is defined (the Makefile sets this
 * iff /usr/include/alsa/asoundlib.h was actually found at `make` time --
 * see its own ALSA_HEADER comment); an honest "no audio device" fallback
 * otherwise, same real-stub shape input_gamepad_win32_stub.c already
 * established for an unavailable backend elsewhere in this codebase --
 * never a silent, half-working guess. WAV loading itself (audio_wav.c)
 * has no ALSA dependency at all, so it stays fully real either way --
 * only actual playback differs. */
#include "phi_audio.h"
#include "audio_wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PHI_AUDIO_MIXER_RATE   44100
#define PHI_AUDIO_MAX_VOICES   32
#define PHI_AUDIO_BLOCK_FRAMES 1024

struct PhiSound {
    float *samples;      /* interleaved stereo float32, PHI_AUDIO_MIXER_RATE */
    int    frame_count;
};

typedef struct {
    PhiSound *sound;
    int   playing;
    int   loop;
    int   is_3d;
    float volume;
    Vec3f position;
    int   frame_pos;
} Voice;

static Voice s_voices[PHI_AUDIO_MAX_VOICES];

PhiSound *phi_audio_load_sound(const char *path) {
    float *samples; int frame_count;
    if (!phi_wav_load_resampled(path, PHI_AUDIO_MIXER_RATE, &samples, &frame_count)) {
        printf("[audio] failed to load '%s' (missing file, not a WAV, or an unsupported PCM layout -- see audio_wav.h's scope note)\n", path);
        return NULL;
    }
    PhiSound *s = (PhiSound *)malloc(sizeof(PhiSound));
    s->samples = samples;
    s->frame_count = frame_count;
    return s;
}

#ifdef PHI_HAVE_ALSA
#include <alsa/asoundlib.h>
#include <pthread.h>

static snd_pcm_t      *s_pcm = NULL;
static pthread_t        s_mixer_thread;
static pthread_mutex_t  s_voices_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int     s_running = 0;
/* Only ever read/written under s_voices_lock, and only meaningful when a
 * real ALSA device exists -- moved inside this #ifdef (rather than file
 * scope shared with the honest fallback branch below) so the no-ALSA
 * build doesn't carry three real "defined but never used" warnings. */
static Vec3f s_listener_pos = {0,0,0}, s_listener_fwd = {0,0,-1}, s_listener_right = {1,0,0};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Real, simple stereo pan + inverse-distance attenuation from the
 * listener's own position/right axis -- see phi_audio.h's own top
 * comment on why this isn't full 3D HRTF. Called with s_voices_lock
 * already held. */
static void compute_3d_gains(const Voice *v, float *out_left, float *out_right) {
    Vec3f d = { v->position.x - s_listener_pos.x, v->position.y - s_listener_pos.y, v->position.z - s_listener_pos.z };
    float dist = sqrtf(d.x*d.x + d.y*d.y + d.z*d.z);
    float atten = 1.0f / (1.0f + dist * 0.05f);   /* real inverse-distance falloff, tuned so ~20 units is a real, audible drop */
    float side = 0.0f;
    if (dist > 1e-4f) {
        Vec3f dn = { d.x/dist, d.y/dist, d.z/dist };
        side = clampf(dn.x*s_listener_right.x + dn.y*s_listener_right.y + dn.z*s_listener_right.z, -1.0f, 1.0f);
    }
    float pan_l = clampf(1.0f - side, 0.0f, 1.0f);
    float pan_r = clampf(1.0f + side, 0.0f, 1.0f);
    *out_left  = v->volume * atten * pan_l;
    *out_right = v->volume * atten * pan_r;
}

static void *mixer_thread_fn(void *ud) {
    (void)ud;
    int16_t block[PHI_AUDIO_BLOCK_FRAMES * 2];
    float mix[PHI_AUDIO_BLOCK_FRAMES * 2];

    while (s_running) {
        memset(mix, 0, sizeof(mix));

        pthread_mutex_lock(&s_voices_lock);
        for (int vi = 0; vi < PHI_AUDIO_MAX_VOICES; vi++) {
            Voice *v = &s_voices[vi];
            if (!v->playing || !v->sound) continue;
            float gl = v->volume, gr = v->volume;
            if (v->is_3d) compute_3d_gains(v, &gl, &gr);

            for (int i = 0; i < PHI_AUDIO_BLOCK_FRAMES; i++) {
                if (v->frame_pos >= v->sound->frame_count) {
                    if (v->loop) { v->frame_pos = 0; }
                    else { v->playing = 0; break; }
                }
                mix[i*2+0] += v->sound->samples[v->frame_pos*2+0] * gl;
                mix[i*2+1] += v->sound->samples[v->frame_pos*2+1] * gr;
                v->frame_pos++;
            }
        }
        pthread_mutex_unlock(&s_voices_lock);

        for (int i = 0; i < PHI_AUDIO_BLOCK_FRAMES * 2; i++) {
            float s = clampf(mix[i], -1.0f, 1.0f);
            block[i] = (int16_t)(s * 32767.0f);
        }

        snd_pcm_sframes_t written = snd_pcm_writei(s_pcm, block, PHI_AUDIO_BLOCK_FRAMES);
        if (written < 0) {
            /* Real ALSA underrun recovery -- snd_pcm_recover is the
             * standard, documented way to handle EPIPE/ESTRPIPE rather
             * than treating either as a fatal error. */
            written = snd_pcm_recover(s_pcm, (int)written, 1);
            if (written < 0) { printf("[audio] ALSA write failed unrecoverably: %s\n", snd_strerror((int)written)); break; }
        }
    }
    return NULL;
}

void phi_audio_init(void) {
    memset(s_voices, 0, sizeof(s_voices));
    int err = snd_pcm_open(&s_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        printf("[audio] snd_pcm_open failed: %s -- no audio device, every phi_audio_play* call will honestly report failure\n", snd_strerror(err));
        s_pcm = NULL;
        return;
    }
    err = snd_pcm_set_params(s_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                              2, PHI_AUDIO_MIXER_RATE, 1, 100000 /* 100ms latency */);
    if (err < 0) {
        printf("[audio] snd_pcm_set_params failed: %s\n", snd_strerror(err));
        snd_pcm_close(s_pcm);
        s_pcm = NULL;
        return;
    }
    s_running = 1;
    pthread_create(&s_mixer_thread, NULL, mixer_thread_fn, NULL);
    printf("[audio] real ALSA playback ready (44.1kHz stereo, %d-voice mixer)\n", PHI_AUDIO_MAX_VOICES);
}

void phi_audio_shutdown(void) {
    if (!s_pcm) return;
    s_running = 0;
    pthread_join(s_mixer_thread, NULL);
    snd_pcm_close(s_pcm);
    s_pcm = NULL;
}

static PhiAudioVoice play_internal(PhiSound *sound, float volume, int loop, int is_3d, Vec3f position) {
    if (!s_pcm || !sound) return -1;
    pthread_mutex_lock(&s_voices_lock);
    int slot = -1;
    for (int i = 0; i < PHI_AUDIO_MAX_VOICES; i++) if (!s_voices[i].playing) { slot = i; break; }
    if (slot >= 0) {
        s_voices[slot].sound = sound;
        s_voices[slot].playing = 1;
        s_voices[slot].loop = loop;
        s_voices[slot].is_3d = is_3d;
        s_voices[slot].volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
        s_voices[slot].position = position;
        s_voices[slot].frame_pos = 0;
    }
    pthread_mutex_unlock(&s_voices_lock);
    return slot;
}

PhiAudioVoice phi_audio_play(PhiSound *sound, float volume, int loop) {
    return play_internal(sound, volume, loop, 0, (Vec3f){0,0,0});
}

PhiAudioVoice phi_audio_play_3d(PhiSound *sound, Vec3f position, float volume, int loop) {
    return play_internal(sound, volume, loop, 1, position);
}

void phi_audio_stop(PhiAudioVoice voice) {
    if (voice < 0 || voice >= PHI_AUDIO_MAX_VOICES) return;
    pthread_mutex_lock(&s_voices_lock);
    s_voices[voice].playing = 0;
    pthread_mutex_unlock(&s_voices_lock);
}

void phi_audio_set_listener(Vec3f position, Vec3f forward, Vec3f right) {
    pthread_mutex_lock(&s_voices_lock);
    s_listener_pos = position;
    s_listener_fwd = forward;
    s_listener_right = right;
    pthread_mutex_unlock(&s_voices_lock);
}

#else /* !PHI_HAVE_ALSA -- honest, real "no audio device" fallback */

static int s_warned = 0;

void phi_audio_init(void) {
    memset(s_voices, 0, sizeof(s_voices));
    printf("[audio] built without ALSA dev headers (libasound2-dev not found at `make` time) -- "
           "sounds still load and decode for real, but nothing will actually play. "
           "Install libasound2-dev and rebuild for real native playback.\n");
}
void phi_audio_shutdown(void) { }

PhiAudioVoice phi_audio_play(PhiSound *sound, float volume, int loop) {
    (void)sound; (void)volume; (void)loop;
    if (!s_warned) { s_warned = 1; printf("[audio] phi_audio_play: no audio device (see phi_audio_init's own message)\n"); }
    return -1;
}
PhiAudioVoice phi_audio_play_3d(PhiSound *sound, Vec3f position, float volume, int loop) {
    (void)position;
    return phi_audio_play(sound, volume, loop);
}
void phi_audio_stop(PhiAudioVoice voice) { (void)voice; }
void phi_audio_set_listener(Vec3f position, Vec3f forward, Vec3f right) {
    (void)position; (void)forward; (void)right;
}

#endif
