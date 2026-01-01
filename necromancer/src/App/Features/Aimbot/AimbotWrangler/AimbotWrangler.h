#pragma once

#include "../AimbotCommon/AimbotCommon.h"
#include "../../LagRecords/LagRecords.h"
#include "../../MovementSimulation/MovementSimulation.h"

// Wrangler Aimbot - handles both hitscan (bullets) and projectile (rockets) aiming
// The Wrangler allows manual control of the Engineer's sentry gun:
// - Primary fire (IN_ATTACK) = bullets (hitscan from sentry position)
// - Alt fire (IN_ATTACK2) = rockets (projectile from sentry position, level 3 only)
// Sentry rockets travel in a straight line at ~1100 units/sec

class CAimbotWrangler
{
	struct WranglerTarget_t : AimTarget_t
	{
		int AimedHitbox = -1;
		float SimulationTime = -1.0f;
		const LagRecord_t* LagRecord = nullptr;
		bool IsRocketTarget = false;  // True if this target is for rocket prediction
		float PredictedTime = 0.0f;   // Time to reach target (for rockets)
	};

	std::vector<WranglerTarget_t> m_vecTargets = {};
	
	// Rocket cooldown tracking
	float m_flLastRocketFireTime = 0.0f;  // Last time we fired a rocket (0 = ready immediately)
	int m_nLastRocketAmmo = -1;           // Track ammo to detect when rocket was fired
	// Wrangler fires rockets 25% faster than unwrangled (3.0 * 0.75 = 2.25 seconds)
	static constexpr float ROCKET_COOLDOWN = 2.25f;
	


	// Get the local player's sentry gun
	C_ObjectSentrygun* GetLocalSentry(C_TFPlayer* pLocal);
	
	// Get the shoot position of the sentry (where bullets/rockets originate)
	Vec3 GetSentryShootPos(C_ObjectSentrygun* pSentry);
	
	// Get the rocket launch position (slightly different from bullet origin)
	Vec3 GetSentryRocketPos(C_ObjectSentrygun* pSentry);
	
	// Check if sentry can fire rockets (level 3 with ammo)
	bool CanFireRockets(C_ObjectSentrygun* pSentry);
	
	// Check if sentry can fire bullets
	bool CanFireBullets(C_ObjectSentrygun* pSentry);
	
	// Get targets for hitscan (bullets)
	bool GetHitscanTarget(C_TFPlayer* pLocal, C_ObjectSentrygun* pSentry, WranglerTarget_t& outTarget);
	
	// Get targets for projectile (rockets) with movement prediction
	bool GetRocketTarget(C_TFPlayer* pLocal, C_ObjectSentrygun* pSentry, WranglerTarget_t& outTarget);
	
	// Calculate rocket travel time to target
	float GetRocketTravelTime(const Vec3& vFrom, const Vec3& vTo);
	
	// Predict target position after travel time
	Vec3 PredictTargetPosition(C_TFPlayer* pTarget, float flTime);
	
	// Splash prediction for rockets
	std::vector<Vec3> GenerateSplashSphere(float flRadius, int nSamples);
	bool FindSplashPoint(C_TFPlayer* pLocal, C_ObjectSentrygun* pSentry, const Vec3& vTargetPos, C_BaseEntity* pTarget, Vec3& outSplashPoint);
	
	// Aim at target
	void Aim(CUserCmd* pCmd, C_TFPlayer* pLocal, const Vec3& vAngles);
	
	// Should we fire?
	bool ShouldFire(C_TFPlayer* pLocal, C_ObjectSentrygun* pSentry, const WranglerTarget_t& target);

public:
	void Run(CUserCmd* pCmd, C_TFPlayer* pLocal, C_TFWeaponBase* pWeapon);
	bool IsWrangler(C_TFWeaponBase* pWeapon);
};

MAKE_SINGLETON_SCOPED(CAimbotWrangler, AimbotWrangler, F);
