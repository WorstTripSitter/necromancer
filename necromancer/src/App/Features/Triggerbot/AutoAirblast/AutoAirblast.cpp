#include "AutoAirblast.h"

#include "../../CFG.h"
#include "../../amalgam_port/AimbotGlobal/AimbotGlobal.h"
#include "../../amalgam_port/Simulation/MovementSimulation/AmalgamMoveSim.h"
#include "../../amalgam_port/Simulation/ProjectileSimulation/ProjectileSimulation.h"

struct TargetProjectile
{
	C_BaseProjectile* Projectile = nullptr;
	Vec3 Position = {};
};

// Rocket launcher speeds (base values before attributes)
// Standard Rocket Launcher: 1100 HU/s
// Direct Hit: 1980 HU/s (1.8x multiplier)
// Liberty Launcher: 1540 HU/s (1.4x multiplier)
// Reflected rockets maintain their original speed

float CAutoAirblast::GetReflectedRocketSpeed(C_BaseProjectile* pProjectile)
{
	if (!pProjectile)
		return 1100.0f;

	// Get the velocity from the projectile
	Vec3 vVel;
	pProjectile->EstimateAbsVelocity(vVel);
	float flSpeed = vVel.Length();

	// If we have a valid speed, use it
	// Rockets maintain constant speed, so any valid velocity should be used
	if (flSpeed > 10.0f)
		return flSpeed;

	// Fallback to default rocket speed
	return 1100.0f;
}

float CAutoAirblast::GetReflectedSplashRadius(C_BaseProjectile* pProjectile)
{
	if (!pProjectile)
		return 146.0f;

	// Base splash radius for rockets is 146 units
	// Direct Hit has reduced splash (44 units)
	// Sentry rockets have standard splash

	switch (pProjectile->GetClassId())
	{
	case ETFClassIds::CTFProjectile_Rocket:
	case ETFClassIds::CTFProjectile_SentryRocket:
		return 146.0f;
	default:
		return 146.0f;
	}
}

EReflectProjectileType CAutoAirblast::GetProjectileType(C_BaseProjectile* pProjectile)
{
	if (!pProjectile)
		return EReflectProjectileType::Unknown;

	switch (pProjectile->GetClassId())
	{
	// Straight line projectiles (no gravity)
	case ETFClassIds::CTFProjectile_Rocket:
	case ETFClassIds::CTFProjectile_SentryRocket:
	case ETFClassIds::CTFProjectile_EnergyBall:
	case ETFClassIds::CTFProjectile_EnergyRing:
		return EReflectProjectileType::Rocket;

	// Arc projectiles (gravity affected)
	case ETFClassIds::CTFGrenadePipebombProjectile:  // Pipes (NOT stickies - those are filtered out)
	case ETFClassIds::CTFProjectile_Arrow:           // Huntsman, Crossbow, Rescue Ranger
	case ETFClassIds::CTFProjectile_HealingBolt:     // Crusader's Crossbow healing bolt
	case ETFClassIds::CTFProjectile_Flare:           // Flare gun
	case ETFClassIds::CTFProjectile_Cleaver:         // Flying Guillotine
	case ETFClassIds::CTFProjectile_Jar:             // Jarate
	case ETFClassIds::CTFProjectile_JarMilk:         // Mad Milk
	case ETFClassIds::CTFProjectile_JarGas:          // Gas Passer
		return EReflectProjectileType::Arc;

	default:
		return EReflectProjectileType::Unknown;
	}
}

float CAutoAirblast::GetReflectedProjectileSpeed(C_BaseProjectile* pProjectile)
{
	if (!pProjectile)
		return 1100.0f;

	// Get the velocity from the projectile - reflected projectiles maintain their speed
	Vec3 vVel;
	pProjectile->EstimateAbsVelocity(vVel);
	float flSpeed = vVel.Length();

	// If we have a valid speed, use it
	// Use a very low threshold (10 HU/s) to catch slow-moving projectiles like rolling pipes
	// Only fall back to hardcoded values if the projectile is essentially stationary
	if (flSpeed > 10.0f)
		return flSpeed;

	// Fallback speeds based on projectile type
	switch (pProjectile->GetClassId())
	{
	case ETFClassIds::CTFProjectile_Rocket:
	case ETFClassIds::CTFProjectile_SentryRocket:
		return 1100.0f;
	case ETFClassIds::CTFGrenadePipebombProjectile:
		return 1200.0f;  // Grenade launcher base speed
	case ETFClassIds::CTFProjectile_Arrow:
	case ETFClassIds::CTFProjectile_HealingBolt:
		return 2400.0f;  // Crossbow/arrow speed
	case ETFClassIds::CTFProjectile_Flare:
		return 2000.0f;
	case ETFClassIds::CTFProjectile_Cleaver:
		return 3000.0f;
	case ETFClassIds::CTFProjectile_Jar:
	case ETFClassIds::CTFProjectile_JarMilk:
		return 1000.0f;
	case ETFClassIds::CTFProjectile_JarGas:
		return 2000.0f;
	case ETFClassIds::CTFProjectile_EnergyBall:
	case ETFClassIds::CTFProjectile_EnergyRing:
		return 1200.0f;
	default:
		return 1100.0f;
	}
}

