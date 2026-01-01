#include "Aimbot.h"

#include "AimbotHitscan/AimbotHitscan.h"
#include "AimbotMelee/AimbotMelee.h"
#include "AimbotWrangler/AimbotWrangler.h"
#include "../amalgam_port/AimbotProjectile/AimbotProjectile.h"
#include "AimbotProjectileArc/AimbotProjectileArc.h"
#include "../RapidFire/RapidFire.h"

#include "../CFG.h"
#include "../Misc/Misc.h"

void CAimbot::RunMain(CUserCmd* pCmd)
{
	G::nTargetIndex = -1;
	G::flAimbotFOV = 0.0f;
	G::nTargetIndexEarly = -1;

	// Handle switch key for head/body toggle
	F::AimbotHitscan->HandleSwitchKey();

	if (!CFG::Aimbot_Active || I::EngineVGui->IsGameUIVisible() || I::MatSystemSurface->IsCursorVisible() || SDKUtils::BInEndOfMatch())
		return;

	if (Shifting::bRecharging)
		return;
	
	// Skip aimbot when AutoFaN is running (it needs to control viewangles for the jump boost)
	if (F::Misc->IsAutoFaNRunning())
		return;

	const auto pLocal = H::Entities->GetLocal();
	const auto pWeapon = H::Entities->GetWeapon();

	if (!pLocal || !pWeapon
		|| pLocal->deadflag()
		|| pLocal->InCond(TF_COND_TAUNTING) || pLocal->InCond(TF_COND_PHASE)
		|| pLocal->InCond(TF_COND_HALLOWEEN_GHOST_MODE)
		|| pLocal->InCond(TF_COND_HALLOWEEN_BOMB_HEAD)
		|| pLocal->InCond(TF_COND_HALLOWEEN_KART)
		|| pLocal->m_bFeignDeathReady() || pLocal->m_flInvisibility() > 0.0f
		|| pWeapon->m_iItemDefinitionIndex() == Soldier_m_RocketJumper || pWeapon->m_iItemDefinitionIndex() == Demoman_s_StickyJumper)
		return;

	// Wrangler gets special handling - it controls the sentry gun
	if (F::AimbotWrangler->IsWrangler(pWeapon))
	{
		F::AimbotWrangler->Run(pCmd, pLocal, pWeapon);
		return;
	}

	switch (H::AimUtils->GetWeaponType(pWeapon))
	{
		case EWeaponType::HITSCAN:
		{
			F::AimbotHitscan->Run(pCmd, pLocal, pWeapon);
			break;
		}

		case EWeaponType::PROJECTILE:
		{
			if (CFG::Aimbot_Amalgam_Projectile_Active)
			{
				// All arc weapons (pipes, stickies, huntsman, etc.) -> arc aimbot
				// Rockets, etc. -> Amalgam projectile aimbot
				if (CAimbotProjectileArc::IsArcWeapon(pWeapon))
					F::AimbotProjectileArc->Run(pCmd, pLocal, pWeapon);
				else
					F::AimbotProjectile->Run(pLocal, pWeapon, pCmd);
			}
			break;
		}

		case EWeaponType::MELEE:
		{
			F::AimbotMelee->Run(pCmd, pLocal, pWeapon);
			break;
		}

		default: break;
	}
}

void CAimbot::Run(CUserCmd* pCmd)
{
	RunMain(pCmd);

	//same-ish code below to see if we are firing manually

	const auto pLocal = H::Entities->GetLocal();
	const auto pWeapon = H::Entities->GetWeapon();

	if (!pLocal || !pWeapon
		|| pLocal->deadflag()
		|| pLocal->InCond(TF_COND_TAUNTING) || pLocal->InCond(TF_COND_PHASE)
		|| pLocal->m_bFeignDeathReady() || pLocal->m_flInvisibility() > 0.0f)
		return;

	const auto nWeaponType = H::AimUtils->GetWeaponType(pWeapon);

	if (!G::bFiring)
	{
		switch (nWeaponType)
		{
			case EWeaponType::HITSCAN:
			{
				G::bFiring = F::AimbotHitscan->IsFiring(pCmd, pWeapon);
				break;
			}

			case EWeaponType::PROJECTILE:
			{
				// Route firing detection based on weapon type
				if (CAimbotProjectileArc::IsArcWeapon(pWeapon))
					G::bFiring = F::AimbotProjectileArc->IsFiring(pCmd, pLocal, pWeapon);
				else
				{
					// For doubletap to work, we need G::bFiring to be true when:
					// 1. We're pressing attack AND can fire normally, OR
					// 2. We have a valid aimbot target AND pressing attack AND have DT ticks available
					// This allows RapidFire to trigger even when weapon is on cooldown
					bool bNormalFiring = (pCmd->buttons & IN_ATTACK) && G::bCanPrimaryAttack;
					bool bDTFiring = (pCmd->buttons & IN_ATTACK) && G::nTargetIndex > 1 && 
						F::RapidFire->IsWeaponSupported(pWeapon) && Shifting::nAvailableTicks >= CFG::Exploits_RapidFire_Ticks;
					G::bFiring = bNormalFiring || bDTFiring;
				}
				break;
			}

			case EWeaponType::MELEE:
			{
				G::bFiring = F::AimbotMelee->IsFiring(pCmd, pWeapon);
				break;
			}

			default: break;
		}
	}

}
