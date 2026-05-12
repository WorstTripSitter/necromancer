#include "RapidFire.h"

#include "../CFG.h"
#include "../Crits/Crits.h"
#include "../TickbaseManip/TickbaseManip.h"

// Check if weapon needs antiwarp in air (rapid fire weapons)
bool NeedsAirAntiwarp(C_TFWeaponBase* pWeapon)
{
	if (!pWeapon)
		return false;

	const int nWeaponID = pWeapon->GetWeaponID();
	const int nDefIndex = pWeapon->m_iItemDefinitionIndex();

	switch (nWeaponID)
	{
	case TF_WEAPON_MINIGUN:
	case TF_WEAPON_PISTOL:
	case TF_WEAPON_PISTOL_SCOUT:
	case TF_WEAPON_SMG:
		return true;
	case TF_WEAPON_SCATTERGUN:
		if (nDefIndex == Scout_m_ForceANature || nDefIndex == Scout_m_FestiveForceANature || nDefIndex == Scout_m_TheSodaPopper)
			return true;
		break;
	default:
		break;
	}

	return false;
}

bool IsRapidFireWeapon(C_TFWeaponBase* pWeapon)
{
	if (!pWeapon)
		return false;

	switch (pWeapon->GetWeaponID())
	{
	case TF_WEAPON_MINIGUN:
	case TF_WEAPON_PISTOL:
	case TF_WEAPON_PISTOL_SCOUT:
	case TF_WEAPON_SMG: return true;
	default: return false;
	}
}

bool CRapidFire::ShouldStart(C_TFPlayer* pLocal, C_TFWeaponBase* pWeapon)
{
	// Don't start if already shifting
	if (Shifting::bShifting || Shifting::bRecharging || Shifting::bShiftingWarp)
		return false;

	// Need enough ticks
	const int nNeededTicks = F::Ticks->GetOptimalDTTicks();
	if (Shifting::nAvailableTicks < nNeededTicks)
		return false;

	// Need DT key held
	if (!H::Input->IsDown(CFG::Exploits_RapidFire_Key))
		return false;

	// Weapon must be supported
	if (!IsWeaponSupported(pWeapon))
		return false;

	// Tick tracking - delay shift if we're too far ahead of server
	if (Shifting::ShouldDelayShift(F::Ticks->GetOptimalTickTrackingMode()))
		return false;

	// Projectile dodge airborne check
	if (CFG::Misc_Projectile_Dodge_Enabled && CFG::Misc_Projectile_Dodge_Disable_DT_Airborne)
	{
		if (!(pLocal->m_fFlags() & FL_ONGROUND))
			return false;
	}

	// Crit hack check
	if (!F::CritHack->ShouldAllowFire(pLocal, pWeapon, G::CurrentUserCmd))
		return false;

	// Need target and firing intent for all weapon types
	if (G::nTargetIndex <= 1 || !G::bFiring)
		return false;

	// Sticky handling
	if (IsStickyWeapon(pWeapon))
	{
		if (!G::bCanSecondaryAttack || !pWeapon->HasPrimaryAmmoForShot())
			return false;
		return true;
	}

	// Projectile handling
	if (IsProjectileWeapon(pWeapon))
	{
		if (!G::bCanPrimaryAttack || pWeapon->IsInReload())
			return false;
		
		const int nMaxClip = pWeapon->GetMaxClip1();
		if (nMaxClip > 0 && pWeapon->m_iClip1() < nMaxClip)
			return false;

		return true;
	}

	// Minigun special case
	if (pWeapon->GetWeaponID() == TF_WEAPON_MINIGUN)
	{
		const int nState = pWeapon->As<C_TFMinigun>()->m_iWeaponState();
		if (nState == AC_STATE_FIRING || nState == AC_STATE_SPINNING)
		{
			if (!pWeapon->HasPrimaryAmmoForShot())
				return false;
			if (G::nTicksTargetSame < F::Ticks->GetOptimalDelayTicks())
				return false;
			return true;
		}
		return false;
	}

	// Hitscan - need some ticks since can fire
	if (G::nTicksSinceCanFire < 1)
		return false;

	if (G::nTicksTargetSame < F::Ticks->GetOptimalDelayTicks())
		return false;

	return true;
}