bool CAutoAirblast::SupportsAimbot(C_BaseProjectile* pProjectile)
{
	if (!pProjectile)
		return false;

	EReflectProjectileType type = GetProjectileType(pProjectile);
	return type != EReflectProjectileType::Unknown;
}

// Get gravity multiplier for arc projectiles (used for pitch compensation)
// Returns 0 for straight-line projectiles
static float GetProjectileGravityMult(C_BaseProjectile* pProjectile)
{
	if (!pProjectile)
		return 0.0f;

	switch (pProjectile->GetClassId())
	{
	// High gravity - pipes, cleaver, jars
	case ETFClassIds::CTFGrenadePipebombProjectile:
		return 1.0f;
	case ETFClassIds::CTFProjectile_Cleaver:
		return 1.0f;
	case ETFClassIds::CTFProjectile_Jar:
	case ETFClassIds::CTFProjectile_JarMilk:
		return 1.0f;
	case ETFClassIds::CTFProjectile_JarGas:
		return 1.0f;

	// Medium gravity - flares
	case ETFClassIds::CTFProjectile_Flare:
		return 0.3f;

	// Low gravity - arrows, crossbow
	case ETFClassIds::CTFProjectile_Arrow:
	case ETFClassIds::CTFProjectile_HealingBolt:
		return 0.2f;

	// No gravity - rockets, energy
	default:
		return 0.0f;
	}
}

