/* wasm/browser backend for phi_audio.h -- the real, standard Web Audio
 * API via EM_JS, same pattern input_gamepad_wasm.c already established
 * for the browser Gamepad API (no SDL involved on this target at all).
 * WAV decode/resample is shared, real C code (audio_wav.c) -- NOT the
 * browser's own decodeAudioData, deliberately: decodeAudioData is async,
 * and phi_audio_load_sound's real signature (matching audio_native.c's)
 * is synchronous, so decoding happens on the C side and the resulting
 * PCM float samples are handed to a real Web Audio AudioBuffer via
 * copyToChannel -- entirely synchronous, no Asyncify needed.
 *
 * Positional audio uses a real Web Audio PannerNode (panningModel
 * 'equalpower') connected through context.listener, not a hand-rolled
 * pan/attenuation calculation -- the browser's own audio graph already
 * does this continuously and correctly as the listener moves, which is
 * simpler AND more correct than reimplementing distance falloff/panning
 * in C the way audio_native.c's ALSA mixer thread has to. */
#include "phi_audio.h"
#include "audio_wav.h"
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>

#define PHI_AUDIO_MIXER_RATE 44100

struct PhiSound {
    int js_buffer_handle;   /* index into JS-side audioBuffers[], see audio_wasm_load_from_samples below */
};

EM_JS(int, audio_wasm_init_js, (void), {
    if (!Module.phiAudio) {
        Module.phiAudio = {
            ctx: null,
            buffers: [],   // AudioBuffer, indexed by handle
            voices: [],    // { source, gain, panner } or null, indexed by handle
        };
    }
    try {
        Module.phiAudio.ctx = new (window.AudioContext || window.webkitAudioContext)();
        console.log("[audio] real Web Audio context ready, sampleRate=" + Module.phiAudio.ctx.sampleRate);
        return 1;
    } catch (e) {
        console.log("[audio] Web Audio unavailable: " + e);
        return 0;
    }
});

/* Builds a real AudioBuffer directly from already-decoded interleaved
 * stereo float32 samples (ptr/frame_count point into wasm heap memory --
 * HEAPF32 access below is the standard, documented Emscripten pattern
 * for handing a C float* to JS), synchronously -- no decodeAudioData,
 * see this file's own top comment for why. Returns a real handle
 * (index into Module.phiAudio.buffers), or -1. */
/* The 44100 literal below must stay in sync with PHI_AUDIO_MIXER_RATE --
 * EM_JS's body is opaque text to Emscripten's own build-time extraction
 * step, not run through the normal C preprocessor, so the #define can't
 * be referenced directly here the way it can everywhere else in this
 * file. */
EM_JS(int, audio_wasm_load_from_samples, (const float *ptr, int frame_count), {
    if (!Module.phiAudio || !Module.phiAudio.ctx) return -1;
    var ctx = Module.phiAudio.ctx;
    var buf = ctx.createBuffer(2, frame_count, 44100);
    var left = buf.getChannelData(0), right = buf.getChannelData(1);
    var base = (ptr >> 2);   // float index, HEAPF32 is 4 bytes/element
    for (var i = 0; i < frame_count; i++) {
        left[i]  = HEAPF32[base + i*2 + 0];
        right[i] = HEAPF32[base + i*2 + 1];
    }
    Module.phiAudio.buffers.push(buf);
    return Module.phiAudio.buffers.length - 1;
});

EM_JS(int, audio_wasm_play_js, (int buffer_handle, float volume, int loop, int is_3d, float x, float y, float z), {
    var A = Module.phiAudio;
    if (!A || !A.ctx || buffer_handle < 0 || buffer_handle >= A.buffers.length) return -1;
    var ctx = A.ctx;
    var source = ctx.createBufferSource();
    source.buffer = A.buffers[buffer_handle];
    source.loop = !!loop;

    var gain = ctx.createGain();
    gain.gain.value = volume;

    var panner = null;
    if (is_3d) {
        panner = ctx.createPanner();
        panner.panningModel = 'equalpower';
        panner.distanceModel = 'inverse';
        panner.refDistance = 1.0;
        panner.rolloffFactor = 1.0;
        if (panner.positionX) { panner.positionX.value = x; panner.positionY.value = y; panner.positionZ.value = z; }
        else { panner.setPosition(x, y, z); }   // older Safari fallback
        source.connect(panner);
        panner.connect(gain);
    } else {
        source.connect(gain);
    }
    gain.connect(ctx.destination);

    var handle = A.voices.length;
    for (var i = 0; i < A.voices.length; i++) if (A.voices[i] === null) { handle = i; break; }
    var entry = { source: source, gain: gain, panner: panner };
    if (handle === A.voices.length) A.voices.push(entry); else A.voices[handle] = entry;

    source.onended = function() { if (A.voices[handle] === entry) A.voices[handle] = null; };
    source.start(0);
    return handle;
});

EM_JS(void, audio_wasm_stop_js, (int voice_handle), {
    var A = Module.phiAudio;
    if (!A || voice_handle < 0 || voice_handle >= A.voices.length) return;
    var v = A.voices[voice_handle];
    if (!v) return;
    try { v.source.stop(0); } catch (e) { }
    A.voices[voice_handle] = null;
});

EM_JS(void, audio_wasm_set_listener_js, (float x, float y, float z), {
    var A = Module.phiAudio;
    if (!A || !A.ctx) return;
    var l = A.ctx.listener;
    if (l.positionX) { l.positionX.value = x; l.positionY.value = y; l.positionZ.value = z; }
    else if (l.setPosition) { l.setPosition(x, y, z); }   // older Safari fallback
});

void phi_audio_init(void) {
    if (!audio_wasm_init_js()) {
        printf("[audio] Web Audio unavailable in this browser -- sounds still load and decode for real, but nothing will actually play\n");
    }
}

void phi_audio_shutdown(void) { }

PhiSound *phi_audio_load_sound(const char *path) {
    float *samples; int frame_count;
    if (!phi_wav_load_resampled(path, PHI_AUDIO_MIXER_RATE, &samples, &frame_count)) {
        printf("[audio] failed to load '%s' (missing file, not a WAV, or an unsupported PCM layout -- see audio_wav.h's scope note)\n", path);
        return NULL;
    }
    int handle = audio_wasm_load_from_samples(samples, frame_count);
    free(samples);   /* already copied into the real AudioBuffer above -- this C-side copy's job is done */
    if (handle < 0) return NULL;
    PhiSound *s = (PhiSound *)malloc(sizeof(PhiSound));
    s->js_buffer_handle = handle;
    return s;
}

PhiAudioVoice phi_audio_play(PhiSound *sound, float volume, int loop) {
    if (!sound) return -1;
    return audio_wasm_play_js(sound->js_buffer_handle, volume, loop, 0, 0.0f, 0.0f, 0.0f);
}

PhiAudioVoice phi_audio_play_3d(PhiSound *sound, Vec3f position, float volume, int loop) {
    if (!sound) return -1;
    return audio_wasm_play_js(sound->js_buffer_handle, volume, loop, 1, position.x, position.y, position.z);
}

void phi_audio_stop(PhiAudioVoice voice) {
    audio_wasm_stop_js(voice);
}

void phi_audio_set_listener(Vec3f position, Vec3f forward, Vec3f right) {
    (void)forward; (void)right;   /* equalpower panning only needs listener position, not orientation */
    audio_wasm_set_listener_js(position.x, position.y, position.z);
}
