#include "FakeLag.h"
#include "../CFG.h"
#include "../Misc/Misc.h"
#include "../Players/Players.h"
#include "../FakeAngle/FakeAngle.h"
#include "../amalgam_port/AmalgamCompat.h"

void CFakeLag::PreserveBlastJump(C_TFPlayer* pLocal)
{
	m_bPreservingBlast = false;
	
	// Skip if auto rocket jump is active
	if (F::Misc->m_bRJDisableFakeLag)
		return;
	
	if (!pLocal->IsAlive())
		return;
	
	static bool bStaticGround = true;
	const bool bLastGround = bStaticGround;
	const bool bCurrGround = bStaticGround = pLocal->m_hGroundEntity() != nullptr;
	
	if (!pLocal->InCond(TF_COND_BLASTJUMPING) || bLastGround || !bCurrGround)
		return;
	
	m_bPreservingBlast = true;
}

void CFakeLag::Unduck(C_TFPlayer* pLocal, CUserCmd* pCmd)
{
	m_bUnducking = false;
	
	if (!pLocal->IsAlive())
		return;
	
	if (!(pLocal->m_hGroundEntity() && IsDucking(pLocal) && !(pCmd->buttons & IN_DUCK)))
		return;
	
	m_bUnducking = true;
}

void CFakeLag::Prediction(C_TFPlayer* pLocal, CUserCmd* pCmd)
{
	PreserveBlastJump(pLocal);
	Unduck(pLocal, pCmd);
}

bool CFakeLag::IsAllowed(C_TFPlayer* pLocal, C_TFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	// Calculate max allowed fakelag ticks
	static auto sv_maxusrcmdprocessticks = I::CVar->FindVar("sv_maxusrcmdprocessticks");
	int nMaxTicks = sv_maxusrcmdprocessticks ? sv_maxusrcmdprocessticks->GetInt() : 24;
	if (CFG::Misc_AntiCheat_Enabled)
		nMaxTicks = std::min(nMaxTicks, 8);
	
	// Reserve ticks for anti-aim if it's active
	int nAntiAimTicks = (F::FakeAngle->YawOn() && F::FakeAngle->ShouldRun(pLocal, pWeapon, pCmd)) ? F::FakeAngle->AntiAimTicks() : 0;
	
	// Calculate max choke based on shifted ticks and anti-aim reservation
	int nMaxChoke = std::min(24 - Shifting::nAvailableTicks - nAntiAimTicks, std::min(21 - nAntiAimTicks, nMaxTicks - nAntiAimTicks));
	nMaxChoke = std::min(nMaxChoke, CFG::Exploits_FakeLag_Max_Ticks);
	
	// Check basic conditions
	if (!(CFG::Exploits_FakeLag_Enabled || m_bPreservingBlast || m_bUnducking)
		|| I::ClientState->chokedcommands >= nMaxChoke
		|| Shifting::bShifting || Shifting::bRecharging
		|| !pLocal->IsAlive())
		return false;
	
	// Preserve blast jump takes priority
	if (m_bPreservingBlast)
	{
		G::bPSilentAngles = true; // Prevent unchoking while grounded
		return true;
	}
	
	// Unchoke on attack
	if (G::Attacking == 1)
		return false;
	
	// Don't fakelag during auto rocket jump
	if (F::Misc->m_bRJDisableFakeLag)
		return false;
	
	// Don't fakelag during AutoFaN
	if (F::Misc->IsAutoFaNRunning())
		return false;
	
	// Don't fakelag with Beggar's Bazooka while charging
	if (pWeapon && pWeapon->m_iItemDefinitionIndex() == Soldier_m_TheBeggarsBazooka 
		&& pCmd->buttons & IN_ATTACK && !(G::LastUserCmd ? G::LastUserCmd->buttons & IN_ATTACK : false))
		return false;
	
	// Unduck handling
	if (m_bUnducking)
		return true;
	
	// Only fakelag when moving (if option enabled)
	if (CFG::Exploits_FakeLag_Only_Moving)
	{
		if (pLocal->m_vecVelocity().Length2D() <= 10.0f)
			return false;
	}
	
	// Adaptive mode: check teleport distance
	static auto sv_lagcompensation_teleport_dist = I::CVar->FindVar("sv_lagcompensation_teleport_dist");
	const float flMaxDist = sv_lagcompensation_teleport_dist ? sv_lagcompensation_teleport_dist->GetFloat() : 64.0f;
	const float flDistSqr = (pLocal->m_vecOrigin() - m_vLastPosition).Length2DSqr();
	
	// If we've moved too far, unchoke
	if (flDistSqr >= (flMaxDist * flMaxDist))
		return false;
	
	return true;
}