bool CAutoAirblast::FindAimbotTarget(C_TFPlayer* pLocal, C_BaseProjectile* pProjectile, Vec3& outAngles,
	std::vector<Vec3>& outPlayerPath, std::vector<Vec3>& outRocketPath)
{
	if (!pLocal || !pProjectile)
		return false;

	// Only support rockets for now (they move in straight lines)
	const auto nClassId = pProjectile->GetClassId();
	if (nClassId != ETFClassIds::CTFProjectile_Rocket && nClassId != ETFClassIds::CTFProjectile_SentryRocket)
		return false;

	const Vec3 vLocalPos = pLocal->GetShootPos();
	const Vec3 vLocalAngles = I::EngineClient->GetViewAngles();
	const float flFOV = CFG::Aimbot_Projectile_FOV;
	const float flRocketSpeed = GetReflectedRocketSpeed(pProjectile);
	const float flSplashRadius = GetReflectedSplashRadius(pProjectile);

	// Get latency for prediction
	const float flLatency = SDKUtils::GetLatency();

	struct AimbotTarget
	{
		C_BaseEntity* pEntity = nullptr;
		Vec3 vPredictedPos = {};
		Vec3 vAimPos = {};
		Vec3 vAimAngles = {};
		float flFOV = 0.0f;
		float flDist = 0.0f;
		bool bSplash = false;
		std::vector<Vec3> vPlayerPath = {};
	};

	std::vector<AimbotTarget> vTargets;

	// Get the projectile's current position - this is where the rocket will be deflected from
	Vec3 vProjectilePos = pProjectile->m_vecOrigin();

	// Find enemy players
	for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PLAYERS_ENEMIES))
	{
		if (!pEntity)
			continue;

		const auto pPlayer = pEntity->As<C_TFPlayer>();
		if (!pPlayer || pPlayer->deadflag())
			continue;

		// Skip invulnerable targets
		if (CFG::Aimbot_Ignore_Invulnerable &&
			(pPlayer->InCond(TF_COND_INVULNERABLE) || pPlayer->InCond(TF_COND_INVULNERABLE_CARD_EFFECT) ||
				pPlayer->InCond(TF_COND_INVULNERABLE_HIDE_UNLESS_DAMAGED) || pPlayer->InCond(TF_COND_INVULNERABLE_USER_BUFF)))
			continue;

		// Skip invisible targets
		if (CFG::Aimbot_Ignore_Invisible && pPlayer->IsInvisible())
			continue;

		// Skip friends
		if (CFG::Aimbot_Ignore_Friends && pPlayer->IsPlayerOnSteamFriendsList())
			continue;

		// Use movement simulation to predict target position
		// We need to iterate to find where the rocket and player will intersect
		MoveStorage tStorage;
		F::MoveSim.Initialize(pPlayer, tStorage);

		Vec3 vPredictedPos = pPlayer->m_vecOrigin();
		std::vector<Vec3> vPlayerPath;
		vPlayerPath.push_back(vPredictedPos);

		// Iteratively simulate to find intersection point
		// The rocket travels from projectile position to where we aim
		// We need to find when rocket arrival time = player simulation time
		int nMaxSimTicks = 66; // Cap at 1 second
		int nBestTick = 0;
		Vec3 vBestPredictedPos = vPredictedPos;
		float flBestTimeDiff = FLT_MAX;

		for (int i = 1; i <= nMaxSimTicks && !tStorage.m_bFailed; i++)
		{
			F::MoveSim.RunTick(tStorage);
			vPredictedPos = tStorage.m_vPredictedOrigin;
			vPlayerPath.push_back(vPredictedPos);

			// Calculate how long rocket would take to reach this predicted position
			float flDistToTarget = vProjectilePos.DistTo(vPredictedPos);
			float flRocketTime = flDistToTarget / flRocketSpeed;
			
			// Player simulation time (including latency)
			float flPlayerTime = TICKS_TO_TIME(i) + flLatency;
			
			// Find the tick where rocket arrival time best matches player simulation time
			float flTimeDiff = fabsf(flRocketTime - flPlayerTime);
			if (flTimeDiff < flBestTimeDiff)
			{
				flBestTimeDiff = flTimeDiff;
				nBestTick = i;
				vBestPredictedPos = vPredictedPos;
			}
			
			// If rocket would arrive before player gets there, we've gone too far
			if (flRocketTime < flPlayerTime && i > 1)
				break;
		}

		F::MoveSim.Restore(tStorage);
		
		// Use the best predicted position
		vPredictedPos = vBestPredictedPos;
		
		// Trim player path to the best tick
		if (vPlayerPath.size() > static_cast<size_t>(nBestTick + 1))
			vPlayerPath.resize(nBestTick + 1);

		// Get target eye position for aiming
		Vec3 vTargetEye = pPlayer->GetViewOffset();

		// Predict where the projectile will be when the server processes our command
		Vec3 vProjectileVel;
		pProjectile->EstimateAbsVelocity(vProjectileVel);
		Vec3 vPredictedProjectilePos = vProjectilePos + vProjectileVel * flLatency;

		// Check if target is on ground for splash
		bool bOnGround = pPlayer->m_fFlags() & FL_ONGROUND;

		// Hitbox fallback system: try feet (splash) -> body -> head
		// This ensures we find a valid aim point even if preferred hitbox is blocked
		struct HitboxAttempt
		{
			Vec3 vAimPos;
			bool bSplash;
			const char* szName;
		};

		std::vector<HitboxAttempt> vHitboxes;

		// 1. Feet (splash) - only if on ground and splash is possible
		if (bOnGround && flSplashRadius > 0.0f)
			vHitboxes.push_back({ vPredictedPos + Vec3(0, 0, 1.0f), true, "feet" });

		// 2. Body - always try
		vHitboxes.push_back({ vPredictedPos + Vec3(0, 0, vTargetEye.z * 0.5f), false, "body" });

		// 3. Head - last resort
		vHitboxes.push_back({ vPredictedPos + Vec3(0, 0, vTargetEye.z * 0.9f), false, "head" });

		// Try each hitbox in order until one works
		Vec3 vAimPos;
		Vec3 vAngles;
		float flFOVTo = 0.0f;
		bool bSplash = false;
		bool bFoundValidHitbox = false;

		CTraceFilterWorldCustom filter{};
		trace_t trace{};

		for (const auto& hitbox : vHitboxes)
		{
			Vec3 vTestAimPos = hitbox.vAimPos;
			bool bTestSplash = hitbox.bSplash;

			if (bTestSplash)
			{
				// For splash, trace to ground first
				Vec3 vGroundStart = vTestAimPos + Vec3(0, 0, 32.0f);
				Vec3 vGroundEnd = vTestAimPos - Vec3(0, 0, 64.0f);
				H::AimUtils->Trace(vGroundStart, vGroundEnd, MASK_SOLID, &filter, &trace);

				if (trace.fraction >= 1.0f)
					continue; // No ground found, try next hitbox

				vTestAimPos = trace.endpos;

				// Check if splash would reach target
				if (vTestAimPos.DistTo(vPredictedPos) > flSplashRadius)
					continue; // Too far for splash, try next hitbox

				// Check visibility from predicted projectile position to ground impact point
				H::AimUtils->Trace(vPredictedProjectilePos, vTestAimPos, MASK_SOLID, &filter, &trace);
				if (trace.fraction < 0.99f)
					continue; // Blocked, try next hitbox
			}
			else
			{
				// Direct hit - check visibility from projectile position
				H::AimUtils->Trace(vPredictedProjectilePos, vTestAimPos, MASK_SOLID, &filter, &trace);
				if (trace.fraction < 0.99f && trace.m_pEnt != pPlayer)
					continue; // Blocked, try next hitbox
			}

			// Calculate angles
			Vec3 vTestAngles = Math::CalcAngle(vLocalPos, vTestAimPos);
			float flTestFOV = Math::CalcFov(vLocalAngles, vTestAngles);

			// Check FOV
			if (flTestFOV > flFOV)
				continue; // Outside FOV, try next hitbox

			// This hitbox works!
			vAimPos = vTestAimPos;
			vAngles = vTestAngles;
			flFOVTo = flTestFOV;
			bSplash = bTestSplash;
			bFoundValidHitbox = true;
			break;
		}

		// Skip this target if no valid hitbox found
		if (!bFoundValidHitbox)
			continue;

		AimbotTarget target;
		target.pEntity = pPlayer;
		target.vPredictedPos = vPredictedPos;
		target.vAimPos = vAimPos;
		target.vAimAngles = vAngles;
		target.flFOV = flFOVTo;
		target.flDist = vPredictedProjectilePos.DistTo(vAimPos); // Distance from predicted projectile to aim point
		target.bSplash = bSplash;
		target.vPlayerPath = vPlayerPath;

		vTargets.push_back(target);
	}

	if (vTargets.empty())
		return false;

	// Sort by FOV (closest to crosshair first)
	std::sort(vTargets.begin(), vTargets.end(), [](const AimbotTarget& a, const AimbotTarget& b) {
		return a.flFOV < b.flFOV;
		});

	// Use the best target
	const auto& bestTarget = vTargets[0];
	outAngles = bestTarget.vAimAngles;
	outPlayerPath = bestTarget.vPlayerPath;

	// Generate rocket trajectory path
	// The rocket starts from its current position (where it is when airblasted)
	// Then travels towards the aim target after deflection
	outRocketPath.clear();
	
	// Get current projectile position (where it will be deflected from)
	Vec3 vRocketStart = pProjectile->m_vecOrigin();
	outRocketPath.push_back(vRocketStart);

	// Rocket travels straight from deflect point towards aim position
	Vec3 vRocketDir = (bestTarget.vAimPos - vRocketStart).Normalized();
	float flTotalDist = vRocketStart.DistTo(bestTarget.vAimPos);
	int nRocketTicks = TIME_TO_TICKS(flTotalDist / flRocketSpeed);
	nRocketTicks = std::min(nRocketTicks, 66);

	for (int i = 1; i <= nRocketTicks; i++)
	{
		float flProgress = static_cast<float>(i) / static_cast<float>(nRocketTicks);
		Vec3 vRocketPos = vRocketStart + vRocketDir * (flTotalDist * flProgress);
		outRocketPath.push_back(vRocketPos);
	}

	return true;
}

