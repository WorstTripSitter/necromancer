#include "FakeLagFix.h"
#include "../CFG.h"
#include "../Misc/AntiCheatCompat/AntiCheatCompat.h"

void CFakeLagFix::Update()
{
	const float flTickInterval = I::GlobalVars->interval_per_tick;

	// Iterate all possible player slots
	for (int i = 1; i < FAKELAG_MAX_SLOTS; i++)
	{
		const auto pEntity = I::ClientEntityList->GetClientEntity(i);
		auto& data = m_ChokeData[i];

		// Not a valid player - clear slot
		if (!pEntity || pEntity->GetClassId() != ETFClassIds::CTFPlayer)
		{
			if (data.bActive)
				data = {};
			continue;
		}

		const auto pPlayer = pEntity->As<C_TFPlayer>();
		if (pPlayer->deadflag())
		{
			if (data.bActive)
				data = {};
			continue;
		}

		// Initialize new player
		if (!data.bActive)
		{
			data.flLastSimTime = pPlayer->m_flSimulationTime();
			data.flLastOldSimTime = pPlayer->m_flOldSimulationTime();
			data.nTicksSinceUpdate = 0;
			data.nLastChokedTicks = 0;
			data.bActive = true;
			continue;
		}

		const float flCurrentSimTime = pPlayer->m_flSimulationTime();

		// Check if simtime changed (player sent an update)
		if (flCurrentSimTime != data.flLastSimTime)
		{
			// They sent an update - record how many ticks they choked
			data.nLastChokedTicks = GetChokedTicks(pPlayer);
			data.flLastOldSimTime = pPlayer->m_flOldSimulationTime();
			data.flLastSimTime = flCurrentSimTime;
			data.nTicksSinceUpdate = 0;
		}
		else
		{
			// No update - they're choking or idle
			data.nTicksSinceUpdate++;
		}
	}
}

bool CFakeLagFix::ShouldShoot(C_TFPlayer* pTarget)
{
	if (!CFG::Aimbot_Hitscan_FakeLagFix || !pTarget)
		return true;

	// Skip FakeLagFix for rapid-fire weapons (SMG, pistol, minigun)
	// These weapons fire too fast for fakelag fix to be useful
	const auto pWeapon = H::Entities->GetWeapon();
	if (pWeapon)
	{
		const int nWeaponID = pWeapon->GetWeaponID();
		if (nWeaponID == TF_WEAPON_SMG ||
			nWeaponID == TF_WEAPON_PISTOL ||
			nWeaponID == TF_WEAPON_PISTOL_SCOUT ||
			nWeaponID == TF_WEAPON_MINIGUN)
		{
			return true; // Always allow shooting for these weapons
		}
	}

	const int nEntIndex = pTarget->entindex();
	if (nEntIndex < 0 || nEntIndex >= FAKELAG_MAX_SLOTS)
		return true;

	const auto& data = m_ChokeData[nEntIndex];
	if (!data.bActive)
		return true;

	// If they're not choking (recent update), allow shooting
	if (data.nTicksSinceUpdate < FAKELAG_CHOKE_THRESHOLD)
		return true;

	// They ARE choking - but we can STILL shoot them using prediction!
	// The server knows their real position (it has their choked commands).
	// We set tick_count to their predicted simtime, and the server will
	// backtrack them to that tick. Since the server has all their commands,
	// it knows exactly where they are.
	//
	// The key insight: we DON'T need to wait for unchoke. We can shoot
	// at their predicted position and the server will validate it.
	// The only risk is if our prediction is wrong (enemy changed direction
	// while choking), but velocity extrapolation is usually close enough.
	//
	// For high-precision weapons (sniper), we should be more careful.
	// For rapid-fire weapons, we already skip the check above.
	if (pWeapon)
	{
		const int nWeaponID2 = pWeapon->GetWeaponID();
		// For sniper rifles: only shoot if we have high confidence
		// (low choke ticks = more accurate prediction)
		if (nWeaponID2 == TF_WEAPON_SNIPERRIFLE ||
			nWeaponID2 == TF_WEAPON_SNIPERRIFLE_CLASSIC ||
			nWeaponID2 == TF_WEAPON_SNIPERRIFLE_DECAP)
		{
			// Sniper: allow shooting if choke is short (prediction is reliable)
			// Block if choke is very long (prediction unreliable for headshots)
			if (data.nTicksSinceUpdate > 8)
			{
				if (CFG::Misc_AntiCheat_Enabled)
					F::AntiCheatCompat->OnFakeLagFixBlocked();
				return false;
			}
		}
	}

	// Notify AntiCheatCompat that we're allowing a shot
	if (CFG::Misc_AntiCheat_Enabled)
		F::AntiCheatCompat->OnFakeLagFixAllowed();

	return true;
}

