#pragma once

#include "../AmalgamCompat.h"
#include "../Simulation/ProjectileSimulation/ProjectileSimulation.h"

class CTrajectoryPreview
{
public:
    // Draw real-time trajectory preview based on current view angles
    // bQuick = true: real-time preview (every frame)
    // bQuick = false: shot path (when firing)
    void ProjectileTrace(C_TFPlayer* pPlayer, C_TFWeaponBase* pWeapon, bool bQuick = true);
};

MAKE_SINGLETON_SCOPED(CTrajectoryPreview, TrajectoryPreview, F);
