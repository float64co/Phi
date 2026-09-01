#include "animation.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Z-up (2026-08-19, see vec3.h's coordinate-convention note): converts
 * one raw keyframe value slot in place, dispatched by channel path --
 * translation/rotation use the same real conversion armature.c's own
 * bone rest_translation/rest_rotation do (see meshobject.c's quat_y_up_
 * to_z_up for why rotation needs actual quaternion conjugation, not just
 * a component swap, and client/quat_axis_convert_test_main.c for how
 * that's verified); scale uses the separate non-negating vec3_swap_yz_
 * for_scale (see its own comment in vec3.h). Cubic-spline in/out tangent
 * slots get the identical conversion as the value slot -- a tangent is a
 * derivative vector, and differentiation commutes with the linear change
 * of basis every one of these conversions is, so there's no separate
 * "tangent version" of any of them needed. */
static void convert_channel_value(AnimPath path, float *v) {
    if (path == ANIM_PATH_TRANSLATION) {
        Vec3f p = vec3_y_up_to_z_up((Vec3f){v[0], v[1], v[2]});
        v[0] = p.x; v[1] = p.y; v[2] = p.z;
    } else if (path == ANIM_PATH_ROTATION) {
        Quat q = quat_y_up_to_z_up((Quat){v[0], v[1], v[2], v[3]});
        v[0] = q.x; v[1] = q.y; v[2] = q.z; v[3] = q.w;
    } else {   /* ANIM_PATH_SCALE */
        Vec3f s = vec3_swap_yz_for_scale((Vec3f){v[0], v[1], v[2]});
        v[0] = s.x; v[1] = s.y; v[2] = s.z;
    }
}

int animation_load_clips(const cgltf_data *data, const Armature *arm,
                          AnimClip *out_clips, int max_clips) {
    if (!data || !arm || !out_clips) return 0;
    int clip_count = 0;

    for (cgltf_size a = 0; a < data->animations_count && clip_count < max_clips; a++) {
        const cgltf_animation *anim = &data->animations[a];
        AnimClip *clip = &out_clips[clip_count];
        memset(clip, 0, sizeof(*clip));
        strncpy(clip->name, anim->name ? anim->name : "", ANIM_NAME_LEN - 1);
        clip->name[ANIM_NAME_LEN - 1] = 0;

        int ch_count = 0;
        for (cgltf_size c = 0; c < anim->channels_count && ch_count < ANIM_MAX_CHANNELS; c++) {
            const cgltf_animation_channel *ch = &anim->channels[c];
            if (ch->target_path == cgltf_animation_path_type_weights) continue;   /* morph targets -- out of scope this pass */
            if (ch->target_path == cgltf_animation_path_type_invalid) continue;
            if (!ch->target_node || !ch->target_node->name) continue;

            int bone_idx = armature_find_bone(arm, ch->target_node->name);
            if (bone_idx < 0) continue;   /* not one of this armature's bones -- skip, not an error */

            const cgltf_animation_sampler *samp = ch->sampler;
            if (!samp || !samp->input || !samp->output) continue;

            AnimChannel *out_ch = &clip->channels[ch_count];
            out_ch->bone_index = bone_idx;
            out_ch->path = (ch->target_path == cgltf_animation_path_type_translation) ? ANIM_PATH_TRANSLATION :
                           (ch->target_path == cgltf_animation_path_type_rotation)    ? ANIM_PATH_ROTATION :
                                                                                          ANIM_PATH_SCALE;
            out_ch->interp = (samp->interpolation == cgltf_interpolation_type_step)         ? ANIM_INTERP_STEP :
                              (samp->interpolation == cgltf_interpolation_type_cubic_spline) ? ANIM_INTERP_CUBIC_SPLINE :
                                                                                                 ANIM_INTERP_LINEAR;

            int kf_count = (int)samp->input->count;
            if (kf_count <= 0) continue;
            out_ch->keyframe_count = kf_count;
            /* Heap-allocated, sized exactly to this channel's real
             * keyframe count -- see AnimChannel's own comment on why
             * (a fixed embedded array here previously made AnimClip
             * ~1.7MB regardless of actual content, and stack-allocating
             * even a handful overflowed the stack -- a real crash this
             * session's own test run caught). */
            out_ch->times = (float *)malloc((size_t)kf_count * sizeof(float));
            out_ch->values = (float (*)[3][4])malloc((size_t)kf_count * sizeof(float[3][4]));

            int comp = (out_ch->path == ANIM_PATH_ROTATION) ? 4 : 3;
            int cubic = (out_ch->interp == ANIM_INTERP_CUBIC_SPLINE);

            for (int k = 0; k < kf_count; k++) {
                float t = 0.0f;
                cgltf_accessor_read_float(samp->input, (cgltf_size)k, &t, 1);
                out_ch->times[k] = t;
                if (t > clip->duration) clip->duration = t;

                if (cubic) {
                    /* glTF cubic-spline sampler output packs 3 entries per
                     * keyframe (in-tangent, value, out-tangent), each
                     * `comp` floats -- read all three into their matching
                     * slot so animation_sample_clip's Hermite evaluation
                     * has both tangents available. */
                    for (int slot = 0; slot < 3; slot++) {
                        cgltf_accessor_read_float(samp->output, (cgltf_size)(k * 3 + slot), out_ch->values[k][slot], comp);
                        convert_channel_value(out_ch->path, out_ch->values[k][slot]);
                    }
                } else {
                    /* LINEAR/STEP: one value per keyframe, stored uniformly
                     * in slot [1] (the "value" slot) so sampling doesn't
                     * need to branch on interpolation mode to find it. */
                    cgltf_accessor_read_float(samp->output, (cgltf_size)k, out_ch->values[k][1], comp);
                    convert_channel_value(out_ch->path, out_ch->values[k][1]);
                }
            }
            ch_count++;
        }
        clip->channel_count = ch_count;
        if (ch_count > 0) {
            clip_count++;   /* a clip with zero usable channels (e.g. entirely on non-joint nodes) is dropped */
        }
        /* A dropped clip's already-allocated channels (if any survived
         * before the final one failed some later check) are freed here
         * too -- ch_count==0 for a genuinely empty clip means nothing
         * was ever malloc'd, so this is always safe, never a double-work
         * cost in the common case. */
        else {
            animation_clip_free(clip);
        }
    }
    return clip_count;
}

void animation_clip_free(AnimClip *clip) {
    if (!clip) return;
    for (int i = 0; i < clip->channel_count; i++) {
        free(clip->channels[i].times);
        free(clip->channels[i].values);
        clip->channels[i].times = NULL;
        clip->channels[i].values = NULL;
    }
}

/* Binary search for the keyframe interval bracketing `time`: returns i
 * such that times[i] <= time < times[i+1], clamped to [0, count-1] for
 * time outside the channel's own range (glTF's defined out-of-range
 * behavior: hold the nearest endpoint). */
static int find_keyframe(const AnimChannel *ch, float time) {
    if (time <= ch->times[0]) return 0;
    if (time >= ch->times[ch->keyframe_count - 1]) return ch->keyframe_count - 1;
    int lo = 0, hi = ch->keyframe_count - 1;
    while (lo + 1 < hi) {
        int mid = (lo + hi) / 2;
        if (ch->times[mid] <= time) lo = mid; else hi = mid;
    }
    return lo;
}

/* glTF's cubic Hermite spline formula (see the spec's "Appendix C:
 * Interpolation"): p0/p1 are the value slots, m0/m1 the tangents scaled
 * by the keyframe interval dt (the spec requires this scaling, not raw
 * tangents), t in [0,1] is the normalized position within the interval. */
static void hermite(const float *p0, const float *m0_raw, const float *p1, const float *m1_raw,
                     float dt, float t, int comp, float *out) {
    float t2 = t * t, t3 = t2 * t;
    float h00 =  2.0f*t3 - 3.0f*t2 + 1.0f;
    float h10 =        t3 - 2.0f*t2 + t;
    float h01 = -2.0f*t3 + 3.0f*t2;
    float h11 =        t3 -      t2;
    for (int i = 0; i < comp; i++) {
        float m0 = m0_raw[i] * dt, m1 = m1_raw[i] * dt;
        out[i] = h00*p0[i] + h10*m0 + h01*p1[i] + h11*m1;
    }
}

static void sample_channel(const AnimChannel *ch, float time, float *out /* 4 floats, caller uses 3 or 4 */) {
    int comp = (ch->path == ANIM_PATH_ROTATION) ? 4 : 3;
    int i = find_keyframe(ch, time);

    if (i >= ch->keyframe_count - 1) {
        memcpy(out, ch->values[ch->keyframe_count - 1][1], (size_t)comp * sizeof(float));
        return;
    }
    int j = i + 1;
    float t0 = ch->times[i], t1 = ch->times[j];
    float dt = t1 - t0;
    float frac = (dt > 0.0f) ? (time - t0) / dt : 0.0f;

    if (ch->interp == ANIM_INTERP_STEP) {
        memcpy(out, ch->values[i][1], (size_t)comp * sizeof(float));
    } else if (ch->interp == ANIM_INTERP_CUBIC_SPLINE) {
        /* values[i][1] = p0 (value), values[i][2] = out-tangent of i,
         * values[j][1] = p1 (value), values[j][0] = in-tangent of j. */
        hermite(ch->values[i][1], ch->values[i][2], ch->values[j][1], ch->values[j][0], dt, frac, comp, out);
        if (ch->path == ANIM_PATH_ROTATION) {
            Quat q = { out[0], out[1], out[2], out[3] };
            q = quat_normalize(q);   /* Hermite doesn't preserve unit length -- glTF's spec expects renormalization */
            out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
        }
    } else if (ch->path == ANIM_PATH_ROTATION) {
        Quat a = { ch->values[i][1][0], ch->values[i][1][1], ch->values[i][1][2], ch->values[i][1][3] };
        Quat b = { ch->values[j][1][0], ch->values[j][1][1], ch->values[j][1][2], ch->values[j][1][3] };
        Quat r = quat_slerp(a, b, frac);
        out[0] = r.x; out[1] = r.y; out[2] = r.z; out[3] = r.w;
    } else {
        const float *a = ch->values[i][1], *b = ch->values[j][1];
        for (int k = 0; k < comp; k++) out[k] = a[k] + (b[k] - a[k]) * frac;
    }
}

void animation_sample_clip(const AnimClip *clip, float time,
                            Vec3f *out_t, Quat *out_r, Vec3f *out_s) {
    if (!clip) return;
    for (int c = 0; c < clip->channel_count; c++) {
        const AnimChannel *ch = &clip->channels[c];
        float v[4];
        sample_channel(ch, time, v);
        int b = ch->bone_index;
        switch (ch->path) {
            case ANIM_PATH_TRANSLATION: out_t[b] = (Vec3f){v[0], v[1], v[2]}; break;
            case ANIM_PATH_ROTATION:    out_r[b] = (Quat){v[0], v[1], v[2], v[3]}; break;
            case ANIM_PATH_SCALE:       out_s[b] = (Vec3f){v[0], v[1], v[2]}; break;
        }
    }
}

void anim_playback_play(AnimPlayback *pb, const AnimClip *clip, int loop) {
    pb->clip = clip;
    pb->time = 0.0f;
    pb->loop = loop;
    pb->playing = 1;
}

void anim_playback_advance(AnimPlayback *pb, float dt) {
    if (!pb->playing || !pb->clip) return;
    pb->time += dt;
    if (pb->clip->duration <= 0.0f) return;
    if (pb->time >= pb->clip->duration) {
        if (pb->loop) {
            pb->time = fmodf(pb->time, pb->clip->duration);
        } else {
            pb->time = pb->clip->duration;
            pb->playing = 0;
        }
    }
}
