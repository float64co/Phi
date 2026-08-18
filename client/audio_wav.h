#pragma once

/* Internal WAV decode + resample helper shared by every phi_audio.h
 * backend (audio_native.c's real ALSA mixer AND audio_wasm.c's real Web
 * Audio path both call this -- see phi_audio.h's own top comment on why
 * that sharing matters: one real decoder, not two to keep in sync).
 * NOT part of the public phi_audio.h API -- game/src authors never
 * include this directly. Plain, portable C: fopen/fread/malloc only, no
 * platform-specific calls, safe to compile on every target including the
 * win32 stub. */

/* Reads an uncompressed PCM WAV file (16-bit signed, mono or stereo --
 * see phi_audio.h's own scope note) and resamples it (simple linear
 * interpolation, not a high-quality resampler) to `target_rate`,
 * upmixing mono to stereo if needed -- every PhiSound this codebase ever
 * produces is real, interleaved stereo float32 in [-1,1] at exactly
 * `target_rate`, regardless of the source file's own format, so every
 * backend's mixer only ever has one sample format to deal with. Returns
 * 1 and fills *out_samples (malloc'd, caller frees) and *out_frame_count
 * on success; 0 on a missing file, a non-WAV/compressed file, or an
 * unsupported PCM layout (anything other than 16-bit mono/stereo). */
int phi_wav_load_resampled(const char *path, int target_rate, float **out_samples, int *out_frame_count);
