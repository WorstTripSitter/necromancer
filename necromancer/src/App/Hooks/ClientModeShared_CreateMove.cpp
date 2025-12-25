#include "../../SDK/SDK.h"

#include "../Features/CFG.h"

#include "../Features/Aimbot/Aimbot.h"
#include "../Features/EnginePrediction/EnginePrediction.h"
#include "../Features/Misc/Misc.h"
#include "../Features/RapidFire/RapidFire.h"
#include "../Features/Triggerbot/Triggerbot.h"
#include "../Features/Triggerbot/AutoVaccinator/AutoVaccinator.h"
#include "../Features/SeedPred/SeedPred.h"
#include "../Features/Crits/Crits.h"
#include "../Features/FakeLag/FakeLag.h"
#include "../Features/FakeAngle/FakeAngle.h"
#include "../Features/ProjectileDodge/ProjectileDodge.h"
#include "../Features/Misc/AntiCheatCompat/AntiCheatCompat.h"
#include "../Features/amalgam_port/Ticks/Ticks.h"
#include "../Features/amalgam_port/AmalgamCompat.h"

MAKE_HOOK(ClientModeShared_CreateMove, Memory::GetVFunc(I::ClientModeShared, 21), bool, __fastcall,
	CClientModeShared* ecx, float flInputSampleTime, CUserCmd* pCmd)
{
	G::bSilentAngles = false;
	G::bPSilentAngles = false;
	G::bFiring = false;
	G::Attacking = 0;
	G::Throwing = false;
	G::LastUserCmd = G::CurrentUserCmd;
	G::CurrentUserCmd = pCmd;
	G::OriginalCmd = *pCmd;  // Store unmodified cmd

	if (!pCmd || !pCmd->command_number)
	{
		return CALL_ORIGINAL(ecx, flInputSampleTime, pCmd);
	}

	// Get the actual command from the input buffer - this is what gets sent to the server
	// The pCmd parameter might be a copy, so we need to modify the buffer directly
	CUserCmd* pBufferCmd = I::Input->GetUserCmd(pCmd->command_number);
	if (!pBufferCmd)
		pBufferCmd = pCmd;

	I::Prediction->Update
	(
		I::ClientState->m_nDeltaTick,
		I::ClientState->m_nDeltaTick > 0,
		I::ClientState->last_command_ack,
		I::ClientState->lastoutgoingcommand + I::ClientState->chokedcommands
	);

	F::AutoVaccinator->PreventReload(pCmd);

	// Run AutoVaccinator early if Always On - needs to run even during RapidFire/Shifting
	if (CFG::Triggerbot_AutoVaccinator_Always_On)
	{
		auto pLocalVacc = H::Entities->GetLocal();
		auto pWeaponVacc = H::Entities->GetWeapon();
		if (pLocalVacc && !pLocalVacc->deadflag() && pWeaponVacc)
		{
			F::AutoVaccinator->Run(pLocalVacc, pWeaponVacc, pCmd);
		}
	}

	if (F::RapidFire->ShouldExitCreateMove(pCmd))
	{
		// During shifting, we still need to run CritHack to apply the forced command number
		// CritHack stores the forced command on the first tick (bRapidFireWantShift)
		// and reuses it for all shifted ticks
		auto pLocal = H::Entities->GetLocal();
		auto pWeapon = H::Entities->GetWeapon();
		if (pLocal && pWeapon && !pLocal->deadflag())
		{
			// Get the buffer command - this is what actually gets sent to server
			CUserCmd* pBufferCmd = I::Input->GetUserCmd(pCmd->command_number);
			if (!pBufferCmd)
				pBufferCmd = pCmd;
			
			F::CritHack->Run(pLocal, pWeapon, pBufferCmd);
			
			// Sync changes back to pCmd
			if (pBufferCmd != pCmd)
			{
				pCmd->command_number = pBufferCmd->command_number;
				pCmd->random_seed = pBufferCmd->random_seed;
			}
		}
		
		return F::RapidFire->GetShiftSilentAngles() ? false : CALL_ORIGINAL(ecx, flInputSampleTime, pCmd);
	}

	if (Shifting::bRecharging)
	{
		if (pCmd->buttons & IN_JUMP)
		{
			pCmd->buttons &= ~IN_JUMP;
		}

		return CALL_ORIGINAL(ecx, flInputSampleTime, pCmd);
	}

	bool* pSendPacket = reinterpret_cast<bool*>(uintptr_t(_AddressOfReturnAddress()) + 0x128);

	// OPTIMIZATION: Cache angles and movement for later restoration
	const Vec3 vOldAngles = pCmd->viewangles;
	const float flOldSide = pCmd->sidemove;
	const float flOldForward = pCmd->forwardmove;

	// OPTIMIZATION: Cache entity pointers once (used multiple times below)
	auto pLocal = H::Entities->GetLocal();
	auto pWeapon = H::Entities->GetWeapon();

	// Cache weapon capabilities (used by multiple features)
	// Reset state at start of frame (Amalgam style)
	G::bFiring = false;
	G::bCanPrimaryAttack = false;
	G::bCanSecondaryAttack = false;
	G::bReloading = false;
	
	if (pLocal && pWeapon)
	{
		G::bCanHeadshot = pWeapon->CanHeadShot(pLocal);
		
		// Amalgam's CheckReload trick - forces weapon to update reload state immediately
		// This makes reload detection faster (shoot as soon as 1 bullet is loaded)
		// WEAPON_NOCLIP = -1
		if (pWeapon->GetMaxClip1() != -1 && !pWeapon->m_bReloadsSingly())
		{
			float flOldCurtime = I::GlobalVars->curtime;
			I::GlobalVars->curtime = TICKS_TO_TIME(pLocal->m_nTickBase());
			pWeapon->CheckReload();
			I::GlobalVars->curtime = flOldCurtime;
		}
		
		// Amalgam's exact logic from UpdateInfo()
		G::bCanPrimaryAttack = pWeapon->CanPrimaryAttack(pLocal);
		G::bCanSecondaryAttack = pWeapon->CanSecondaryAttack(pLocal);
		
		// Minigun special handling - matches Amalgam's UpdateInfo()
		// Minigun can only attack when in FIRING or SPINNING state
		if (pWeapon->GetWeaponID() == TF_WEAPON_MINIGUN)
		{
			int iState = pWeapon->As<C_TFMinigun>()->m_iWeaponState();
			if ((iState != AC_STATE_FIRING && iState != AC_STATE_SPINNING) || !pWeapon->HasPrimaryAmmoForShot())
				G::bCanPrimaryAttack = false;
		}
		
		// Amalgam's default case logic for non-melee weapons
		if (pWeapon->GetSlot() != WEAPON_SLOT_MELEE)
		{
			bool bAmmo = pWeapon->HasPrimaryAmmoForShot();
			bool bReload = pWeapon->IsInReload();
			
			// If no ammo, can't attack
			if (!bAmmo)
			{
				G::bCanPrimaryAttack = false;
				G::bCanSecondaryAttack = false;
			}
			
			// If reloading AND has ammo AND can't attack yet -> G::bReloading = true
			// This is the "queued attack" state
			if (bReload && bAmmo && !G::bCanPrimaryAttack)
			{
				G::bReloading = true;
			}
		}
	}
	else
	{
		G::bCanHeadshot = false;
	}

	// Set G::Attacking like Amalgam does in UpdateInfo
	// This is needed for CritHack to detect manual attacks
	if (pLocal && pWeapon)
	{
		G::Attacking = SDK::IsAttacking(pLocal, pWeapon, pCmd, false);
	}

	//nTicksSinceCanFire
	{
		static bool bOldCanFire = G::bCanPrimaryAttack;

		if (G::bCanPrimaryAttack != bOldCanFire)
		{
			G::nTicksSinceCanFire = 0;
			bOldCanFire = G::bCanPrimaryAttack;
		}

		else
		{
			if (G::bCanPrimaryAttack)
				G::nTicksSinceCanFire++;

			else G::nTicksSinceCanFire = 0;
		}
	}
	// OPTIMIZATION: Early exit if no local player (rare but possible)
	if (!pLocal)
		return CALL_ORIGINAL(ecx, flInputSampleTime, pCmd);

	// Update Amalgam Ticks - saves shoot position for projectile aimbot
	F::Ticks.SaveShootPos(reinterpret_cast<C_TFPlayer*>(pLocal));
	
	// Reset Amalgam AimType override each frame
	Vars::Aimbot::General::AimType.Reset();

	F::Misc->Bunnyhop(pCmd);
	F::Misc->AutoStrafer(pCmd);
	F::Misc->FastStop(pCmd);
	F::Misc->NoiseMakerSpam();
	F::Misc->AutoRocketJump(pCmd);
	F::Misc->AutoFaN(pCmd);
	F::Misc->AutoUber(pCmd);
	F::Misc->AutoDisguise(pCmd);
	F::Misc->MovementLock(pCmd);
	F::Misc->MvmInstaRespawn();
	F::Misc->AntiAFK(pCmd);
	
	// Projectile Dodge (run before aimbot so it can override movement)
	F::ProjectileDodge->Run(pLocal, pCmd);

	// FakeLag (run BEFORE aimbot so it can unchoke before simulation)
	if (pWeapon)
	{
		F::FakeLag->Run(pLocal, pWeapon, pCmd, pSendPacket);
	}

	// CritHack runs early for manual shooting, but we also need to run it
	// AFTER aimbot for aimbot-triggered shots

	F::EnginePrediction->Start(pLocal, pCmd);
	{
		// OPTIMIZATION: Use cached pLocal instead of getting it again
		if (CFG::Misc_Choke_On_Bhop && CFG::Misc_Bunnyhop)
		{
			if ((pLocal->m_fFlags() & FL_ONGROUND) && !(F::EnginePrediction->flags & FL_ONGROUND))
			{
				*pSendPacket = false;
			}
		}

		F::Misc->AutoMedigun(pCmd);
		F::Aimbot->Run(pCmd);
		
		// CrouchWhileAirborne runs AFTER aimbot so projectile aimbot uses correct shoot position
		// The aimbot calculates trajectories from current eye position, then we crouch afterward
		F::Misc->CrouchWhileAirborne(pCmd);
		
		// Run CritHack AFTER aimbot so it can detect aimbot-triggered attacks
		// Use pBufferCmd to modify the actual command that gets sent to the server
		F::CritHack->Run(pLocal, pWeapon, pBufferCmd);
		
		// Sync the local pCmd with buffer changes (for other features that use pCmd)
		if (pBufferCmd != pCmd)
		{
			pCmd->command_number = pBufferCmd->command_number;
			pCmd->random_seed = pBufferCmd->random_seed;
		}
		
		F::Triggerbot->Run(pCmd);
	}
	F::EnginePrediction->End(pLocal, pCmd);
	
	F::Misc->AutoCallMedic();

	F::SeedPred->AdjustAngles(pCmd);

	//nTicksTargetSame
	{
		static int nOldTargetIndex = G::nTargetIndexEarly;

		if (G::nTargetIndexEarly != nOldTargetIndex)
		{
			G::nTicksTargetSame = 0;
			nOldTargetIndex = G::nTargetIndexEarly;
		}

		else
		{
			G::nTicksTargetSame++;
		}

		if (G::nTargetIndexEarly <= 1)
			G::nTicksTargetSame = 0;
	}

	/* Taunt Slide */
	// OPTIMIZATION: Use cached pLocal
	if (CFG::Misc_Taunt_Slide && pLocal)
	{
		if (pLocal->InCond(TF_COND_TAUNTING) && pLocal->m_bAllowMoveDuringTaunt())
		{
			static float flYaw = pCmd->viewangles.y;

			if (H::Input->IsDown(CFG::Misc_Taunt_Spin_Key) && fabsf(CFG::Misc_Taunt_Spin_Speed))
			{
				float yaw{ CFG::Misc_Taunt_Spin_Speed };

				if (CFG::Misc_Taunt_Spin_Sine)
				{
					yaw = sinf(I::GlobalVars->curtime) * CFG::Misc_Taunt_Spin_Speed;
				}

				flYaw -= yaw;
				flYaw = Math::NormalizeAngle(flYaw);

				pCmd->viewangles.y = flYaw;
			}

			else
			{
				flYaw = pCmd->viewangles.y;
			}

			if (CFG::Misc_Taunt_Slide_Control)
				pCmd->viewangles.x = (pCmd->buttons & IN_BACK) ? 91.0f : (pCmd->buttons & IN_FORWARD) ? 0.0f : 90.0f;

			G::bSilentAngles = true;
		}
	}

	/* Warp */
	// OPTIMIZATION: Use cached pLocal
	if (CFG::Exploits_Warp_Exploit && CFG::Exploits_Warp_Mode == 1 && Shifting::bShiftingWarp && pLocal)
	{
		if (CFG::Exploits_Warp_Exploit == 1)
		{
			if (Shifting::nAvailableTicks <= (MAX_COMMANDS - 1))
			{
				Vec3 vAngle = {};
				Math::VectorAngles(pLocal->m_vecVelocity(), vAngle);
				pCmd->viewangles.x = 90.0f;
				pCmd->viewangles.y = vAngle.y;
				G::bSilentAngles = true;
				pCmd->sidemove = pCmd->forwardmove = 0;
			}
		}

		if (CFG::Exploits_Warp_Exploit == 2)
		{
			if (Shifting::nAvailableTicks <= 1)
			{
				Vec3 vAngle = {};
				Math::VectorAngles(pLocal->m_vecVelocity(), vAngle);
				pCmd->viewangles.x = 90.0f;
				pCmd->viewangles.y = vAngle.y;
				G::bSilentAngles = true;
				pCmd->sidemove = pCmd->forwardmove = 0;
			}
		}
	}

	// Don't choke too much
	if (I::ClientState->chokedcommands > 22)
	{
		*pSendPacket = true;
	}

	F::RapidFire->Run(pCmd, pSendPacket);

	//pSilent - choke on attack frame, restore on next frame
	{
		static bool bWasSet = false;

		if (G::bPSilentAngles)
		{
			*pSendPacket = false;
			bWasSet = true;
		}
		else
		{
			// Don't restore angles if regular silent angles are active (e.g., rocket jump)
			if (bWasSet && !G::bSilentAngles)
			{
				*pSendPacket = true;
				pCmd->viewangles = vOldAngles;
				pCmd->sidemove = flOldSide;
				pCmd->forwardmove = flOldForward;
				bWasSet = false;
			}
			else if (bWasSet && G::bSilentAngles)
			{
				bWasSet = false;
			}
		}
	}

	// Anti-Cheat Compatibility - process command history and apply protections
	// IMPORTANT: Run AFTER pSilent restoration so it sees the FINAL angles being sent to server
	// This allows it to detect and fix the snap pattern after angles are restored
	F::AntiCheatCompat->ProcessCommand(pCmd, pSendPacket);

	G::nOldButtons = pCmd->buttons;
	G::vUserCmdAngles = pCmd->viewangles;

	// For silent aim: we need to send the aim angles to server but keep local view unchanged
	// pCmd->viewangles = aim angles (sent to server)
	// vOldAngles = original angles from start of CreateMove (local view)
	if (G::bSilentAngles || G::bPSilentAngles)
	{
		// Use vOldAngles which was captured at the START of CreateMove
		// This is the player's actual view direction before aimbot modified anything
		// DO NOT use GetViewAngles() here - it returns the modified angles!
		Vec3 vRestoreAngles = vOldAngles;
		I::EngineClient->SetViewAngles(vRestoreAngles);
		I::Prediction->SetLocalViewAngles(vRestoreAngles);
		
		// Return false to skip engine's angle processing
		// pCmd->viewangles still contains aim angles which get sent to server
		return false;
	}

	return CALL_ORIGINAL(ecx, flInputSampleTime, pCmd);
}
