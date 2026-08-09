#pragma once
#include <stddef.h>

/* DNA/RNA-style property descriptor system (see phi.md's "Property
 * System (DNA/RNA analogue)" and the Asset tracking/Native UI System
 * sections it's referenced from). Single source of truth for "what
 * fields does this C struct expose, under what name, as what type" --
 * used generically by ui.c's Properties panel (real widgets, not a
 * hardcoded printf per field) and by the MicroPython binding layer
 * (phi_prop_bindings.c's phi.prop_get/set), so both read exactly the
 * same registry rather than each hand-rolling their own idea of "what a
 * MeshObject's editable fields are."
 *
 * Deliberately offset-based (via C11 offsetof at registration time, see
 * phi_prop_registry.c), not getter/setter-function-pointer-based: the
 * common case (a plain float/vec3/bool/int field sitting at a fixed
 * offset in a struct) needs no per-property C code at all, matching
 * Blender's own actual DNA/RNA approach. This module itself has zero
 * knowledge of MeshObject/HEFace/etc. -- it only ever deals in raw byte
 * offsets and a type tag; phi_prop_registry.c is what actually knows
 * which struct's which field a given PhiProp describes. */

typedef enum {
    PHI_PROP_FLOAT,
    PHI_PROP_INT,
    PHI_PROP_BOOL,
    PHI_PROP_VEC3,
} PhiPropType;

typedef struct PhiProp {
    const char *identifier;   /* stable, scriptable name, e.g. "position" -- what phi.prop_get/set and ctx.prop take */
    const char *display_name; /* what the Properties panel shows as a label, e.g. "Position" */
    PhiPropType type;
    float       range[2];     /* [min,max] for FLOAT/INT, clamped on set; {0,0} = unbounded (0 is never a valid nonzero range since min==max) */
    size_t      offset;       /* byte offset into the owning struct -- see offsetof() at registration */
} PhiProp;

typedef struct {
    const char    *type_name;  /* informational, e.g. "MeshObject" */
    const PhiProp *props;
    int            count;
} PhiPropGroup;

/* Linear search by identifier -- property counts per group are small
 * (single digits), no need for a hash map. Returns NULL if not found. */
const PhiProp *phi_prop_find(const PhiPropGroup *group, const char *identifier);

/* Generic get/set against `owner` (a pointer to a real instance of
 * whatever struct `group`/`prop` describes, e.g. a MeshObject* or
 * HEFace*) + `prop` (from phi_prop_find, must belong to that struct
 * type -- this module has no way to check that itself, callers are
 * trusted, same as any offsetof-based access). Returns 0 (out untouched)
 * on a PhiPropType mismatch (e.g. calling the float accessor on a VEC3
 * prop), 1 on success. FLOAT accessors also handle INT/BOOL props,
 * reading/writing through as a float (BOOL is 0.0/1.0, INT is exact for
 * any value a real UI slider would produce) -- one code path for every
 * scalar-shaped prop, since InputState-style text fields don't
 * distinguish "3" from "3.0" anyway. */
int phi_prop_get_float(void *owner, const PhiProp *prop, float *out);
int phi_prop_set_float(void *owner, const PhiProp *prop, float value);   /* clamped to prop->range if range[0] != range[1] */
int phi_prop_get_vec3(void *owner, const PhiProp *prop, float out[3]);
int phi_prop_set_vec3(void *owner, const PhiProp *prop, const float value[3]);