// Arc projectile aimbot - for gravity-affected projectiles (pipes, arrows, flares, etc.)
// These projectiles don't have splash damage, so we aim directly at the predicted player position
bool CAutoAirblast::FindAimbotTargetArc(C_TFPlayer* pLocal, C_BaseProjectile* pProjectile, Vec3& outAngles,
	std::vector<Vec3>& outPlayerPath, std::vector<Vec3>& outProjectilePath)
{
	if (!pLocal || !pProjectile)
		return false;

	const Vec3 vLocalPos = pLocal->GetShootPos();
	const Vec3 vLocalAngles = I::EngineClient->GetViewAngles();
	const float flFOV = CFG::Aimbot_Projectile_FOV;
	const float flProjectileSpeed = GetReflectedProjectileSpeed(pProjectile);

	// Get latency for prediction
	const float flLatency = SDKUtils::GetLatency();

	struct AimbotTarget
	{
		C_BaseEntity* pEntity = nullptr;
		Vec3 vPredictedPos = {};
		Vec3 vAimPos = {};
		Vec3 vAimAngles = {};
		float flFOV = 0.0f;
		float flDist = 0.0f;
		std::vector<Vec3> vPlayerPath = {};
	};

	std::vector<AimbotTarget> vTargets;

	// Get the projectile's current position
	Vec3 vProjectilePos = pProjectile->m_vecOrigin();

	// Find enemy players
	for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PLAYERS_ENEMIES))
	{
		if (!pEntity)
			continue;

		const auto pPlayer = pEntity->As<C_TFPlayer>();
		if (!pPlayer || pPlayer->deadflag())
			continue;

		// Skip invulnerable targets
		if (CFG::Aimbot_Ignore_Invulnerable &&
			(pPlayer->InCond(TF_COND_INVULNERABLE) || pPlayer->InCond(TF_COND_INVULNERABLE_CARD_EFFECT) ||
				pPlayer->InCond(TF_COND_INVULNERABLE_HIDE_UNLESS_DAMAGED) || pPlayer->InCond(TF_COND_INVULNERABLE_USER_BUFF)))
			continue;

		// Skip invisible targets
		if (CFG::Aimbot_Ignore_Invisible && pPlayer->IsInvisible())
			continue;

		// Skip friends
		if (CFG::Aimbot_Ignore_Friends && pPlayer->IsPlayerOnSteamFriendsList())
			continue;

		// Use movement simulation to predict target position
		MoveStorage tStorage;
		F::MoveSim.Initialize(pPlayer, tStorage);

		Vec3 vPredictedPos = pPlayer->m_vecOrigin();
		std::vector<Vec3> vPlayerPath;
		vPlayerPath.push_back(vPredictedPos);

		// Iteratively simulate to find intersection point
		// For arc projectiles, use HORIZONTAL distance since vertical velocity changes due to gravity
		// but horizontal velocity remains constant
		int nMaxSimTicks = 66; // Cap at 1 second
		int nBestTick = 0;
		Vec3 vBestPredictedPos = vPredictedPos;
		float flBestTimeDiff = FLT_MAX;

		for (int i = 1; i <= nMaxSimTicks && !tStorage.m_bFailed; i++)
		{
			F::MoveSim.RunTick(tStorage);
			vPredictedPos = tStorage.m_vPredictedOrigin;
			vPlayerPath.push_back(vPredictedPos);

			// For arc projectiles, use horizontal distance for time calculation
			// The horizontal component of velocity is constant (no air resistance in TF2 for most projectiles)
			Vec3 vDelta = vPredictedPos - vProjectilePos;
			float flHorizDist = Vec3(vDelta.x, vDelta.y, 0).Length();
			
			// Time = horizontal distance / horizontal speed
			// For reflected projectiles, we assume they maintain their speed
			float flProjectileTime = flHorizDist / flProjectileSpeed;
			
			// Player simulation time (including latency)
			float flPlayerTime = TICKS_TO_TIME(i) + flLatency;
			
			// Find the tick where projectile arrival time best matches player simulation time
			float flTimeDiff = fabsf(flProjectileTime - flPlayerTime);
			if (flTimeDiff < flBestTimeDiff)
			{
				flBestTimeDiff = flTimeDiff;
				nBestTick = i;
				vBestPredictedPos = vPredictedPos;
			}
			
			// If projectile would arrive before player gets there, we've gone too far
			if (flProjectileTime < flPlayerTime && i > 1)
				break;
		}

		F::MoveSim.Restore(tStorage);
		
		// Use the best predicted position
		vPredictedPos = vBestPredictedPos;
		
		// Trim player path to the best tick
		if (vPlayerPath.size() > static_cast<size_t>(nBestTick + 1))
			vPlayerPath.resize(nBestTick + 1);

		// Get target eye position for aiming - aim at body center for arc projectiles
		Vec3 vTargetEye = pPlayer->GetViewOffset();
		Vec3 vAimPos = vPredictedPos + Vec3(0, 0, vTargetEye.z * 0.5f);

		// Predict where the projectile will be when the server processes our command
		Vec3 vProjectileVel;
		pProjectile->EstimateAbsVelocity(vProjectileVel);
		Vec3 vPredictedProjectilePos = vProjectilePos + vProjectileVel * flLatency;
		
		// For arc projectiles, we need to calculate the proper launch angle using ballistic equations
		// The deflection works like this:
		// 1. Server traces from player's eye in direction of viewangles -> gets hit point
		// 2. Projectile velocity direction = (hit_point - projectile_pos).Normalized()
		// 3. Gravity then affects the projectile
		//
		// So we need to find a point to AIM at such that the direction from projectile to that point
		// has the correct pitch for the ballistic trajectory to hit the target.
		
		float flGravityMult = GetProjectileGravityMult(pProjectile);
		Vec3 vAngles;
		
		if (flGravityMult > 0.0f)
		{
			// Calculate the required launch angle using ballistic formula
			Vec3 vDelta = vAimPos - vPredictedProjectilePos;
			float flHorizDist = Vec3(vDelta.x, vDelta.y, 0).Length();
			float flHeight = vDelta.z;
			
			if (flHorizDist < 1.0f)
				continue; // Too close, skip
			
			float flGrav = 800.0f * flGravityMult;
			float v2 = flProjectileSpeed * flProjectileSpeed;
			float v4 = v2 * v2;
			float x2 = flHorizDist * flHorizDist;
			
			// Ballistic formula: solve for launch angle
			float flRoot = v4 - flGrav * (flGrav * x2 + 2.0f * flHeight * v2);
			
			if (flRoot < 0.0f)
				continue; // Target is out of range
			
			flRoot = sqrtf(flRoot);
			
			// Calculate required pitch angle for projectile (low trajectory)
			float flRequiredPitch = atanf((v2 - flRoot) / (flGrav * flHorizDist));
			
			// Now we need to find a point to aim at from our EYE such that:
			// The direction from PROJECTILE to that point has pitch = flRequiredPitch
			//
			// The trace from our eye will hit something. We want that hit point to be
			// on a line from the projectile at the required pitch angle.
			//
			// Solution: Calculate a point along the required trajectory direction from the projectile,
			// then aim at that point from our eye.
			
			// Direction from projectile with required pitch
			Vec3 vHorizDir = Vec3(vDelta.x, vDelta.y, 0).Normalized();
			float flCosPitch = cosf(flRequiredPitch);
			float flSinPitch = sinf(flRequiredPitch);
			
			// Point along the trajectory (far enough that trace will likely hit it or go past)
			Vec3 vTrajectoryDir = Vec3(
				vHorizDir.x * flCosPitch,
				vHorizDir.y * flCosPitch,
				flSinPitch
			);
			
			// Aim point is along this direction from the projectile
			Vec3 vAimPoint = vPredictedProjectilePos + vTrajectoryDir * 4096.0f;
			
			// Calculate angle from our eye to this aim point
			vAngles = Math::CalcAngle(vLocalPos, vAimPoint);
			
			// Clamp pitch to valid range
			vAngles.x = std::clamp(vAngles.x, -89.0f, 89.0f);
		}
		else
		{
			// No gravity - straight line projectile, aim directly at target
			vAngles = Math::CalcAngle(vLocalPos, vAimPos);
		}
		
		float flFOVTo = Math::CalcFov(vLocalAngles, vAngles);

		// Check FOV
		if (flFOVTo > flFOV)
			continue;

		// For arc projectiles, we do a simplified visibility check
		// We check if the target is visible from our position (not the projectile)
		// because the projectile will arc over obstacles
		// We also check if there's a clear sky above the projectile (for the arc)
		CTraceFilterWorldCustom filter{};
		trace_t trace{};
		
		// Check if we can see the target from our eye position
		H::AimUtils->Trace(vLocalPos, vAimPos, MASK_SOLID, &filter, &trace);
		bool bCanSeeTarget = (trace.fraction >= 0.99f || trace.m_pEnt == pPlayer);
		
		// For high-gravity projectiles (pipes), also check if there's clearance above
		// the projectile for the arc trajectory
		bool bHasClearance = true;
		if (flGravityMult >= 0.5f)
		{
			// Check if there's space above the projectile for the arc
			// The arc peak is roughly at the midpoint, height depends on distance and gravity
			float flHorizDist = Vec3(vAimPos.x - vPredictedProjectilePos.x, vAimPos.y - vPredictedProjectilePos.y, 0).Length();
			float flTime = flHorizDist / flProjectileSpeed;
			float flArcHeight = 0.5f * 800.0f * flGravityMult * flTime * flTime * 0.25f; // Peak is at 1/4 of the drop
			
			Vec3 vMidPoint = (vPredictedProjectilePos + vAimPos) * 0.5f;
			vMidPoint.z += flArcHeight + 50.0f; // Add some buffer
			
			// Trace from projectile up to the arc peak
			H::AimUtils->Trace(vPredictedProjectilePos, vMidPoint, MASK_SOLID, &filter, &trace);
			if (trace.fraction < 0.9f)
				bHasClearance = false;
		}
		
		// Skip if we can't see the target and there's no clearance for the arc
		if (!bCanSeeTarget && !bHasClearance)
			continue;

		AimbotTarget target;
		target.pEntity = pPlayer;
		target.vPredictedPos = vPredictedPos;
		target.vAimPos = vAimPos;
		target.vAimAngles = vAngles;
		target.flFOV = flFOVTo;
		target.flDist = vPredictedProjectilePos.DistTo(vAimPos);
		target.vPlayerPath = vPlayerPath;

		vTargets.push_back(target);
	}

	if (vTargets.empty())
		return false;

	// Sort by FOV (closest to crosshair first)
	std::sort(vTargets.begin(), vTargets.end(), [](const AimbotTarget& a, const AimbotTarget& b) {
		return a.flFOV < b.flFOV;
	});

	// Use the best target
	const auto& bestTarget = vTargets[0];
	outAngles = bestTarget.vAimAngles;
	outPlayerPath = bestTarget.vPlayerPath;

	// Generate projectile trajectory path (straight line approximation for visualization)
	outProjectilePath.clear();
	
	Vec3 vProjectileStart = pProjectile->m_vecOrigin();
	outProjectilePath.push_back(vProjectileStart);

	// Projectile travels from deflect point towards aim position
	Vec3 vProjectileDir = (bestTarget.vAimPos - vProjectileStart).Normalized();
	float flTotalDist = vProjectileStart.DistTo(bestTarget.vAimPos);
	int nProjectileTicks = TIME_TO_TICKS(flTotalDist / flProjectileSpeed);
	nProjectileTicks = std::min(nProjectileTicks, 66);

	for (int i = 1; i <= nProjectileTicks; i++)
	{
		float flProgress = static_cast<float>(i) / static_cast<float>(nProjectileTicks);
		Vec3 vPos = vProjectileStart + vProjectileDir * (flTotalDist * flProgress);
		outProjectilePath.push_back(vPos);
	}

	return true;
}


