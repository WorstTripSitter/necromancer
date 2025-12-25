#include "FakeLag.h"
#include "../CFG.h"
#include "../Misc/Misc.h"
#include "../Players/Players.h"

bool CFakeLag::IsAllowed(C_TFPlayer* pLocal, C_TFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	// Don't fakelag if disabled or dead
	if (!CFG::Exploits_FakeLag_Enabled || pLocal->deadflag())
		return false;

	// Calculate max allowed fakelag ticks based on saved DT ticks
	// Total budget is ~24 ticks, shared between fakelag and doubletap
	// When anti-cheat is enabled, max is clamped to 8
	static auto sv_maxusrcmdprocessticks = I::CVar->FindVar("sv_maxusrcmdprocessticks");
	int nMaxTicks = sv_maxusrcmdprocessticks ? sv_maxusrcmdprocessticks->GetInt() : 24;
	if (CFG::Misc_AntiCheat_Enabled)
		nMaxTicks = std::min(nMaxTicks, 8);
	
	// Calculate available ticks for fakelag (total budget minus saved DT ticks)
	int nAvailableForFakelag = nMaxTicks - Shifting::nAvailableTicks;
	nAvailableForFakelag = std::min(nAvailableForFakelag, 21); // Hard cap at 21
	nAvailableForFakelag = std::min(nAvailableForFakelag, CFG::Exploits_FakeLag_Max_Ticks); // User config cap
	
	// If we've already choked enough, stop
	if (I::ClientState->chokedcommands >= nAvailableForFakelag)
		return false;

	// Don't fakelag when actively shifting/recharging
	if (Shifting::bShifting || Shifting::bRecharging)
		return false;

	// Don't fakelag during auto rocket jump
	if (F::Misc->m_bRJDisableFakeLag)
		return false;
	
	// Don't fakelag during AutoFaN (needs to send packet immediately for jump boost)
	if (F::Misc->IsAutoFaNRunning())
		return false;

	// Don't fakelag with Beggar's Bazooka while charging
	if (pWeapon && pWeapon->m_iItemDefinitionIndex() == Soldier_m_TheBeggarsBazooka 
		&& pCmd->buttons & IN_ATTACK && !(G::nOldButtons & IN_ATTACK))
		return false;

	// Only fakelag when moving (if option enabled)
	if (CFG::Exploits_FakeLag_Only_Moving)
	{
		if (pLocal->m_vecVelocity().Length2D() <= 10.f)
			return false;
	}

	// Adaptive mode: check if we should keep choking
	static auto sv_lagcompensation_teleport_dist = I::CVar->FindVar("sv_lagcompensation_teleport_dist");
	const float flMaxDist = sv_lagcompensation_teleport_dist ? sv_lagcompensation_teleport_dist->GetFloat() : 64.f;
	const float flDistSqr = (pLocal->m_vecOrigin() - m_vLastPosition).Length2DSqr();
	
	// If we've moved too far, unchoke
	if (flDistSqr >= (flMaxDist * flMaxDist))
		return false;

	// Otherwise, keep choking
	return true;
}