void CRapidFire::Run(CUserCmd* pCmd, bool* pSendPacket)
{
	Shifting::bRapidFireWantShift = false;
	if (!pSendPacket)
		return;

	const auto pLocal = H::Entities->GetLocal();
	if (!pLocal)
		return;

	const auto pWeapon = H::Entities->GetWeapon();
	if (!pWeapon)
		return;

	if (ShouldStart(pLocal, pWeapon))
	{
		const bool bIsProjectile = IsProjectileWeapon(pWeapon);
		const bool bIsSticky = IsStickyWeapon(pWeapon);

		if (!*pSendPacket)
			return;

		Shifting::bRapidFireWantShift = true;

		m_ShiftCmd = *pCmd;
		
		if (bIsSticky)
			m_ShiftCmd.buttons &= ~IN_ATTACK;
		else
			m_ShiftCmd.buttons |= IN_ATTACK;
		
		m_bShiftSilentAngles = G::bSilentAngles;
		m_bSetCommand = false;
		m_bIsProjectileDT = bIsProjectile;
		m_bIsStickyDT = bIsSticky;

		m_vShiftStart = pLocal->m_vecOrigin();
		m_bStartedShiftOnGround = pLocal->m_fFlags() & FL_ONGROUND;

		// Capture velocity at shift start for antiwarp
		m_vShiftVelocity = pLocal->m_vecVelocity();
		m_nShiftTick = 0;
		m_nShiftTotal = F::Ticks->GetOptimalDTTicks();

		pCmd->buttons &= ~IN_ATTACK;

		if (pWeapon->GetWeaponID() == TF_WEAPON_MINIGUN)
			pCmd->buttons |= IN_ATTACK2;

	}
}

bool CRapidFire::ShouldExitCreateMove(CUserCmd* pCmd)
{
	const auto pLocal = H::Entities->GetLocal();
	if (!pLocal)
		return false;

	if (Shifting::bShifting && !Shifting::bShiftingWarp)
	{
		m_ShiftCmd.command_number = pCmd->command_number;
		*pCmd = m_ShiftCmd;

		if (m_bIsStickyDT)
		{
			if (!m_bSetCommand)
			{
				pCmd->buttons |= IN_ATTACK;
				m_bSetCommand = true;
			}
			else
			{
				pCmd->buttons &= ~IN_ATTACK;
			}
		}
		else if (m_bIsProjectileDT)
		{
			if (!m_bSetCommand)
			{
				m_bSetCommand = true;
			}
			else
			{
				pCmd->buttons &= ~IN_ATTACK;
			}
		}
		else
		{
			m_bSetCommand = true;
		}

		// Antiwarp — prevent positional warp during tick shifts while maintaining velocity
		// When the server processes N shifted commands at once, the player "warps" forward.
		// Antiwarp modifies movement inputs so the net displacement is minimal and the
		// player ends the shift at their original speed.
		//
		// 3-phase approach (remaining ticks counts down as shift progresses):
		// Phase 1 (high remaining, start of shift): reverse velocity direction
		//   - Source acceleration means the player DECELERATES, doesn't instantly go backward
		// Phase 2 (middle ticks): zero movement — friction coasts the player down
		// Phase 3 (last 3 ticks): maintain original velocity — restores speed
		if (CFG::Exploits_RapidFire_Antiwarp)
		{
			const auto pWeapon = H::Entities->GetWeapon();

			if (m_bStartedShiftOnGround || NeedsAirAntiwarp(pWeapon))
			{
				const float flSpeed = m_vShiftVelocity.Length2D();

				if (flSpeed > 0.0f)
				{
					// Convert velocity direction to forwardmove/sidemove (same math as WalkTo)
					Vec3 vVelDir;
					Math::VectorAngles(m_vShiftVelocity, vVelDir);
					const float flYawDelta = DEG2RAD(vVelDir.y - pCmd->viewangles.y);
					const float flFwd = std::cosf(flYawDelta) * flSpeed;
					const float flSide = -std::sinf(flYawDelta) * flSpeed;

					const int iRemaining = m_nShiftTotal - m_nShiftTick;
					const int iThreshold = std::max(m_nShiftTotal - 8, 3);

					if (iRemaining > iThreshold)
					{
						// Phase 1: reverse velocity to counteract warp
						pCmd->forwardmove = -flFwd;
						pCmd->sidemove = -flSide;
					}
					else if (iRemaining > 3)
					{
						// Phase 2: zero movement — coast
						pCmd->forwardmove = 0.0f;
						pCmd->sidemove = 0.0f;
					}
					else
					{
						// Phase 3: maintain original velocity
						pCmd->forwardmove = flFwd;
						pCmd->sidemove = flSide;
					}
				}
				else
				{
					pCmd->forwardmove = 0.0f;
					pCmd->sidemove = 0.0f;
				}
			}
			// Air antiwarp not needed for this weapon/shift state
		}

		m_nShiftTick++;

		return true;
	}

	return false;
}