bool FindTargetProjectile(C_TFPlayer* local, TargetProjectile& outTarget)
{
	for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PROJECTILES_ENEMIES))
	{
		if (!pEntity)
		{
			continue;
		}

		const auto pProjectile = pEntity->As<C_BaseProjectile>();

		if (!pProjectile)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Ignore_Rocket && pProjectile->GetClassId() == ETFClassIds::CTFProjectile_Rocket)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Ignore_SentryRocket && pProjectile->GetClassId() == ETFClassIds::CTFProjectile_SentryRocket)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Ignore_Jar && pProjectile->GetClassId() == ETFClassIds::CTFProjectile_Jar)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Ignore_JarGas && pProjectile->GetClassId() == ETFClassIds::CTFProjectile_JarGas)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Ignore_JarMilk && pProjectile->GetClassId() == ETFClassIds::CTFProjectile_JarMilk)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Ignore_Arrow && pProjectile->GetClassId() == ETFClassIds::CTFProjectile_Arrow)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Ignore_Flare && pProjectile->GetClassId() == ETFClassIds::CTFProjectile_Flare)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Ignore_Cleaver && pProjectile->GetClassId() == ETFClassIds::CTFProjectile_Cleaver)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Ignore_HealingBolt && pProjectile->GetClassId() == ETFClassIds::CTFProjectile_HealingBolt)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Ignore_PipebombProjectile && pProjectile->GetClassId() == ETFClassIds::CTFGrenadePipebombProjectile)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Ignore_BallOfFire && pProjectile->GetClassId() == ETFClassIds::CTFProjectile_BallOfFire)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Ignore_EnergyRing && pProjectile->GetClassId() == ETFClassIds::CTFProjectile_EnergyRing)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Ignore_EnergyBall && pProjectile->GetClassId() == ETFClassIds::CTFProjectile_EnergyBall)
		{
			continue;
		}

		Vec3 vel{};
		pProjectile->EstimateAbsVelocity(vel);

		if (pProjectile->GetClassId() == ETFClassIds::CTFGrenadePipebombProjectile
			&& (pProjectile->As<C_TFGrenadePipebombProjectile>()->m_bTouched()
				|| pProjectile->As<C_TFGrenadePipebombProjectile>()->m_iType() == TF_PROJECTILE_PIPEBOMB_PRACTICE))
		{
			continue;
		}

		if (pProjectile->GetClassId() == ETFClassIds::CTFProjectile_Arrow && fabsf(vel.Length()) <= 10.0f)
		{
			continue;
		}

		auto pos = pProjectile->m_vecOrigin() + (vel * SDKUtils::GetLatency());

		if (pos.DistTo(local->GetShootPos()) > 160.0f)
		{
			continue;
		}

		if (CFG::Triggerbot_AutoAirblast_Mode == 0
			&& Math::CalcFov(I::EngineClient->GetViewAngles(), Math::CalcAngle(local->GetShootPos(), pos)) > 60.0f)
		{
			continue;
		}

		CTraceFilterWorldCustom filter{};
		trace_t trace{};

		H::AimUtils->Trace(local->GetShootPos(), pos, MASK_SOLID, &filter, &trace);

		if (trace.fraction < 1.0f || trace.allsolid || trace.startsolid)
		{
			continue;
		}

		outTarget.Projectile = pProjectile;
		outTarget.Position = pos;

		return true;
	}

	return false;
}

