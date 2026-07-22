#include "cadence.h"
#include <math.h>

CadenceDecision cadence_decide(CadenceState& st, const CadenceParams& p,
                               bool has_noise, float noise_dba, uint32_t now_ms) {
    // Arm densification on either trigger:
    //  - delta: the level moved fast between two captures (impulsive onset/decay);
    //  - absolute: the level is simply loud (a flyover ramps too slowly per-capture
    //    to ever trip the delta). Each loud capture re-arms, so the hold window
    //    rides through the plateau and covers the decay tail after it drops.
    if (has_noise && ((st.have_last && fabsf(noise_dba - st.last_noise) >= p.densify_delta_dba)
                      || noise_dba >= p.densify_abs_dba)) {
        st.densify_until_ms = now_ms + p.densify_hold_ms;
    }
    const bool densified = (int32_t)(st.densify_until_ms - now_ms) > 0;

    bool store;
    if (!st.primed) {
        store = true;                     // always store the first capture after boot
    } else if (densified) {
        store = true;                     // every capture during an event
    } else {
        // Quiet: store at the baseline interval. Unsigned-safe elapsed compare.
        store = (uint32_t)(now_ms - st.last_store_ms) >= p.quiet_store_ms;
    }

    if (has_noise) { st.last_noise = noise_dba; st.have_last = true; }
    if (store)     { st.last_store_ms = now_ms; }
    st.primed = true;

    return CadenceDecision{ store, densified };
}