bool CRapidFire::IsWeaponSupported(C_TFWeaponBase* pWeapon)
{
	const auto nWeaponType = H::AimUtils->GetWeaponType(pWeapon);

	if (nWeaponType == EWeaponType::MELEE || nWeaponType == EWeaponType::OTHER)
		return false;

	const auto nWeaponID = pWeapon->GetWeaponID();

	if (nWeaponID == TF_WEAPON_COMPOUND_BOW
		|| nWeaponID == TF_WEAPON_SNIPERRIFLE_CLASSIC
		|| nWeaponID == TF_WEAPON_FLAMETHROWER
		|| pWeapon->m_iItemDefinitionIndex() == Soldier_m_TheBeggarsBazooka)
		return false;

	return true;
}

bool CRapidFire::IsStickyWeapon(C_TFWeaponBase* pWeapon)
{
	if (!pWeapon)
		return false;

	const auto nWeaponID = pWeapon->GetWeaponID();
	return nWeaponID == TF_WEAPON_PIPEBOMBLAUNCHER || nWeaponID == TF_WEAPON_CANNON;
}

bool CRapidFire::IsProjectileWeapon(C_TFWeaponBase* pWeapon)
{
	if (!pWeapon)
		return false;

	const auto nWeaponID = pWeapon->GetWeaponID();

	switch (nWeaponID)
	{
	case TF_WEAPON_ROCKETLAUNCHER:
	case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
	case TF_WEAPON_PARTICLE_CANNON:
	case TF_WEAPON_GRENADELAUNCHER:
	case TF_WEAPON_FLAREGUN:
	case TF_WEAPON_FLAREGUN_REVENGE:
	case TF_WEAPON_CROSSBOW:
	case TF_WEAPON_SHOTGUN_BUILDING_RESCUE:
	case TF_WEAPON_SYRINGEGUN_MEDIC:
	case TF_WEAPON_RAYGUN:
	case TF_WEAPON_DRG_POMSON:
		return true;
	default:
		return false;
	}
}

int CRapidFire::GetTicks(C_TFWeaponBase* pWeapon)
{
	if (!pWeapon)
		pWeapon = H::Entities->GetWeapon();

	if (!pWeapon || !IsWeaponSupported(pWeapon))
		return 0;

	if (Shifting::bShifting || Shifting::bRecharging || Shifting::bShiftingWarp)
		return 0;

	const int nNeededTicks = F::Ticks->GetOptimalDTTicks();
	if (Shifting::nAvailableTicks < nNeededTicks)
		return 0;

	if (IsStickyWeapon(pWeapon))
	{
		if (!G::bCanSecondaryAttack)
			return 0;
		if (!pWeapon->HasPrimaryAmmoForShot())
			return 0;
		return std::min(nNeededTicks, Shifting::nAvailableTicks);
	}

	if (IsProjectileWeapon(pWeapon))
	{
		if (!G::bCanPrimaryAttack)
			return 0;
		if (pWeapon->IsInReload())
			return 0;
		const int nMaxClip = pWeapon->GetMaxClip1();
		if (nMaxClip > 0 && pWeapon->m_iClip1() < nMaxClip)
			return 0;
		return std::min(nNeededTicks, Shifting::nAvailableTicks);
	}

	if (G::nTicksSinceCanFire < 1)
		return 0;

	return std::min(nNeededTicks, Shifting::nAvailableTicks);
}

