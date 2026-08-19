/* Windows backend for phi_audio.h -- a real, honest stub, matching
 * input_gamepad_win32_stub.c's own precedent exactly (see that file's
 * top comment for the full reasoning): not attempted blind against an
 * unverified build environment this pass. A real WASAPI or winmm
 * backend is the natural next step for whoever next has a real Windows
 * environment to verify sound against.
 *
 * WAV loading itself still works for real (audio_wav.c has no
 * platform-specific dependency at all) -- only actual playback is
 * stubbed, so game code that checks phi_audio_load_sound's return value
 * still exercises its own real asset-pipeline error handling on this
 * target, it just never hears anything. Every phi_audio_play/stop call
 * still does something real (bounds-appropriate, non-crashing), it just
 * never has a device to report -- same "real, valid, non-crashing
 * answer, not a build failure or silent wrong answer" contract input_
 * gamepad_win32_stub.c already establishes. */
#include "phi_audio.h"
#include "audio_wav.h"
#include <stdio.h>
#include <stdlib.h>

struct PhiSound {
    float *samples;
    int    frame_count;
};

static int s_warned = 0;

void phi_audio_init(void) {
    if (!s_warned) {
        printf("[audio] win32: not implemented yet (no verified build environment for it this pass) -- "
               "sounds still load and decode for real, but nothing will actually play\n");
        s_warned = 1;
    }
}

void phi_audio_shutdown(void) { }

PhiSound *phi_audio_load_sound(const char *path) {
    float *samples; int frame_count;
    if (!phi_wav_load_resampled(path, 44100, &samples, &frame_count)) {
        printf("[audio] failed to load '%s' (missing file, not a WAV, or an unsupported PCM layout -- see audio_wav.h's scope note)\n", path);
        return NULL;
    }
    PhiSound *s = (PhiSound *)malloc(sizeof(PhiSound));
    s->samples = samples;
    s->frame_count = frame_count;
    return s;
}

PhiAudioVoice phi_audio_play(PhiSound *sound, float volume, int loop) {
    (void)sound; (void)volume; (void)loop;
    return -1;
}

PhiAudioVoice phi_audio_play_3d(PhiSound *sound, Vec3f position, float volume, int loop) {
    (void)sound; (void)position; (void)volume; (void)loop;
    return -1;
}

void phi_audio_stop(PhiAudioVoice voice) { (void)voice; }

void phi_audio_set_listener(Vec3f position, Vec3f forward, Vec3f right) {
    (void)position; (void)forward; (void)right;
}
