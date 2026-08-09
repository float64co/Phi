#include "phi_prop.h"
#include <string.h>

const PhiProp *phi_prop_find(const PhiPropGroup *group, const char *identifier) {
    for (int i = 0; i < group->count; i++) {
        if (strcmp(group->props[i].identifier, identifier) == 0) return &group->props[i];
    }
    return NULL;
}

int phi_prop_get_float(void *owner, const PhiProp *prop, float *out) {
    char *p = (char *)owner + prop->offset;
    switch (prop->type) {
        case PHI_PROP_FLOAT: *out = *(float *)p; return 1;
        case PHI_PROP_INT:   *out = (float)(*(int *)p); return 1;
        case PHI_PROP_BOOL:  *out = (*(int *)p) ? 1.0f : 0.0f; return 1;
        default: return 0;
    }
}

int phi_prop_set_float(void *owner, const PhiProp *prop, float value) {
    if (prop->range[0] != prop->range[1]) {
        if (value < prop->range[0]) value = prop->range[0];
        if (value > prop->range[1]) value = prop->range[1];
    }
    char *p = (char *)owner + prop->offset;
    switch (prop->type) {
        case PHI_PROP_FLOAT: *(float *)p = value; return 1;
        case PHI_PROP_INT:   *(int *)p = (int)value; return 1;
        case PHI_PROP_BOOL:  *(int *)p = (value != 0.0f) ? 1 : 0; return 1;
        default: return 0;
    }
}

int phi_prop_get_vec3(void *owner, const PhiProp *prop, float out[3]) {
    if (prop->type != PHI_PROP_VEC3) return 0;
    float *p = (float *)((char *)owner + prop->offset);
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
    return 1;
}

int phi_prop_set_vec3(void *owner, const PhiProp *prop, const float value[3]) {
    if (prop->type != PHI_PROP_VEC3) return 0;
    float *p = (float *)((char *)owner + prop->offset);
    p[0] = value[0]; p[1] = value[1]; p[2] = value[2];
    return 1;
}