// ============================================
// FAST STICKY SHOOTING
// ============================================

int CRapidFire::GetFastStickyMaxRecharge()
{
	if (CFG::Misc_AntiCheat_Enabled && !CFG::Misc_AntiCheat_IgnoreTickLimit)
		return 8;

	const int nUserLimit = std::clamp(CFG::Exploits_Shifting_Recharge_Limit, 2, 24);
	return nUserLimit;
}

bool CRapidFire::IsFastStickyUsable()
{
	if (CFG::Misc_AntiCheat_Enabled && !CFG::Misc_AntiCheat_IgnoreTickLimit)
		return false;

	if (CFG::Exploits_Shifting_Recharge_Limit <= 12)
		return false;

	return true;
}

bool CRapidFire::ShouldStartFastSticky(C_TFPlayer* pLocal, C_TFWeaponBase* pWeapon)
{
	if (Shifting::bShifting || Shifting::bShiftingWarp)
		return false;

	if (Shifting::nAvailableTicks < 1)
		return false;

	return true;
}

void CRapidFire::RunFastSticky(CUserCmd* pCmd, bool* pSendPacket)
{
	if (!pSendPacket)
		return;

	const auto pLocal = H::Entities->GetLocal();
	if (!pLocal || pLocal->deadflag())
	{
		m_bStickyCharging = false;
		return;
	}

	const auto pWeapon = H::Entities->GetWeapon();
	if (!pWeapon || !IsStickyWeapon(pWeapon))
	{
		m_bStickyCharging = false;
		return;
	}

	if (I::MatSystemSurface->IsCursorVisible() || I::EngineVGui->IsGameUIVisible())
	{
		m_bStickyCharging = false;
		return;
	}

	if (Shifting::bShifting || Shifting::bShiftingWarp)
		return;

	const bool bKeyDown = H::Input->IsDown(CFG::Exploits_FastSticky_Key);

	if (!bKeyDown)
	{
		m_bStickyCharging = false;
		return;
	}

	if (!IsFastStickyUsable())
	{
		m_bStickyCharging = false;
		return;
	}

	m_bStickyCharging = true;

	const bool bCanFire = G::bCanSecondaryAttack && pWeapon->HasPrimaryAmmoForShot();
	const int nMaxRecharge = GetFastStickyMaxRecharge();
	const int nTargetTicks = 21;

	if (!bCanFire)
	{
		const int nRechargeTarget = std::min(nTargetTicks, nMaxRecharge);
		if (Shifting::nAvailableTicks < nRechargeTarget)
			Shifting::bRecharging = true;
		return;
	}

	int nTicksToUse = 0;

	if (Shifting::nAvailableTicks >= nTargetTicks)
		nTicksToUse = nTargetTicks;
	else if (Shifting::nAvailableTicks >= 1)
		nTicksToUse = Shifting::nAvailableTicks;

	if (nTicksToUse >= 1)
	{
		if (!*pSendPacket)
			return;

		Shifting::bRecharging = false;
		pCmd->buttons |= IN_ATTACK;

		Shifting::bStickyDTWantShift = true;
		Shifting::nStickyDTTicksToUse = nTicksToUse;

		m_ShiftCmd = *pCmd;
		m_ShiftCmd.buttons |= IN_ATTACK;

		m_bShiftSilentAngles = G::bSilentAngles;
		m_bSetCommand = false;
		m_bIsProjectileDT = false;
		m_bIsStickyDT = true;

		m_vShiftStart = pLocal->m_vecOrigin();
		m_bStartedShiftOnGround = pLocal->m_fFlags() & FL_ONGROUND;

		// Capture velocity at shift start for antiwarp
		m_vShiftVelocity = pLocal->m_vecVelocity();
		m_nShiftTick = 0;
		m_nShiftTotal = nTicksToUse;

	}
	else
	{
		const int nRechargeTarget = std::min(nTargetTicks, nMaxRecharge);
		if (Shifting::nAvailableTicks < nRechargeTarget)
			Shifting::bRecharging = true;
	}
}
