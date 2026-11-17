#include "rollback.h"

int rb_release_decide(uint32_t image_version, const rb_release_in_t* in, rb_source_t* src) {
    if (!in || !src) return RB_E_ARGS;
    *src = RB_SRC_STATIC_MIN;
    if (in->static_min && image_version < in->static_min) return RB_E_ROLLBACK_MIN;
    if (in->tpm_counter_ok) {
        if ((uint64_t)image_version < in->counter) return RB_E_ROLLBACK_TPM;
        *src = RB_SRC_TPM_COUNTER;
        return RB_OK;
    }
    if (in->require_tpm) return RB_E_NO_TPM;
    return RB_OK;
}

uint32_t rb_release_raise_steps(uint32_t image_version, uint64_t counter) {
    if ((uint64_t)image_version <= counter) return 0;
    uint64_t gap = (uint64_t)image_version - counter;
    return gap > RB_MAX_RAISE ? RB_MAX_RAISE : (uint32_t)gap;
}

const char* rb_release_strerror(int rc) {
    switch (rc) {
    case RB_OK:               return "release is not older than the TPM counter";
    case RB_E_ARGS:           return "internal error: bad arguments";
    case RB_E_ROLLBACK_TPM:   return "release is older than the TPM rollback counter (rollback refused)";
    case RB_E_ROLLBACK_MIN:   return "image version is older than the allowed minimum (rollback refused)";
    case RB_E_NO_TPM:         return "rollback counter in TPM is required (KIRA_REQUIRE_TPM=1) but not available";
    default:                  return "unknown error";
    }
}

rb_disk_verdict_t rb_disk_check(uint64_t disk_epoch, int anchor_state, uint64_t anchor) {
    if (anchor_state < 0) return RB_DISK_NO_TPM;
    if (anchor_state == 0) return RB_DISK_NO_ANCHOR;
    return disk_epoch < anchor ? RB_DISK_ROLLBACK : RB_DISK_OK;
}

int rb_anchor_should_write(int anchor_state, uint64_t anchor, uint64_t persisted_epoch) {
    if (anchor_state < 0) return 0;
    if (anchor_state == 0) return 1;
    return persisted_epoch > anchor;
}

void rb_anchor_encode(uint64_t epoch, uint8_t out[RB_ANCHOR_LEN]) {
    out[0] = 'K'; out[1] = 'F'; out[2] = 'S'; out[3] = 'A';
    out[4] = 0; out[5] = 0; out[6] = 0; out[7] = 1;
    for (int i = 0; i < 8; i++) out[8 + i] = (uint8_t)(epoch >> (56 - 8 * i));
}

int rb_anchor_decode(const uint8_t in[RB_ANCHOR_LEN], uint64_t* epoch) {
    if (!in || !epoch) return RB_E_ARGS;
    if (in[0] != 'K' || in[1] != 'F' || in[2] != 'S' || in[3] != 'A') return RB_E_ARGS;
    if (in[4] || in[5] || in[6] || in[7] != 1) return RB_E_ARGS;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | in[8 + i];
    *epoch = v;
    return RB_OK;
}