void CAutoAirblast::Run(C_TFPlayer* pLocal, C_TFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	if (!CFG::Triggerbot_AutoAirblast_Active)
	{
		return;
	}

	if (!G::bCanSecondaryAttack || (pWeapon->GetWeaponID() != TF_WEAPON_FLAMETHROWER && pWeapon->GetWeaponID() != TF_WEAPON_FLAME_BALL)
		|| pWeapon->m_iItemDefinitionIndex() == Pyro_m_ThePhlogistinator)
	{
		return;
	}

	TargetProjectile targetProjectile{};
	if (!FindTargetProjectile(pLocal, targetProjectile))
	{
		return;
	}

	pCmd->buttons |= IN_ATTACK2;

	// Check if aimbot support is enabled and projectile supports it
	bool bUseAimbotSupport = CFG::Triggerbot_AutoAirblast_Aimbot_Support && SupportsAimbot(targetProjectile.Projectile);

	Vec3 vAimbotAngles;
	bool bFoundAimbotTarget = false;
	std::vector<Vec3> vPlayerPath;
	std::vector<Vec3> vProjectilePath;

	if (bUseAimbotSupport)
	{
		// Determine projectile type and use appropriate aimbot function
		EReflectProjectileType projectileType = GetProjectileType(targetProjectile.Projectile);
		
		if (projectileType == EReflectProjectileType::Rocket)
		{
			// Straight line projectiles (rockets) - use original function with splash support
			bFoundAimbotTarget = FindAimbotTarget(pLocal, targetProjectile.Projectile, vAimbotAngles, vPlayerPath, vProjectilePath);
		}
		else if (projectileType == EReflectProjectileType::Arc)
		{
			// Arc projectiles (pipes, arrows, flares, etc.) - direct aim, no splash
			bFoundAimbotTarget = FindAimbotTargetArc(pLocal, targetProjectile.Projectile, vAimbotAngles, vPlayerPath, vProjectilePath);
		}
	}

	// Only aim if aimbot support found a valid target with prediction
	if (bFoundAimbotTarget)
	{
		// Use aimbot-calculated angles to reflect at predicted enemy position
		pCmd->viewangles = vAimbotAngles;

		// Draw visualization if enabled
		if (Vars::Visuals::Simulation::PlayerPath.Value && !vPlayerPath.empty())
		{
			// Clear previous paths
			G::PathStorage.clear();

			// Draw player movement prediction path
			if (Vars::Colors::PlayerPathIgnoreZ.Value.a)
				G::PathStorage.emplace_back(vPlayerPath, I::GlobalVars->curtime + Vars::Visuals::Simulation::DrawDuration.Value, Vars::Colors::PlayerPathIgnoreZ.Value, Vars::Visuals::Simulation::PlayerPath.Value);
			if (Vars::Colors::PlayerPath.Value.a)
				G::PathStorage.emplace_back(vPlayerPath, I::GlobalVars->curtime + Vars::Visuals::Simulation::DrawDuration.Value, Vars::Colors::PlayerPath.Value, Vars::Visuals::Simulation::PlayerPath.Value, true);

			// Draw projectile trajectory path
			if (Vars::Visuals::Simulation::ProjectilePath.Value && !vProjectilePath.empty())
			{
				if (Vars::Colors::ProjectilePathIgnoreZ.Value.a)
					G::PathStorage.emplace_back(vProjectilePath, I::GlobalVars->curtime + Vars::Visuals::Simulation::DrawDuration.Value, Vars::Colors::ProjectilePathIgnoreZ.Value, Vars::Visuals::Simulation::ProjectilePath.Value);
				if (Vars::Colors::ProjectilePath.Value.a)
					G::PathStorage.emplace_back(vProjectilePath, I::GlobalVars->curtime + Vars::Visuals::Simulation::DrawDuration.Value, Vars::Colors::ProjectilePath.Value, Vars::Visuals::Simulation::ProjectilePath.Value, true);
			}
		}

		// Silent aim
		if (CFG::Triggerbot_AutoAirblast_Aim_Mode == 1)
		{
			G::bSilentAngles = true;
		}
	}
	// No aimbot target found - just airblast without aiming
}