bool CFakeLag::IsSniperThreat(C_TFPlayer* pLocal, int& outMinTicks, int& outMaxTicks)
{
	if (!CFG::Exploits_FakeLag_Activate_On_Sightline)
		return false;

	outMinTicks = 3;
	outMaxTicks = 8;

	for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PLAYERS_ENEMIES))
	{
		if (!pEntity)
			continue;

		const auto pEnemy = pEntity->As<C_TFPlayer>();
		if (!pEnemy || pEnemy->deadflag())
			continue;

		if (pEnemy->m_iClass() != TF_CLASS_SNIPER)
			continue;

		const auto pWeapon = pEnemy->m_hActiveWeapon().Get()->As<C_TFWeaponBase>();
		if (!pWeapon || pWeapon->GetSlot() != WEAPON_SLOT_PRIMARY || pWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW)
			continue;

		PlayerPriority pInfo{};
		const bool bHasTag = F::Players->GetInfo(pEnemy->entindex(), pInfo);

		if (bHasTag && pInfo.Cheater)
		{
			outMinTicks = 10;
			outMaxTicks = 18;
			return true;
		}

		if (bHasTag && pInfo.RetardLegit)
		{
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
			
			continue;
		}

		bool bZoomed = pEnemy->InCond(TF_COND_ZOOMED);
		if (pWeapon->GetWeaponID() == TF_WEAPON_SNIPERRIFLE_CLASSIC)
		{
			bZoomed = pWeapon->As<C_TFSniperRifleClassic>()->m_bCharging();
		}

		if (!bZoomed)
			continue;

		auto vMins = pLocal->m_vecMins();
		auto vMaxs = pLocal->m_vecMaxs();

		vMins.x *= 6.0f;
		vMins.y *= 6.0f;
		vMins.z *= 3.0f;

		vMaxs.x *= 6.0f;
		vMaxs.y *= 6.0f;
		vMaxs.z *= 3.0f;

		Vec3 vForward{};
		Math::AngleVectors(pEnemy->GetEyeAngles(), &vForward);

		if (!Math::RayToOBB(pEnemy->GetShootPos(), vForward, pLocal->m_vecOrigin(), vMins, vMaxs, pLocal->RenderableToWorldTransform()))
			continue;

		return true;
	}

	return false;
}

void CFakeLag::Run(C_TFPlayer* pLocal, C_TFWeaponBase* pWeapon, CUserCmd* pCmd, bool* pSendPacket)
{
	// Default to sending packet
	*pSendPacket = true;
	
	// Set goal to 22 for adaptive mode
	if (!m_iGoal)
		m_iGoal = 22;
	
	// Check for new entities and force unchoke
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
		m_iNewEntityUnchokeTicks = 5;
	nLastPlayerCount = nCurrentPlayerCount;
	
	if (m_iNewEntityUnchokeTicks > 0)
	{
		m_iNewEntityUnchokeTicks--;
		m_iGoal = 0;
		m_vLastPosition = pLocal->m_vecOrigin();
		m_bEnabled = false;
		return;
	}
	
	// Run prediction (blast jump preservation, unduck)
	Prediction(pLocal, pCmd);
	
	// Sightline-based logic
	if (CFG::Exploits_FakeLag_Activate_On_Sightline)
	{
		if (!CFG::Exploits_FakeLag_Enabled || pLocal->deadflag())
		{
			m_iGoal = 0;
			m_vLastPosition = pLocal->m_vecOrigin();
			m_bEnabled = false;
			return;
		}
		
		// Calculate max allowed fakelag ticks
		static auto sv_maxusrcmdprocessticks = I::CVar->FindVar("sv_maxusrcmdprocessticks");
		int nMaxTicks = sv_maxusrcmdprocessticks ? sv_maxusrcmdprocessticks->GetInt() : 24;
		if (CFG::Misc_AntiCheat_Enabled)
			nMaxTicks = std::min(nMaxTicks, 8);
		
		// Reserve ticks for anti-aim if it's active
		int nAntiAimTicks = (F::FakeAngle->YawOn() && F::FakeAngle->ShouldRun(pLocal, pWeapon, pCmd)) ? F::FakeAngle->AntiAimTicks() : 0;
		int nAvailableForFakelag = nMaxTicks - Shifting::nAvailableTicks - nAntiAimTicks;
		nAvailableForFakelag = std::min(nAvailableForFakelag, 21 - nAntiAimTicks);
		nAvailableForFakelag = std::min(nAvailableForFakelag, CFG::Exploits_FakeLag_Max_Ticks);
		
		if (I::ClientState->chokedcommands >= nAvailableForFakelag)
		{
			m_iGoal = 0;
			m_vLastPosition = pLocal->m_vecOrigin();
			m_bEnabled = false;
			return;
		}
		
		if (Shifting::bShifting || Shifting::bRecharging)
		{
			m_iGoal = 0;
			m_vLastPosition = pLocal->m_vecOrigin();
			m_bEnabled = false;
			return;
		}
		
		if (F::Misc->m_bRJDisableFakeLag || F::Misc->IsAutoFaNRunning())
		{
			m_iGoal = 0;
			m_vLastPosition = pLocal->m_vecOrigin();
			m_bEnabled = false;
			return;
		}
		
		int nMinTicks = 3;
		int nMaxTicksSightline = 8;
		if (!IsSniperThreat(pLocal, nMinTicks, nMaxTicksSightline))
		{
			m_iGoal = 0;
			m_iCurrentChokeTicks = 0;
			m_iTargetChokeTicks = 0;
			m_vLastPosition = pLocal->m_vecOrigin();
			m_bEnabled = false;
			return;
		}
		
		if (m_iCurrentChokeTicks >= m_iTargetChokeTicks || m_iTargetChokeTicks == 0)
		{
			m_iTargetChokeTicks = nMinTicks + (rand() % (nMaxTicksSightline - nMinTicks + 1));
			m_iCurrentChokeTicks = 0;
		}
		
		if (m_iCurrentChokeTicks < m_iTargetChokeTicks)
		{
			*pSendPacket = false;
			m_iCurrentChokeTicks++;
		}
		else
		{
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

// Called after Run() to set the draw chams flag based on actual fakelag state
void CFakeLag::UpdateDrawChams()
{
	// Show fake model when fakelagging or anti-aim is active
	F::FakeAngle->m_bDrawChams = m_bEnabled || F::FakeAngle->AntiAimOn();
}
