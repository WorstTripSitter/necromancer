#include "AimUtils.h"

#include "../../SDK.h"

void CAimUtils::Trace(const Vec3 &start, const Vec3 &end, unsigned int mask, CTraceFilter *filter, trace_t *trace)
{
	Ray_t ray = {};
	ray.Init(start, end);
	I::EngineTrace->TraceRay(ray, mask, filter, trace);
}

void CAimUtils::TraceHull(const Vec3 &start, const Vec3 &end, const Vec3 &mins, const Vec3 &maxs, unsigned int mask, CTraceFilter *filter, trace_t *trace)
{
	Ray_t ray = {};
	ray.Init(start, end, mins, maxs);
	I::EngineTrace->TraceRay(ray, mask, filter, trace);
}

bool CAimUtils::TraceEntityBullet(C_BaseEntity *pEntity, const Vec3 &vFrom, const Vec3 &vTo, int *pHitHitboxOut)
{
	trace_t trace = {};
	CTraceFilterHitscan filter = {};

	Trace(vFrom, vTo, (MASK_SHOT | CONTENTS_GRATE), &filter, &trace);

	if (trace.m_pEnt == pEntity && !trace.allsolid && !trace.startsolid)
	{
		if (pHitHitboxOut)
			*pHitHitboxOut = trace.hitbox;

		return true;
	}

	return false;
}

bool CAimUtils::TraceEntityBulletDirect(C_BaseEntity *pEntity, const Vec3 &vFrom, const Vec3 &vTo, int *pHitHitboxOut)
{
	// Bypasses the spatial partition which can be stale during EnginePrediction.
	// ClipRayToEntity tests the ray directly against the entity's hitboxes.
	// We also need a world-only trace to check for walls blocking the shot.

	Ray_t ray = {};
	ray.Init(vFrom, vTo);

	// Step 1: ClipRayToEntity — directly test hitboxes, bypasses spatial partition
	trace_t clipTrace = {};
	I::EngineTrace->ClipRayToEntity(ray, (MASK_SHOT | CONTENTS_GRATE), pEntity, &clipTrace);

	if (clipTrace.fraction >= 1.0f || clipTrace.allsolid || clipTrace.startsolid)
		return false;

	// Step 2: World-only trace — check if a wall blocks the shot before the entity
	trace_t worldTrace = {};
	CTraceFilterWorldOnly worldFilter = {};
	I::EngineTrace->TraceRay(ray, (MASK_SHOT | CONTENTS_GRATE), &worldFilter, &worldTrace);

	// If a wall is closer than the entity hit, the shot is blocked
	if (worldTrace.fraction < clipTrace.fraction)
		return false;

	if (pHitHitboxOut)
		*pHitHitboxOut = clipTrace.hitbox;

	return true;
}

bool CAimUtils::TraceEntityAutoDet(C_BaseEntity *pEntity, const Vec3 &vFrom, const Vec3 &vTo)
{
	trace_t trace = {};
	CTraceFilterWorldCustom filter = {};
	filter.m_pTarget = pEntity;
	Trace(vFrom, vTo, MASK_SOLID, &filter, &trace);
	return trace.m_pEnt == pEntity || trace.fraction > 0.99f;
}

bool CAimUtils::TraceProjectile(C_BaseEntity *pEntity, const Vec3 &vFrom, const Vec3 &vTo)
{
	trace_t trace = {};
	CTraceFilterWorldCustom filter = {};
	filter.m_pTarget = pEntity;

	TraceHull(vFrom, vTo, { -4.0f, -4.0f, -4.0f }, { 4.0f, 4.0f, 4.0f }, MASK_SOLID, &filter, &trace);

	return trace.m_pEnt == pEntity || (trace.fraction > 0.99f && !trace.allsolid && !trace.startsolid);
}

bool CAimUtils::TraceProjectilePipes(const Vec3 &vFrom, const Vec3 &vTo, C_BaseEntity *pTarget, bool *pHitTarget)
{
	trace_t Trace = {};
	CTraceFilterWorldCustom Filter = {};
	Filter.m_pTarget = pTarget;

	TraceHull(vFrom, vTo, { -8.0f, -8.0f, -8.0f }, { 8.0f, 8.0f, 8.0f }, MASK_SOLID, &Filter, &Trace);

	if (Trace.m_pEnt == pTarget) {
		*pHitTarget = true;
		return false;
	}

	return Trace.fraction > 0.99f && !Trace.startsolid && !Trace.allsolid;
}

bool CAimUtils::TraceFlames(C_BaseEntity *pEntity, const Vec3 &vFrom, const Vec3 &vTo)
{
	trace_t trace = {};
	CTraceFilterWorldCustom filter = {};
	filter.m_pTarget = pEntity;

	TraceHull(vFrom, vTo, { -12.0f, -12.0f, -12.0f }, { 12.0f, 12.0f, 12.0f }, MASK_SOLID, &filter, &trace);

	return trace.m_pEnt == pEntity || (trace.fraction > 0.99f && !trace.allsolid && !trace.startsolid);
}