bool CFakeLag::IsSniperThreat(C_TFPlayer* pLocal, int& outMinTicks, int& outMaxTicks)
{
	if (!CFG::Exploits_FakeLag_Activate_On_Sightline)
		return false;

	// Default intervals (will be overridden if threat found)
	outMinTicks = 3;
	outMaxTicks = 8;

	for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PLAYERS_ENEMIES))
	{
		if (!pEntity)
			continue;

		const auto pEnemy = pEntity->As<C_TFPlayer>();
		if (!pEnemy || pEnemy->deadflag())
			continue;

		// Check if enemy is a sniper
		if (pEnemy->m_iClass() != TF_CLASS_SNIPER)
			continue;

		// Check if holding sniper rifle
		const auto pWeapon = pEnemy->m_hActiveWeapon().Get()->As<C_TFWeaponBase>();
		if (!pWeapon || pWeapon->GetSlot() != WEAPON_SLOT_PRIMARY || pWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
			continue;

		// Get player priority info
		PlayerPriority pInfo{};
		const bool bHasTag = F::Players->GetInfo(pEnemy->entindex(), pInfo);

		// === CASE 3: CHEATER TAG ===
		// If tagged as cheater, activate fakelag just for holding sniper rifle (no scope/visibility check)
		// Medium to high interval (10-18 ticks)
		if (bHasTag && pInfo.Cheater)
		{
			outMinTicks = 10;
			outMaxTicks = 18;
			return true;
		}

		// === CASE 2: RETARD LEGIT TAG ===
		// If tagged as retard legit, activate if they can see you (no scope check needed)
		// Low to high interval (2-20 ticks)
		if (bHasTag && pInfo.RetardLegit)
		{
			// Check visibility only (no scope requirement)
			const bool bVisibleFromCenter = H::AimUtils->TraceEntityAutoDet(
				pEntity,
				pLocal->GetCenter(),
				pEnemy->m_vecOrigin() + Vec3{ 0.0f, 0.0f, pEnemy->m_vecMaxs().z }
			);

			const bool bVisibleFromHead = H::AimUtils->TraceEntityAutoDet(
				pEntity,
				pLocal->m_vecOrigin() + Vec3{ 0.0f, 0.0f, pLocal->m_vecMaxs().z },
				pEnemy->m_vecOrigin() + Vec3{ 0.0f, 0.0f, pEnemy->m_vecMaxs().z }
			);

			if (bVisibleFromCenter || bVisibleFromHead)
			{
				outMinTicks = 2;
				outMaxTicks = 20;
				return true;
			}
			
			continue; // Skip default check for tagged players
		}

		// === CASE 1: DEFAULT (NO TAG) ===
		// For untagged players, check if scoped and aiming near you
		// Low interval (3-8 ticks)
		
		// Check if scoped
		bool bZoomed = pEnemy->InCond(TF_COND_ZOOMED);
		if (pWeapon->GetWeaponID() == TF_WEAPON_SNIPERRIFLE_CLASSIC)
		{
			bZoomed = pWeapon->As<C_TFSniperRifleClassic>()->m_bCharging();
		}

		if (!bZoomed)
			continue;

		// Expand bounds significantly for wider detection (~50 feet)
		auto vMins = pLocal->m_vecMins();
		auto vMaxs = pLocal->m_vecMaxs();

		vMins.x *= 6.0f;  // 6x for horizontal (very wide detection)
		vMins.y *= 6.0f;
		vMins.z *= 3.0f;  // 3x for vertical

		vMaxs.x *= 6.0f;
		vMaxs.y *= 6.0f;
		vMaxs.z *= 3.0f;

		Vec3 vForward{};
		Math::AngleVectors(pEnemy->GetEyeAngles(), &vForward);

		// Check if sniper is aiming at us
		if (!Math::RayToOBB(pEnemy->GetShootPos(), vForward, pLocal->m_vecOrigin(), vMins, vMaxs, pLocal->RenderableToWorldTransform()))
			continue;

		// Sightline detected for default player (low interval already set)
		return true;
	}

	return false;
}