int CFakeLagFix::GetChokedTicks(C_TFPlayer* pPlayer)
{
	if (!pPlayer)
		return 0;

	const float flSimTime = pPlayer->m_flSimulationTime();
	const float flOldSimTime = pPlayer->m_flOldSimulationTime();
	const float flTickInterval = I::GlobalVars->interval_per_tick;

	// TIME_TO_TICKS equivalent
	const int nChokedTicks = static_cast<int>((flSimTime - flOldSimTime) / flTickInterval + 0.5f);

	return std::clamp(nChokedTicks, 0, FAKELAG_MAX_TICKS);
}

bool CFakeLagFix::IsChoking(C_TFPlayer* pPlayer)
{
	if (!pPlayer)
		return false;

	const int nIdx = pPlayer->entindex();
	if (nIdx < 0 || nIdx >= FAKELAG_MAX_SLOTS)
		return false;

	return m_ChokeData[nIdx].bActive && m_ChokeData[nIdx].nTicksSinceUpdate >= FAKELAG_CHOKE_THRESHOLD;
}

int CFakeLagFix::GetTicksSinceUpdate(C_TFPlayer* pPlayer)
{
	if (!pPlayer)
		return 0;

	const int nIdx = pPlayer->entindex();
	if (nIdx < 0 || nIdx >= FAKELAG_MAX_SLOTS)
		return 0;

	if (!m_ChokeData[nIdx].bActive)
		return 0;

	return m_ChokeData[nIdx].nTicksSinceUpdate;
}

Vec3 CFakeLagFix::GetPredictedPosition(C_TFPlayer* pPlayer)
{
	if (!pPlayer)
		return {};

	const int nIdx = pPlayer->entindex();
	if (nIdx < 0 || nIdx >= FAKELAG_MAX_SLOTS)
		return pPlayer->GetAbsOrigin();

	const auto& data = m_ChokeData[nIdx];

	// If not choking, return current position
	if (!data.bActive || data.nTicksSinceUpdate < FAKELAG_CHOKE_THRESHOLD)
		return pPlayer->GetAbsOrigin();

	// Predict position using last known velocity
	// The server has the enemy's real commands, so it knows their exact position.
	// We can only estimate using velocity, but this is usually close enough.
	//
	// Key: the enemy's velocity from the last update is still a good estimate
	// because fakelagging players typically maintain their trajectory while choking
	// (changing direction while choking defeats the purpose of fakelag).
	Vec3 vVelocity;
	pPlayer->EstimateAbsVelocity(vVelocity);

	const float flPredictedTime = data.nTicksSinceUpdate * I::GlobalVars->interval_per_tick;
	Vec3 vPredicted = pPlayer->GetAbsOrigin() + (vVelocity * flPredictedTime);

	return vPredicted;
}

float CFakeLagFix::GetPredictedSimTime(C_TFPlayer* pPlayer)
{
	if (!pPlayer)
		return pPlayer ? pPlayer->m_flSimulationTime() : 0.0f;

	const int nIdx = pPlayer->entindex();
	if (nIdx < 0 || nIdx >= FAKELAG_MAX_SLOTS)
		return pPlayer->m_flSimulationTime();

	const auto& data = m_ChokeData[nIdx];

	// If not choking, return current simtime
	if (!data.bActive || data.nTicksSinceUpdate < FAKELAG_CHOKE_THRESHOLD)
		return pPlayer->m_flSimulationTime();

	// The enemy's real simtime on the server is their last simtime + choked ticks
	// Because the server processes all their choked commands, advancing their simtime
	const float flPredictedSimTime = data.flLastSimTime + (data.nTicksSinceUpdate * I::GlobalVars->interval_per_tick);

	return flPredictedSimTime;
}