bool CAimUtils::TraceEntityMelee(C_BaseEntity *pEntity, const Vec3 &vFrom, const Vec3 &vTo)
{
	trace_t Trace = {};
	CTraceFilterHitscan Filter = {};

	TraceHull(vFrom, vTo, { -18.0f, -18.0f, -18.0f }, { 18.0f, 18.0f, 18.0f }, MASK_SOLID, &Filter, &Trace);

	return Trace.m_pEnt == pEntity;
}

bool CAimUtils::TraceEntityMeleeDirect(C_BaseEntity *pEntity, const Vec3 &vFrom, const Vec3 &vTo)
{
	return TraceEntityMeleeDirect(pEntity, vFrom, vTo, { -18.0f, -18.0f, -18.0f }, { 18.0f, 18.0f, 18.0f });
}

bool CAimUtils::TraceEntityMeleeDirect(C_BaseEntity *pEntity, const Vec3 &vFrom, const Vec3 &vTo, const Vec3 &vMins, const Vec3 &vMaxs)
{
	// Bypasses the spatial partition which can be stale during EnginePrediction.
	// Matches server's DoSwingTraceInternal: point trace first, then hull trace if point misses.

	// Step 1a: Point trace via ClipRayToEntity
	Ray_t pointRay = {};
	pointRay.Init(vFrom, vTo);
	trace_t pointTrace = {};
	I::EngineTrace->ClipRayToEntity(pointRay, MASK_SOLID, pEntity, &pointTrace);

	bool bHit = (pointTrace.fraction < 1.0f && !pointTrace.allsolid && !pointTrace.startsolid);

	// Step 1b: Hull trace via ClipRayToEntity (only if point missed)
	if (!bHit)
	{
		Ray_t hullRay = {};
		hullRay.Init(vFrom, vTo, vMins, vMaxs);
		trace_t hullTrace = {};
		I::EngineTrace->ClipRayToEntity(hullRay, MASK_SOLID, pEntity, &hullTrace);
		bHit = (hullTrace.fraction < 1.0f && !hullTrace.allsolid && !hullTrace.startsolid);
	}

	if (!bHit)
		return false;

	// Step 2: World-only trace — check if a wall blocks the swing before the entity
	Ray_t worldRay = {};
	worldRay.Init(vFrom, vTo, vMins, vMaxs);
	trace_t worldTrace = {};
	CTraceFilterWorldOnly worldFilter = {};
	I::EngineTrace->TraceRay(worldRay, MASK_SOLID, &worldFilter, &worldTrace);

	// If a wall is closer than the entity hit, the swing is blocked
	if (worldTrace.fraction < 0.99f)
		return false;

	return true;
}

bool CAimUtils::TracePositionWorld(const Vec3 &vFrom, const Vec3 &vTo)
{
	trace_t trace = {};
	CTraceFilterWorldCustom filter = {};

	Trace(vFrom, vTo, MASK_SOLID, &filter, &trace);

	return trace.fraction > 0.99f && !trace.allsolid && !trace.startsolid;
}

EWeaponType CAimUtils::GetWeaponType(C_TFWeaponBase *pWeapon)
{
	if (!pWeapon)
	{
		return EWeaponType::OTHER;
	}

	// Validate the weapon pointer is still in the entity list
	// Without this, a dangling pointer (from class change, death, etc.) passes the null check
	// but crashes on vtable calls like GetSlot() reading 0xffffffffffffffff
	if (!H::Entities->IsEntityValid(pWeapon))
	{
		return EWeaponType::OTHER;
	}

	if (pWeapon->GetSlot() == WEAPON_SLOT_MELEE)
	{
		return EWeaponType::MELEE;
	}

	switch (pWeapon->GetWeaponID())
	{
		case TF_WEAPON_ROCKETLAUNCHER:
		case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
		case TF_WEAPON_GRENADELAUNCHER:
		case TF_WEAPON_PIPEBOMBLAUNCHER:
		case TF_WEAPON_FLAREGUN:
		case TF_WEAPON_COMPOUND_BOW:
		case TF_WEAPON_CROSSBOW:
		case TF_WEAPON_PARTICLE_CANNON:
		case TF_WEAPON_DRG_POMSON:
		case TF_WEAPON_RAYGUN:
		case TF_WEAPON_FLAREGUN_REVENGE:
		case TF_WEAPON_CANNON:
		case TF_WEAPON_SYRINGEGUN_MEDIC:
		case TF_WEAPON_FLAME_BALL:
		case TF_WEAPON_FLAMETHROWER:
		case TF_WEAPON_SHOTGUN_BUILDING_RESCUE:
		{
			return EWeaponType::PROJECTILE;
		}

		case TF_WEAPON_BUILDER:
		{
			return EWeaponType::MELEE; // Sapper uses melee aimbot
		}

		case TF_WEAPON_ROCKETPACK:
		case TF_WEAPON_MEDIGUN:
		{
			return EWeaponType::OTHER;
		}

		default:
		{
			int nDamageType = pWeapon->GetDamageType();

			if (nDamageType & DMG_BULLET || nDamageType & DMG_BUCKSHOT)
			{
				return EWeaponType::HITSCAN;
			}

			break;
		}
	}

	return EWeaponType::OTHER;
}

