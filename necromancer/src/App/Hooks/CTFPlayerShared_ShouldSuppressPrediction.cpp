// ============================================
// CTFPlayerShared_ShouldSuppressPrediction Hook
// ============================================
// This hook ensures prediction is never suppressed.
//
// The game normally suppresses prediction when in Halloween karts
// (bumper cars mode). This can cause issues with tick manipulation
// because the client won't predict movement/actions.
//
// By always returning false, we ensure prediction runs consistently,
// which is important for:
// - Accurate aimbot during doubletap
// - Proper antiwarp calculations
// - Consistent tickbase manipulation
//
// This is a simple, safe hook that just prevents an edge case.
// ============================================

#include "../../SDK/SDK.h"

MAKE_SIGNATURE(CTFPlayerShared_ShouldSuppressPrediction, "client.dll", "8B 81 ? ? ? ? 0F BA E0", 0x0);

MAKE_HOOK(CTFPlayerShared_ShouldSuppressPrediction, Signatures::CTFPlayerShared_ShouldSuppressPrediction.Get(), bool, __fastcall,
    void* rcx)
{
    // Never suppress prediction
    // Original behavior: return InCond(TF_COND_HALLOWEEN_KART) && !InCond(TF_COND_HALLOWEEN_GHOST_MODE)
    return false;
}