void CFakeLag::Run(C_TFPlayer* pLocal, C_TFWeaponBase* pWeapon, CUserCmd* pCmd, bool* pSendPacket)
{
	// Set goal to 22 for adaptive mode
	if (!m_iGoal)
		m_iGoal = 22;
	
	// Check for new entities and force unchoke for a few ticks
	static int nLastPlayerCount = 0;
	int nCurrentPlayerCount = 0;
	for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PLAYERS_ENEMIES))
	{
		if (auto pPlayer = pEntity->As<C_TFPlayer>())
		{
			if (!pPlayer->deadflag())
				nCurrentPlayerCount++;
		}
	}
	
	if (nCurrentPlayerCount > nLastPlayerCount)
	{
		// New entity detected, force unchoke for 5 ticks to let records initialize properly
		m_iNewEntityUnchokeTicks = 5;
	}
	nLastPlayerCount = nCurrentPlayerCount;
	
	if (m_iNewEntityUnchokeTicks > 0)
	{
		m_iNewEntityUnchokeTicks--;
		*pSendPacket = true;
		m_iGoal = 0;
		m_vLastPosition = pLocal->m_vecOrigin();
		m_bEnabled = false;
		return;
	}
	
	// If "Activate on Sightline" is enabled, use sightline-based logic
	if (CFG::Exploits_FakeLag_Activate_On_Sightline)
	{
		// Still respect critical safety checks even in sightline mode
		
		// Don't fakelag if disabled or dead
		if (!CFG::Exploits_FakeLag_Enabled || pLocal->deadflag())
		{
			m_iGoal = 0;
			m_vLastPosition = pLocal->m_vecOrigin();
			m_bEnabled = false;
			*pSendPacket = true;
			return;
		}
		
		// Calculate max allowed fakelag ticks based on saved DT ticks
		static auto sv_maxusrcmdprocessticks = I::CVar->FindVar("sv_maxusrcmdprocessticks");
		int nMaxTicks = sv_maxusrcmdprocessticks ? sv_maxusrcmdprocessticks->GetInt() : 24;
		if (CFG::Misc_AntiCheat_Enabled)
			nMaxTicks = std::min(nMaxTicks, 8);
		
		int nAvailableForFakelag = nMaxTicks - Shifting::nAvailableTicks;
		nAvailableForFakelag = std::min(nAvailableForFakelag, 21);
		nAvailableForFakelag = std::min(nAvailableForFakelag, CFG::Exploits_FakeLag_Max_Ticks);
		
		if (I::ClientState->chokedcommands >= nAvailableForFakelag)
		{
			m_iGoal = 0;
			m_vLastPosition = pLocal->m_vecOrigin();
			m_bEnabled = false;
			*pSendPacket = true;
			return;
		}
		
		// Don't fakelag when shifting/recharging
		if (Shifting::bShifting || Shifting::bRecharging)
		{
			m_iGoal = 0;
			m_vLastPosition = pLocal->m_vecOrigin();
			m_bEnabled = false;
			*pSendPacket = true;
			return;
		}
		
		// Don't fakelag during auto rocket jump
		if (F::Misc->m_bRJDisableFakeLag)
		{
			m_iGoal = 0;
			m_vLastPosition = pLocal->m_vecOrigin();
			m_bEnabled = false;
			*pSendPacket = true;
			return;
		}
		
		// Don't fakelag during AutoFaN
		if (F::Misc->IsAutoFaNRunning())
		{
			m_iGoal = 0;
			m_vLastPosition = pLocal->m_vecOrigin();
			m_bEnabled = false;
			*pSendPacket = true;
			return;
		}
		
		// Now check for sniper threat
		int nMinTicks = 3;
		int nMaxTicksSightline = 8;
		if (!IsSniperThreat(pLocal, nMinTicks, nMaxTicksSightline))
		{
			m_iGoal = 0;
			m_iCurrentChokeTicks = 0;
			m_iTargetChokeTicks = 0;
			m_vLastPosition = pLocal->m_vecOrigin();
			m_bEnabled = false;
			*pSendPacket = true;
			return;
		}
		
		// Sniper threat detected and all safety checks passed, enable fakelag with random interval
		
		// Generate new random target if we've reached the current target or just started
		if (m_iCurrentChokeTicks >= m_iTargetChokeTicks || m_iTargetChokeTicks == 0)
		{
			m_iTargetChokeTicks = nMinTicks + (rand() % (nMaxTicksSightline - nMinTicks + 1));
			m_iCurrentChokeTicks = 0;
		}
		
		// Choke until we reach target
		if (m_iCurrentChokeTicks < m_iTargetChokeTicks)
		{
			*pSendPacket = false;
			m_iCurrentChokeTicks++;
		}
		else
		{
			// Reached target, send packet and reset
			*pSendPacket = true;
			m_iCurrentChokeTicks = 0;
		}
		
		m_bEnabled = true;
		return;
	}
	
	if (!IsAllowed(pLocal, pWeapon, pCmd))
	{
		m_iGoal = 0;
		m_vLastPosition = pLocal->m_vecOrigin();
		m_bEnabled = false;
		return;
	}

	*pSendPacket = false;
	m_bEnabled = true;
}
