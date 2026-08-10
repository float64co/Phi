#pragma once
#include "vec3.h"
#include "meshobject.h"   /* Quat, quat_slerp */
#include "armature.h"
#include "cgltf.h"

/* Phase 4 (see phi.md's "Animation Editor" -- Architecture): the Clip/
 * Curve/Playback layers. Clip+Curve are one module here (a channel's
 * keyframes ARE its curve -- interpolation lives inside
 * animation_sample_clip, not a separate type) since splitting them
 * wouldn't add anything a Bezier-curve-editor UI doesn't yet need;
 * Playback is the small AnimPlayback struct + advance function below. */

#define ANIM_MAX_CHANNELS   256   /* up to 3 channels (T/R/S) per bone, generous headroom over ARMATURE_MAX_BONES */
#define ANIM_NAME_LEN        64

typedef enum {
    ANIM_PATH_TRANSLATION,
    ANIM_PATH_ROTATION,
    ANIM_PATH_SCALE,
} AnimPath;

typedef enum {
    ANIM_INTERP_LINEAR,
    ANIM_INTERP_STEP,
    ANIM_INTERP_CUBIC_SPLINE,
} AnimInterp;

/* One channel: a single (bone, path)'s keyframes over time, sourced
 * directly from one cgltf_animation_channel + its sampler.
 *
 * times/values are HEAP-ALLOCATED, sized exactly to keyframe_count at
 * load time (animation_load_clips) -- NOT a fixed-size embedded array.
 * A first version of this module used a fixed
 * `float values[512][3][4]` per channel; with up to ANIM_MAX_CHANNELS
 * channels per clip, sizeof(AnimClip) came out to roughly 1.7MB, and
 * stack-allocating even a handful of clips (as animation_test_main.c's
 * own first run did) overflowed the stack -- a real crash, caught by
 * actually running the test, not a hypothetical one. Dynamic allocation
 * fixes the root cause (no arbitrary keyframe-count cap needed at all,
 * and a clip with only a few small channels costs only a few hundred
 * bytes, not ~1.7MB regardless of what it actually contains) rather
 * than just raising or shrinking the fixed cap, which would only move
 * the same problem to a different clip size. See animation_clip_free.
 *
 * values[k] holds keyframe k's data as a (in-tangent, value,
 * out-tangent) triple of up-to-4-float slots -- glTF's own cubic-spline
 * convention (see the spec's "Cubic Spline Interpolation" section)
 * stores exactly that triple per keyframe; LINEAR/STEP channels only
 * ever populate slot [1] (the value), keeping one uniform layout instead
 * of two separate ones so animation_sample_clip doesn't need a separate
 * code path just to find "the value" depending on interpolation mode. */
typedef struct {
    int        bone_index;    /* into the Armature this clip was loaded against */
    AnimPath   path;
    AnimInterp interp;
    int        keyframe_count;
    float      *times;         /* malloc'd, keyframe_count floats */
    float      (*values)[3][4]; /* malloc'd, keyframe_count entries */
} AnimChannel;

typedef struct {
    char        name[ANIM_NAME_LEN];
    AnimChannel channels[ANIM_MAX_CHANNELS];
    int         channel_count;
    float       duration;   /* max keyframe time across all channels, seconds */
} AnimClip;

/* Loads every animation clip in `data`, mapping each channel's target
 * glTF node to a bone index via armature_find_bone (by node name) -- a
 * channel targeting a node that isn't one of arm's bones is silently
 * skipped, not an error: a real glTF file's animations can target
 * non-joint nodes (a camera, a whole-object transform), which this
 * Phase 4 first slice doesn't animate yet. A clip left with zero usable
 * channels after that filtering is dropped entirely (nothing in it would
 * ever do anything). Morph-target (`weights`) channels are skipped too --
 * out of scope for this pass, see phi.md. Returns the number of clips
 * written into out_clips (capped at max_clips) -- each successfully
 * loaded clip owns real heap allocations (its channels' times/values)
 * that MUST be released via animation_clip_free when done with it. */
int animation_load_clips(const cgltf_data *data, const Armature *arm,
                          AnimClip *out_clips, int max_clips);

/* Frees every channel's times/values allocations in clip (safe to call
 * on an already-freed or zeroed clip -- free(NULL) is a no-op). Does NOT
 * free `clip` itself (callers own that storage, same convention as
 * halfedge_destroy taking a HalfEdgeMesh* it doesn't free). */
void animation_clip_free(AnimClip *clip);

/* Samples clip at `time` (seconds -- NOT wrapped or clamped here, see
 * anim_playback_advance for that), writing this instant's local pose
 * into out_t/out_r/out_s (each must have arm->bone_count entries,
 * indexed by bone_index, and should be SEEDED with the armature's rest
 * pose by the caller first -- armature_rest_pose). A bone with no
 * channel in this clip is left exactly as the caller seeded it, not
 * overwritten with a default identity/zero -- so a clip that only
 * animates one bone doesn't silently snap every other bone in the rig
 * back to its rest pose (or worse, to an uninitialized value) the moment
 * it's sampled. Time before the first keyframe or after the last one
 * clamps to that endpoint's value (holds the pose), matching glTF's own
 * defined out-of-range sampling behavior. */
void animation_sample_clip(const AnimClip *clip, float time,
                            Vec3f *out_t, Quat *out_r, Vec3f *out_s);

typedef struct {
    const AnimClip *clip;
    float time;
    int   loop;
    int   playing;
} AnimPlayback;

/* Starts (or restarts) playback of `clip` from time 0. */
void anim_playback_play(AnimPlayback *pb, const AnimClip *clip, int loop);

/* Advances pb->time by dt -- wraps (fmod) if pb->loop, otherwise clamps
 * to pb->clip->duration and sets pb->playing = 0 once reached (a
 * non-looping clip stops itself, callers don't need to poll duration
 * themselves to know when it's done). No-op if pb->playing is 0 or
 * pb->clip is NULL. */
void anim_playback_advance(AnimPlayback *pb, float dt);