#pragma warning (push)
#pragma warning (disable : 4244)
#pragma warning (disable : 26451)

// Two-argument version that takes both current and target angles
// This properly handles out-of-bounds pitch (>90 degrees) for anti-aim
void CAimUtils::FixMovement(CUserCmd *pCmd, const Vec3 &vCurAngle, const Vec3 &vTargetAngle)
{
	// Check if pitch is out of bounds (>90 degrees) - happens with anti-aim exploit pitches
	bool bCurOOB = fabsf(Math::NormalizeAngle(vCurAngle.x)) > 90.f;
	bool bTargetOOB = fabsf(Math::NormalizeAngle(vTargetAngle.x)) > 90.f;

	// When pitch is OOB, sidemove is inverted
	Vec3 vMove = { pCmd->forwardmove, pCmd->sidemove * (bCurOOB ? -1.f : 1.f), pCmd->upmove };
	float flSpeed = vMove.Length2D();
	
	Vec3 vMoveAng = {};
	Math::VectorAngles(vMove, vMoveAng);

	// When pitch is OOB, yaw is effectively rotated 180 degrees
	float flCurYaw = vCurAngle.y + (bCurOOB ? 180.f : 0.f);
	float flTargetYaw = vTargetAngle.y + (bTargetOOB ? 180.f : 0.f);
	float flYaw = DEG2RAD(flTargetYaw - flCurYaw + vMoveAng.y);

	pCmd->forwardmove = cos(flYaw) * flSpeed;
	pCmd->sidemove = sin(flYaw) * flSpeed * (bTargetOOB ? -1.f : 1.f);
}

// Single-argument version uses current cmd viewangles as the "from" angle
void CAimUtils::FixMovement(CUserCmd *pCmd, const Vec3 &vTargetAngle)
{
	FixMovement(pCmd, pCmd->viewangles, vTargetAngle);
}

#pragma warning (pop)

bool CAimUtils::IsWeaponCapableOfHeadshot(C_TFWeaponBase *pWeapon)
{
	auto pOwner = pWeapon->m_hOwnerEntity().Get();

	if (!pOwner)
		return false;

	bool bMaybe = false, bIsSniperRifle = false, bIsRevolver = false;

	switch (pWeapon->GetWeaponID())
	{
		case TF_WEAPON_COMPOUND_BOW: return true;
		case TF_WEAPON_SNIPERRIFLE:
		case TF_WEAPON_SNIPERRIFLE_CLASSIC:
		case TF_WEAPON_SNIPERRIFLE_DECAP: bMaybe = bIsSniperRifle = pOwner->As<C_TFPlayer>()->IsZoomed(); break;
		case TF_WEAPON_REVOLVER: bMaybe = bIsRevolver = true; break;
		default: break;
	}

	if (bMaybe)
	{
		int nWeaponMode = static_cast<int>(SDKUtils::AttribHookValue(0.0f, "set_weapon_mode", pWeapon));

		if (bIsSniperRifle)
			return nWeaponMode != 1;

		else if (bIsRevolver)
			return nWeaponMode == 1;
	}

	return false;
}

void CAimUtils::GetProjectileFireSetup(const Vec3 &vViewAngles, Vec3 vOffset, Vec3 *vSrc)
{
	static ConVar *cl_flipviewmodels = I::CVar->FindVar("cl_flipviewmodels");

	if (cl_flipviewmodels && cl_flipviewmodels->GetInt())
		vOffset.y *= -1.0f;

	Vec3 vForward = {}, vRight = {}, vUp = {};
	Math::AngleVectors(vViewAngles, &vForward, &vRight, &vUp);

	*vSrc += (vForward * vOffset.x) + (vRight * vOffset.y) + (vUp * vOffset.z);
}

bool CAimUtils::IsBehindAndFacingTarget(const Vec3 &vPlayerCenter, const Vec3 &vTargetCenter, const Vec3 &vPlayerViewAngles, const Vec3 &vTargetEyeAngles)
{
	Vec3 vToTarget = {};
	vToTarget = vTargetCenter - vPlayerCenter;
	vToTarget.z = 0.0f;
	vToTarget.NormalizeInPlace();

	Vec3 vPlayerForward = {};
	Math::AngleVectors(vPlayerViewAngles, &vPlayerForward);
	vPlayerForward.z = 0.0f;
	vPlayerForward.NormalizeInPlace();

	Vec3 vTargetForward = {};
	Math::AngleVectors(vTargetEyeAngles, &vTargetForward);
	vTargetForward.z = 0.0f;
	vTargetForward.NormalizeInPlace();

	float flPosVsTargetViewDot = vToTarget.Dot(vTargetForward);
	float flPosVsOwnerViewDot = vToTarget.Dot(vPlayerForward);
	float flViewAnglesDot = vTargetForward.Dot(vPlayerForward);

	return flPosVsTargetViewDot > 0.0f && flPosVsOwnerViewDot > 0.5f && flViewAnglesDot > -0.3f;
}