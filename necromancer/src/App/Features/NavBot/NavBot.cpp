#include "NavBot.h"
#include "NavEngine/NavEngine.h"
#include "../../../SDK/SDK.h"
#include "../CFG.h"
#include "../amalgam_port/AmalgamCompat.h"
#include "../Players/Players.h"
#include "../MovementSimulation/MovementSimulation.h"

#undef min
#undef max
#include <algorithm>

// Objective-based NavBot with proper nav mesh pathfinding
// Uses NavEngine for A* pathfinding through TF2 nav meshes

bool CNavBot::FindObjective(C_TFPlayer* pLocal, Vec3& vOut)
{
	if (!pLocal)
		return false;

	Vec3 vLocalPos = pLocal->m_vecOrigin();
	int nLocalTeam = pLocal->m_iTeamNum();
	int nEnemyTeam = (nLocalTeam == TF_TEAM_RED) ? TF_TEAM_BLUE : TF_TEAM_RED;

	// === CTF Logic (ported from Amalgam's Capture.cpp / FlagController) ===
	// Find enemy flag and check its status
	C_BaseEntity* pEnemyFlag = nullptr;
	Vec3 vEnemyFlagPos;
	Vec3 vOurFlagSpawnPos;
	bool bFoundOurFlagSpawn = false;

	const int nHighestIndex = I::ClientEntityList->GetHighestEntityIndex();
	for (int i = 1; i < nHighestIndex; i++)
	{
		IClientEntity* pClientEntity = I::ClientEntityList->GetClientEntity(i);
		if (!pClientEntity || pClientEntity->IsDormant())
			continue;

		auto pNetworkable = pClientEntity->GetClientNetworkable();
		if (!pNetworkable)
			continue;

		auto pClientClass = pNetworkable->GetClientClass();
		if (!pClientClass)
			continue;

		const auto nClassId = static_cast<ETFClassIds>(pClientClass->m_ClassID);

		// Look for CCaptureFlag entities
		if (nClassId == ETFClassIds::CCaptureFlag)
		{
			auto pEntity = pClientEntity->As<C_BaseEntity>();
			if (!pEntity)
				continue;

			int nFlagTeam = pEntity->m_iTeamNum();

			// Find enemy flag
			if (nFlagTeam == nEnemyTeam)
			{
				pEnemyFlag = pEntity;
				vEnemyFlagPos = pEntity->m_vecOrigin();
			}

			// Remember our flag's spawn position (this IS the capture zone in CTF)
			// When flag is at home (TF_FLAGINFO_HOME = 0), its position is the spawn/cap zone
			if (nFlagTeam == nLocalTeam)
			{
				// m_nFlagStatus netvar: 0 = at home, 1 = stolen, 2 = dropped
				static int nFlagStatusOffset = NetVars::GetNetVar("CCaptureFlag", "m_nFlagStatus");
				int nFlagStatus = *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(pEntity) + nFlagStatusOffset);

				if (nFlagStatus == TF_FLAGINFO_HOME)
				{
					vOurFlagSpawnPos = pEntity->m_vecOrigin();
					bFoundOurFlagSpawn = true;
				}
			}
		}
	}

	// If we found the enemy flag, check if we're carrying it
	if (pEnemyFlag)
	{
		// Check flag status via netvar
		static int nFlagStatusOffset = NetVars::GetNetVar("CCaptureFlag", "m_nFlagStatus");
		int nFlagStatus = *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(pEnemyFlag) + nFlagStatusOffset);

		if (nFlagStatus & TF_FLAGINFO_STOLEN)
		{
			// Flag is stolen - check if WE are the carrier via m_hOwnerEntity
			auto pOwner = pEnemyFlag->m_hOwnerEntity().Get();
			if (pOwner && pOwner == pLocal)
			{
				// We have the flag! Navigate to our capture zone (our flag's spawn position)
				if (bFoundOurFlagSpawn)
				{
					vOut = vOurFlagSpawnPos;
					m_pTargetEntity = pEnemyFlag;
					return true;
				}
			}
			// Someone else has the flag - skip it, can't pick it up
			pEnemyFlag = nullptr;
		}
		else if (nFlagStatus & TF_FLAGINFO_DROPPED)
		{
			// Flag is dropped on the ground - go pick it up
			vOut = vEnemyFlagPos;
			m_pTargetEntity = pEnemyFlag;
			return true;
		}
		else
		{
			// Flag is at home - go steal it
			vOut = vEnemyFlagPos;
			m_pTargetEntity = pEnemyFlag;
			return true;
		}
	}

	// === Control Point logic (ported from Amalgam's CPController) ===
	// Uses CBaseTeamObjectiveResource to find uncaptured control points on CP/KOTH maps.
	// This is the proper way to find CPs — CCaptureZone only exists in CTF.
	{
		C_BaseEntity* pObjectiveResource = nullptr;

		for (int i = 1; i < nHighestIndex; i++)
		{
			IClientEntity* pClientEntity = I::ClientEntityList->GetClientEntity(i);
			if (!pClientEntity)
				continue;

			auto pNetworkable = pClientEntity->GetClientNetworkable();
			if (!pNetworkable)
				continue;

			auto pClientClass = pNetworkable->GetClientClass();
			if (!pClientClass)
				continue;

			if (static_cast<ETFClassIds>(pClientClass->m_ClassID) == ETFClassIds::CTFObjectiveResource)
			{
				pObjectiveResource = pClientEntity->As<C_BaseEntity>();
				break;
			}
		}

		if (pObjectiveResource)
		{
			static int nNumCPsOffset = NetVars::GetNetVar("CTFObjectiveResource", "m_iNumControlPoints");
			static int nCPPositionsOffset = NetVars::GetNetVar("CTFObjectiveResource", "m_vCPPositions[0]");
			static int nOwnerOffset = NetVars::GetNetVar("CTFObjectiveResource", "m_iOwner");
			static int nCPLockedOffset = NetVars::GetNetVar("CTFObjectiveResource", "m_bCPLocked");
			static int nTeamCanCapOffset = NetVars::GetNetVar("CTFObjectiveResource", "m_bTeamCanCap");
			static int nInMiniRoundOffset = NetVars::GetNetVar("CTFObjectiveResource", "m_bInMiniRound");
			static int nPlayingMiniRoundsOffset = NetVars::GetNetVar("CTFObjectiveResource", "m_bPlayingMiniRounds");

			// GetNetVar returns 0 on failure - validate critical offsets
			if (nNumCPsOffset == 0 || nCPPositionsOffset == 0 || nOwnerOffset == 0)
				return false;

			{
				auto pBase = reinterpret_cast<std::uintptr_t>(pObjectiveResource);
				int nNumCPs = *reinterpret_cast<int*>(pBase + nNumCPsOffset);

				if (nNumCPs > 0 && nNumCPs <= 8)
				{
					bool bPlayingMiniRounds = nPlayingMiniRoundsOffset > 0 ? *reinterpret_cast<bool*>(pBase + nPlayingMiniRoundsOffset) : false;

					// Find the closest cappable control point
					C_BaseEntity* pBestCP = nullptr;
					Vec3 vBestCPPos;
					float flBestDist = FLT_MAX;

					for (int i = 0; i < nNumCPs; i++)
					{
						// Get position first
						Vec3 vCPPos = *reinterpret_cast<Vec3*>(pBase + nCPPositionsOffset + i * sizeof(Vec3));

						// Check if we already own this point
						int nOwner = *reinterpret_cast<int*>(pBase + nOwnerOffset + i * sizeof(int));

						bool bLocked = nCPLockedOffset > 0 ? *reinterpret_cast<bool*>(pBase + nCPLockedOffset + i * sizeof(bool)) : false;
						bool bTeamCanCap = nTeamCanCapOffset > 0 ? *reinterpret_cast<bool*>(pBase + nTeamCanCapOffset + (i + nLocalTeam * 8) * sizeof(bool)) : true;

						if (nOwner == nLocalTeam)
							continue;

						if (bLocked)
							continue;

						if (!bTeamCanCap)
							continue;

						// Check if this point is in the current mini-round
						if (bPlayingMiniRounds && nInMiniRoundOffset > 0)
						{
							bool bInMiniRound = *reinterpret_cast<bool*>(pBase + nInMiniRoundOffset + i * sizeof(bool));
							if (!bInMiniRound)
								continue;
						}

						if (vCPPos.IsZero())
							continue;

						float flDist = vLocalPos.DistTo(vCPPos);
						if (flDist < flBestDist)
						{
							flBestDist = flDist;
							vBestCPPos = vCPPos;
							pBestCP = pObjectiveResource;  // Use resource as target entity
						}
					}

					if (pBestCP)
					{
						vOut = vBestCPPos;
						m_pTargetEntity = pBestCP;
						return true;
					}
					else
					{
					}
				}
			}
		}
		else
		{
		}
	}

	// === Payload logic (ported from Amalgam's PLController) ===
	// Find CObjectCartDispenser entities — these are the payload carts on PL/PLR maps.
	{
		C_BaseEntity* pNearestCart = nullptr;
		float flNearestDist = FLT_MAX;

		for (int i = 1; i < nHighestIndex; i++)
		{
			IClientEntity* pClientEntity = I::ClientEntityList->GetClientEntity(i);
			if (!pClientEntity || pClientEntity->IsDormant())
				continue;

			auto pNetworkable = pClientEntity->GetClientNetworkable();
			if (!pNetworkable)
				continue;

			auto pClientClass = pNetworkable->GetClientClass();
			if (!pClientClass)
				continue;

			if (static_cast<ETFClassIds>(pClientClass->m_ClassID) == ETFClassIds::CObjectCartDispenser)
			{
				auto pEntity = pClientEntity->As<C_BaseEntity>();
				if (!pEntity)
					continue;

				int nCartTeam = pEntity->m_iTeamNum();

				// Only push our team's cart
				if (nCartTeam != nLocalTeam)
					continue;

				Vec3 vCartPos = pEntity->m_vecOrigin();
				float flDist = vLocalPos.DistTo(vCartPos);

				if (flDist < flNearestDist)
				{
					flNearestDist = flDist;
					pNearestCart = pEntity;
				}
			}
		}

		if (pNearestCart)
		{
			vOut = pNearestCart->m_vecOrigin();
			m_pTargetEntity = pNearestCart;
			return true;
		}
	}

	// === CTF fallback: CCaptureZone (capture zone where you bring the flag) ===
	// This is only used if no control points or payload were found (pure CTF without flags detected)
	{
		C_BaseEntity* pNearestObjective = nullptr;
		float flNearestDist = FLT_MAX;

		for (int i = 1; i < nHighestIndex; i++)
		{
			IClientEntity* pClientEntity = I::ClientEntityList->GetClientEntity(i);
			if (!pClientEntity || pClientEntity->IsDormant())
				continue;

			auto pNetworkable = pClientEntity->GetClientNetworkable();
			if (!pNetworkable)
				continue;

			auto pClientClass = pNetworkable->GetClientClass();
			if (!pClientClass)
				continue;

			const auto nClassId = static_cast<ETFClassIds>(pClientClass->m_ClassID);

			if (nClassId == ETFClassIds::CCaptureZone)
			{
				auto pEntity = pClientEntity->As<C_BaseEntity>();
				if (!pEntity)
					continue;

				int nObjTeam = pEntity->m_iTeamNum();

				// Target enemy/neutral capture zones
				if (nObjTeam == nLocalTeam)
					continue;

				Vec3 vObjPos = pEntity->m_vecOrigin();
				float flDist = vLocalPos.DistTo(vObjPos);

				if (flDist < flNearestDist)
				{
					flNearestDist = flDist;
					pNearestObjective = pEntity;
				}
			}
		}

		if (pNearestObjective)
		{
			vOut = pNearestObjective->m_vecOrigin();
			m_pTargetEntity = pNearestObjective;
			return true;
		}
	}

	return false;
}

bool CNavBot::ShouldSearchHealth(C_TFPlayer* pLocal)
{
	if (!CFG::NavBot_SearchHealth)
		return false;

	if (!pLocal)
		return false;

	// Don't search if already being healed (ported from TF2 bot's GetNumHealers check)
	if (pLocal->m_nPlayerCond() & (1 << 21)) // TFCond_Healing
		return false;

	float flHealthPercent = static_cast<float>(pLocal->m_iHealth()) / pLocal->GetMaxHealth();

	// Ported from TF2 bot's CTFBotTacticalMonitor and CTFBotGetHealth
	// Dynamic thresholds based on combat state, like TF2 bots:
	// - In combat: stay in fight until nearly dead (critical ratio 0.3)
	// - Out of combat: go get health when hurt (ok ratio 0.8)
	// - On fire: always seek health immediately (critical)
	const float flCriticalRatio = 0.3f;
	const float flOkRatio = 0.8f;

	// On fire - get health now (ported from TF2 bot's burning check)
	if (pLocal->InCond(TF_COND_BURNING))
		return true;

	// Approximate "in combat" by checking if there are visible enemies nearby
	// TF2 bot uses CTFNavArea::IsInCombat() which tracks recent damage in the area
	// We approximate by checking if we can see living enemies within 800 units
	bool bInCombat = false;
	{
		Vec3 vLocalPos = pLocal->m_vecOrigin();
		const int nMaxClients = I::EngineClient->GetMaxClients();
		for (int i = 1; i <= nMaxClients; i++)
		{
			if (i == I::EngineClient->GetLocalPlayer())
				continue;
			auto pClientEntity = I::ClientEntityList->GetClientEntity(i);
			if (!pClientEntity)
				continue;
			auto pPlayer = pClientEntity->As<C_TFPlayer>();
			if (!pPlayer || !pPlayer->IsAlive() || pPlayer->m_iTeamNum() == pLocal->m_iTeamNum())
				continue;
			if (pPlayer->IsDormant())
				continue;
			if (vLocalPos.DistTo(pPlayer->m_vecOrigin()) < 800.0f)
			{
				bInCombat = true;
				break;
			}
		}
	}

	if (bInCombat)
	{
		// In combat - stay in the fight until nearly dead (ported from TF2 bot)
		return flHealthPercent < flCriticalRatio;
	}
	else
	{
		// Not in combat - go get health when hurt (ported from TF2 bot)
		return flHealthPercent < flOkRatio;
	}
}

bool CNavBot::ShouldSearchAmmo(C_TFPlayer* pLocal)
{
	if (!CFG::NavBot_SearchAmmo)
		return false;

	if (!pLocal)
		return false;

	// Ported from TF2 bot's CTFBot::IsAmmoLow
	// TF2 bot checks: primary ammo ratio < 0.2 (20% of max)
	// Also skips melee weapons and weapons with no projectiles (medigun, etc.)

	// Max ammo values per class (TF2 server CTFPlayer::GetMaxAmmo)
	// Index: TF_AMMO_PRIMARY=1, TF_AMMO_SECONDARY=2, TF_AMMO_METAL=3
	// These are the default max values without any ammo-increasing items
	static const int s_iMaxAmmo[10][4] = {
		//                    PRIMARY  SECONDARY  METAL
		/* Scout    */  { 0, 32,  75,  0  },
		/* Soldier  */  { 0, 20,  32,  0  },
		/* Pyro     */  { 0, 200, 32,  0  },
		/* Demoman  */  { 0, 16,  24,  0  },
		/* Heavy    */  { 0, 200, 150, 0  },
		/* Engineer */  { 0, 32,  150, 200 },
		/* Medic    */  { 0, 150, 32,  0  },
		/* Sniper   */  { 0, 25,  75,  0  },
		/* Spy      */  { 0, 20,  24,  0  },
	};

	int iClass = pLocal->m_iClass();
	if (iClass < 1 || iClass > 9)
		return false;

	for (int nSlot = 0; nSlot <= 1; nSlot++)  // 0=Primary, 1=Secondary
	{
		auto pWeapon = pLocal->GetWeaponFromSlot(nSlot);
		if (!pWeapon)
			continue;

		auto pWeaponBase = pWeapon->As<C_TFWeaponBase>();
		if (!pWeaponBase)
			continue;

		// Wrench is special — it's melee that uses metal (ammo)
		// TF2 bot: "return ( GetAmmoCount( TF_AMMO_METAL ) <= 0 )"
		if (pWeaponBase->GetWeaponID() == TF_WEAPON_WRENCH)
		{
			int iMetal = pLocal->GetAmmoCount(3);  // TF_AMMO_METAL = 3
			if (iMetal <= 0)
				return true;
			continue;
		}

		int iAmmoType = pWeaponBase->m_iPrimaryAmmoType();
		if (iAmmoType < 0)
			continue;  // Weapon doesn't use ammo (e.g., medigun)

		int iReserve = pLocal->GetAmmoCount(iAmmoType);

		// Get max ammo from table (ammoType 1-3, class 1-9)
		int iMaxAmmo = 0;
		if (iAmmoType >= 1 && iAmmoType <= 3)
			iMaxAmmo = s_iMaxAmmo[iClass - 1][iAmmoType];

		if (iMaxAmmo > 0)
		{
			// TF2 bot: ratio < 0.2f means low ammo
			float flRatio = static_cast<float>(iReserve) / static_cast<float>(iMaxAmmo);
			if (flRatio < 0.2f)
				return true;
		}
		else if (iReserve <= 5)
		{
			// Fallback for weapons where we can't determine max ammo
			return true;
		}
	}

	return false;
}

bool CNavBot::FindSupply(C_TFPlayer* pLocal, Vec3& vOut, bool bHealth)
{
	if (!pLocal)
		return false;

	Vec3 vLocalPos = pLocal->m_vecOrigin();
	C_BaseEntity* pBestSupply = nullptr;
	float flBestCost = FLT_MAX;

	// When following a player (tagged or teammate), limit how far we'll go for supplies
	// Uses NavBot_FollowSupplyDistance as max distance from the FOLLOWED PLAYER (not from us)
	// This prevents the bot from abandoning the player it's following
	float flMaxSupplyDistFromPlayer = FLT_MAX;
	bool bIsFollowing = m_bIsFollowingTaggedPlayer || m_bIsFollowingTeammate || m_bIsGettingSupply;

	if (bIsFollowing && CFG::NavBot_FollowSupplyDistance > 0.0f)
	{
		flMaxSupplyDistFromPlayer = CFG::NavBot_FollowSupplyDistance;
	}

	// Get the followed player's position for distance checking
	Vec3 vFollowedPlayerPos(0, 0, 0);
	bool bHasFollowedPlayerPos = false;
	if (bIsFollowing)
	{
		int iFollowIdx = m_bIsFollowingTaggedPlayer ? m_iFollowTaggedTargetIdx : m_iFollowTargetIdx;
		if (iFollowIdx > 0)
		{
			auto pFollowedEntity = I::ClientEntityList->GetClientEntity(iFollowIdx);
			if (pFollowedEntity)
			{
				auto pFollowedPlayer = pFollowedEntity->As<C_TFPlayer>();
				if (pFollowedPlayer && pFollowedPlayer->IsAlive())
				{
					vFollowedPlayerPos = pFollowedPlayer->m_vecOrigin();
					bHasFollowedPlayerPos = true;
				}
			}
		}
	}

	// Ported from TF2 bot's dynamic search range (CTFBotGetHealth/CTFBotGetAmmo)
	// The more hurt/low on ammo we are, the farther we'll travel to get supplies
	float flSearchRange = 1000.0f;  // Default near range
	if (bHealth)
	{
		float flHealthPercent = static_cast<float>(pLocal->m_iHealth()) / pLocal->GetMaxHealth();
		const float flCriticalRatio = 0.3f;
		const float flOkRatio = 0.8f;
		float t = (flHealthPercent - flCriticalRatio) / (flOkRatio - flCriticalRatio);
		t = std::clamp(t, 0.0f, 1.0f);
		// On fire - search far
		if (pLocal->InCond(TF_COND_BURNING))
			t = 0.0f;
		// Interpolate: critical = far range (2000), ok = near range (1000)
		flSearchRange = 2000.0f + t * (1000.0f - 2000.0f);
	}
	else
	{
		// Ammo search range (ported from TF2 bot's tf_bot_ammo_search_range)
		flSearchRange = 5000.0f;
	}

	// Get local player's nav area for path-cost-based selection
	CNavArea* pLocalArea = G_NavEngine.IsNavMeshLoaded() ? G_NavEngine.m_pMap->FindClosestNavArea(vLocalPos) : nullptr;
	int iTeam = pLocal->m_iTeamNum();

	// Helper: compute travel cost to a supply via nav mesh (ported from TF2 bot's CollectSurroundingAreas approach)
	// If nav mesh is available, uses actual path cost. Falls back to straight-line distance.
	auto fnComputeCost = [&](C_BaseEntity* pEntity) -> float {
		Vec3 vPackPos = pEntity->m_vecOrigin();

		// Straight-line pre-filter: skip if too far away even in a straight line
		float flStraightDist = vLocalPos.DistTo(vPackPos);
		if (flStraightDist > flSearchRange * 1.5f)
			return FLT_MAX;  // Way too far, skip

		// When following, also check distance from the followed player
		if (bHasFollowedPlayerPos)
		{
			float flDistFromPlayer = vFollowedPlayerPos.DistTo(vPackPos);
			if (flDistFromPlayer > flMaxSupplyDistFromPlayer)
				return FLT_MAX;
		}

		// Ported from TF2 bot's CHealthFilter/CAmmoFilter: skip items where an enemy is closer
		// TF2 bot: "if ( close.m_closePlayer && !m_me->InSameTeam( close.m_closePlayer ) ) return false"
		// This prevents selecting items that enemies will grab first
		{
			float flOurDist = vLocalPos.DistTo(vPackPos);
			const int nMaxClients = I::EngineClient->GetMaxClients();
			for (int i = 1; i <= nMaxClients; i++)
			{
				if (i == I::EngineClient->GetLocalPlayer())
					continue;
				auto pClientEntity = I::ClientEntityList->GetClientEntity(i);
				if (!pClientEntity)
					continue;
				auto pPlayer = pClientEntity->As<C_TFPlayer>();
				if (!pPlayer || !pPlayer->IsAlive() || pPlayer->m_iTeamNum() == iTeam)
					continue;
				if (pPlayer->IsDormant())
					continue;

				float flEnemyDist = pPlayer->m_vecOrigin().DistTo(vPackPos);
				if (flEnemyDist < flOurDist * 0.8f)
					return FLT_MAX;  // Enemy is significantly closer — skip this item
			}
		}

		// Ported from TF2 bot's CHealthFilter/CAmmoFilter: check nav area TF attributes
		// to avoid enemy spawn rooms and inaccessible resupply cabinets
		if (pLocalArea && G_NavEngine.IsNavMeshLoaded())
		{
			CNavArea* pPackArea = G_NavEngine.m_pMap->FindClosestNavArea(vPackPos);
			if (pPackArea)
			{
				// Don't go into enemy spawn rooms (ported from TF2 bot's spawn room check)
				const bool bRedSpawn = pPackArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_RED;
				const bool bBlueSpawn = pPackArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_BLUE;
				if (iTeam == TF_TEAM_RED && bBlueSpawn && !bRedSpawn)
					return FLT_MAX;  // Red player, blue spawn - avoid
				if (iTeam == TF_TEAM_BLUE && bRedSpawn && !bBlueSpawn)
					return FLT_MAX;  // Blue player, red spawn - avoid

				// Ported from TF2 bot's path-cost-based supply selection:
				// Use nav mesh path cost instead of straight-line distance.
				// A supply 500 units away behind a wall is worse than one 800 units away on a direct path.
				CNavArea* pStartArea = pLocalArea;
				float flCost;
				std::vector<void*> vPath;
				int iResult = G_NavEngine.m_pMap->m_pather.Solve(
					reinterpret_cast<void*>(pStartArea),
					reinterpret_cast<void*>(pPackArea),
					&vPath, &flCost);

				if (iResult == micropather::MicroPather::START_END_SAME)
					return 1.0f;  // Same area, very close

				if (iResult == micropather::MicroPather::SOLVED && flCost > 0.0f)
					return flCost;

				// Path not solvable via nav mesh - fall through to straight-line
			}
		}

		// Fallback: straight-line distance (no nav mesh or path not found)
		return flStraightDist;
	};

	if (bHealth)
	{
		// Search health packs from entity group
		for (const auto pEntity : H::Entities->GetGroup(EEntGroup::HEALTHPACKS))
		{
			if (!pEntity || pEntity->IsDormant())
				continue;

			// Skip picked-up items (EF_NODRAW = invisible, waiting to respawn)
			// TF2 bot: "if ( candidate->IsEffectActive( EF_NODRAW ) ) return false"
			if (pEntity->m_fEffects() & EF_NODRAW)
				continue;

			float flCost = fnComputeCost(pEntity);
			if (flCost < flBestCost)
			{
				flBestCost = flCost;
				pBestSupply = pEntity;
			}
		}

		// Ported from TF2 bot's CHealthFilter: also search for friendly dispensers
		const int nHighestIndex = I::ClientEntityList->GetHighestEntityIndex();
		for (int i = 1; i <= nHighestIndex; i++)
		{
			IClientEntity* pClientEntity = I::ClientEntityList->GetClientEntity(i);
			if (!pClientEntity || pClientEntity->IsDormant())
				continue;

			auto pNetworkable = pClientEntity->GetClientNetworkable();
			if (!pNetworkable)
				continue;

			auto pClientClass = pNetworkable->GetClientClass();
			if (!pClientClass)
				continue;

			const auto nClassId = static_cast<ETFClassIds>(pClientClass->m_ClassID);

			// Friendly dispenser (ported from TF2 bot's obj_dispenser check)
			if (nClassId == ETFClassIds::CObjectDispenser)
			{
				if (CFG::NavBot_IgnoreDispensers)
					continue;

				auto pEntity = pClientEntity->As<C_BaseEntity>();
				if (!pEntity)
					continue;

				// Only use friendly team dispensers
				if (pEntity->m_iTeamNum() != iTeam)
					continue;

				// Skip dispensers that aren't operational
				// TF2 bot: "if ( !dispenser->IsBuilding() && !dispenser->IsPlacing() && !dispenser->IsDisabled() )"
				auto pBuilding = pEntity->As<C_BaseObject>();
				if (pBuilding && (pBuilding->m_bPlacing() || pBuilding->m_bBuilding() || pBuilding->m_bHasSapper()))
					continue;

				float flCost = fnComputeCost(pEntity);
				if (flCost < flBestCost)
				{
					flBestCost = flCost;
					pBestSupply = pEntity;
				}
			}
		}
	}
	else
	{
		// Search ammo packs - CTFAmmoPack entities + model-based ammo packs
		// First check CTFAmmoPack class ID
		const int nHighestIndex = I::ClientEntityList->GetHighestEntityIndex();
		for (int i = 1; i < nHighestIndex; i++)
		{
			IClientEntity* pClientEntity = I::ClientEntityList->GetClientEntity(i);
			if (!pClientEntity || pClientEntity->IsDormant())
				continue;

			auto pNetworkable = pClientEntity->GetClientNetworkable();
			if (!pNetworkable)
				continue;

			auto pClientClass = pNetworkable->GetClientClass();
			if (!pClientClass)
				continue;

			const auto nClassId = static_cast<ETFClassIds>(pClientClass->m_ClassID);

			if (nClassId == ETFClassIds::CTFAmmoPack)
			{
				auto pEntity = pClientEntity->As<C_BaseEntity>();
				if (!pEntity)
					continue;

				// Skip picked-up items (EF_NODRAW = invisible, waiting to respawn)
				if (pEntity->m_fEffects() & EF_NODRAW)
					continue;

				float flCost = fnComputeCost(pEntity);
				if (flCost < flBestCost)
				{
					flBestCost = flCost;
					pBestSupply = pEntity;
				}
			}

			// Friendly dispenser also provides ammo (ported from TF2 bot's CAmmoFilter)
			if (nClassId == ETFClassIds::CObjectDispenser)
			{
				if (CFG::NavBot_IgnoreDispensers)
					continue;

				auto pEntity = pClientEntity->As<C_BaseEntity>();
				if (!pEntity)
					continue;

				// Only use friendly team dispensers
				if (pEntity->m_iTeamNum() != iTeam)
					continue;

				// Skip dispensers that aren't operational
				auto pBuilding = pEntity->As<C_BaseObject>();
				if (pBuilding && (pBuilding->m_bPlacing() || pBuilding->m_bBuilding() || pBuilding->m_bHasSapper()))
					continue;

				float flCost = fnComputeCost(pEntity);
				if (flCost < flBestCost)
				{
					flBestCost = flCost;
					pBestSupply = pEntity;
				}
			}
		}

		// Also check model-based ammo packs from entity group
		for (const auto pEntity : H::Entities->GetGroup(EEntGroup::AMMOPACKS))
		{
			if (!pEntity || pEntity->IsDormant())
				continue;

			// Skip picked-up items (EF_NODRAW = invisible, waiting to respawn)
			if (pEntity->m_fEffects() & EF_NODRAW)
				continue;

			float flCost = fnComputeCost(pEntity);
			if (flCost < flBestCost)
			{
				flBestCost = flCost;
				pBestSupply = pEntity;
			}
		}
	}

	if (pBestSupply)
	{
		// Use GetCenter() (WorldSpaceCenter approximation) instead of m_vecOrigin (base position)
		// TF2 bot paths to WorldSpaceCenter — this is the actual pickup point, not the floor position
		vOut = pBestSupply->GetCenter();
		m_pTargetEntity = pBestSupply;
		m_pSupplyEntity = pBestSupply;
		m_iSupplyEntityIdx = pBestSupply->entindex();
		m_bSupplyIsDispenser = (pBestSupply->GetClassId() == ETFClassIds::CObjectDispenser);
		return true;
	}

	return false;
}

void CNavBot::PickRandomWanderTarget(C_TFPlayer* pLocal)
{
	if (!pLocal)
		return;

	float flCurTime = I::GlobalVars->curtime;

	// Clear visited areas if they get too large or after some time (like Amalgam's Roam)
	if (flCurTime >= m_flNextVisitedClearTime || m_vVisitedAreaCenters.size() > 40)
	{
		m_vVisitedAreaCenters.clear();
		m_nConsecutiveFails = 0;
		m_flNextVisitedClearTime = flCurTime + 60.0f;
	}

	// Reset wander target
	m_vWanderTarget = Vec3(0, 0, 0);

	// Check if NavEngine has a valid nav mesh loaded
	if (!G_NavEngine.IsNavMeshLoaded())
		return;

	auto& vAreas = G_NavEngine.m_pMap->m_navfile.m_vAreas;
	if (vAreas.empty())
		return;

	Vec3 vLocalPos = pLocal->m_vecOrigin();
	std::vector<CNavArea*> vValidAreas;

	// Filter valid areas (ported from Amalgam's Roam.cpp)
	for (auto& tArea : vAreas)
	{
		// Skip areas marked as AVOID
		if (tArea.m_iAttributeFlags & NAV_MESH_AVOID)
			continue;

		// Skip spawn rooms (like Amalgam: "Dont run in spawn bitch")
		if (tArea.m_iTFAttributeFlags & (TF_NAV_SPAWN_ROOM_BLUE | TF_NAV_SPAWN_ROOM_RED))
			continue;

		// Skip if we recently visited an area near this one (750 unit radius, like Amalgam)
		bool bTooCloseToVisited = false;
		for (const auto& vVisited : m_vVisitedAreaCenters)
		{
			if (tArea.m_vCenter.DistTo(vVisited) < 750.0f)
			{
				bTooCloseToVisited = true;
				break;
			}
		}
		if (bTooCloseToVisited)
			continue;

		// Skip areas that are too close to us (like Amalgam's 500 unit minimum)
		float flDist = tArea.m_vCenter.DistTo(vLocalPos);
		if (flDist < 500.0f)
			continue;

		vValidAreas.push_back(&tArea);
	}

	// No valid areas found
	if (vValidAreas.empty())
	{
		// If we failed too many times in a row, clear visited areas (like Amalgam)
		if (++m_nConsecutiveFails >= 3)
		{
			m_vVisitedAreaCenters.clear();
			m_nConsecutiveFails = 0;
		}
		return;
	}

	// Reset fail counter since we found valid areas
	m_nConsecutiveFails = 0;

	// Sort by distance (farthest first, like Amalgam's Roam)
	std::sort(vValidAreas.begin(), vValidAreas.end(), [&vLocalPos](CNavArea* a, CNavArea* b) {
		if (!a || !b) return false;
		return a->m_vCenter.DistToSqr(vLocalPos) > b->m_vCenter.DistToSqr(vLocalPos);
	});

	// Add some randomness (like Amalgam's 20% swap chance)
	for (size_t i = 0; i < vValidAreas.size(); ++i)
	{
		if (static_cast<float>(rand()) / RAND_MAX < 0.2f)
		{
			size_t j = (i + 1 < vValidAreas.size()) ? i + 1 : (i > 0 ? i - 1 : i);
			if (i != j)
				std::swap(vValidAreas[i], vValidAreas[j]);
		}
	}

	// Try to path to each candidate area until one succeeds (like Amalgam's Roam)
	for (auto pArea : vValidAreas)
	{
		if (!pArea)
			continue;

		// Validate that we can actually path there before committing
		if (G_NavEngine.NavTo(pArea->m_vCenter, NAV_PRIO_PATROL))
		{
			m_vWanderTarget = pArea->m_vCenter;
			m_vVisitedAreaCenters.push_back(pArea->m_vCenter);
			return;
		}
	}

	// All areas failed to path - no valid wander target
}

void CNavBot::Run(C_TFPlayer* pLocal, CUserCmd* pCmd)
{
	// Auto join team/class (ported from Amalgam's AutoJoin) - runs before alive check
	// so we can join team/class even when dead or not yet on a team
	// Note: these may send ClientCmd_Unrestricted which can change player state,
	// so we re-fetch pLocal afterwards to avoid stale pointers
	AutoJoinTeam(pLocal);
	AutoJoinClass(pLocal);

	// Re-fetch local player after auto join commands may have changed game state
	pLocal = H::Entities->GetLocal();

	if (!pLocal || !pLocal->IsAlive())
	{
		m_bActive = false;
		m_pTargetEntity = nullptr;
		G_NavEngine.CancelPath();
		m_bWasDeadLastTick = true;
		return;
	}

	float flCurTime = I::GlobalVars->curtime;

	// === Death pause: stop moving for a duration after respawning ===
	if (CFG::NavBot_DeathPause)
	{
		// Detect respawn (was dead last tick, now alive)
		if (m_bWasDeadLastTick)
		{
			m_flDeathPauseEndTime = flCurTime + CFG::NavBot_DeathPauseDuration;
			m_bWasDeadLastTick = false;
		}

		// While pause is active, cancel path and suppress movement
		if (flCurTime < m_flDeathPauseEndTime)
		{
			G_NavEngine.CancelPath();
			return;
		}
	}
	else
	{
		m_bWasDeadLastTick = false;
	}

	// Still in setup time (blue team on PL/CP maps) - don't path yet
	if (CFG::NavBot_WaitForSetup && G_NavEngine.IsSetupTime())
	{
		G_NavEngine.CancelPath();
		return;
	}

	Vec3 vLocalPos = pLocal->m_vecOrigin();

	// === Per-tick teleport / round-reset detection ===
	// If player suddenly moved a huge distance since last tick (respawn, round reset),
	// the old path is completely invalid - cancel it immediately
	if (G_NavEngine.IsPathing() && m_bHasLastTickPos)
	{
		float flDistFromLast = vLocalPos.DistTo(m_vLastTickPos);
		if (flDistFromLast > CFG::NavBot_TeleportThreshold)
		{
			G_NavEngine.CancelPath();
			m_nStuckCount = 0;
			m_nStuckCheckTicks = 0;
			m_bStuckWiggle = false;
			m_flNextWanderTime = 0.0f;
			m_flNextRepathTime = 0.0f;
		}
	}
	m_vLastTickPos = vLocalPos;
	m_bHasLastTickPos = true;

	// Stuck detection - Amalgam-style per-connection tracking with blacklisting
	// (replaces old windowed stuck detection which couldn't blacklist stuck connections)
	if (G_NavEngine.IsPathing())
	{
		G_NavEngine.UpdateStuckTime(pLocal);

		// Wiggle to try to break free when stuck (supplementary to UpdateStuckTime)
		float flStuckTime = I::GlobalVars->curtime - G_NavEngine.GetInactivityTime();
		if (flStuckTime > 0.5f)
		{
			m_bStuckWiggle = !m_bStuckWiggle;
			pCmd->sidemove = m_bStuckWiggle ? 450.0f : -450.0f;
		}
	}
	else
	{
		m_bStuckWiggle = false;
	}

	bool bHasObjective = false;

	// === Update closest enemy tracking (like Amalgam's BotUtils::UpdateCloseEnemies) ===
	UpdateCloseEnemies(pLocal);

	// === Vischeck current path (ported from Amalgam's VischeckPath) ===
	// Checks if path is still passable, auto-repaths if blocked
	if (G_NavEngine.IsPathing())
		G_NavEngine.VischeckPath(pLocal);

	// === Weapon switching (ported from Amalgam's UpdateBestSlot/SetSlot) ===
	// Switches to best weapon based on class, ammo, and enemy distance
	SwitchWeapon(pLocal);

	// === Priority chain (ported from Amalgam's NavBotCore::Run) ===
	// Higher priority jobs cancel lower priority paths via NavTo priority check
	// Order: EscapeSpawn > EscapeProjectiles > EscapeDanger > FollowSupply > FollowTagged > FollowTeammates > Health > Capture > Ammo > StayNear > Wander
	// When following a player and needing supplies, m_bIsGettingSupply pauses follow so they don't fight

	// === Update danger blacklist (must run before escape checks) ===
	UpdateDangerBlacklist(pLocal);

	// === EscapeSpawn - get out of spawn room (HIGHEST PRIORITY) ===
	if (EscapeSpawn(pLocal))
		bHasObjective = true;

	// === EscapeProjectiles - flee from incoming rockets/stickies ===
	// Skip if getting supplies — committed to supply run, don't flip-flop
	if (!bHasObjective && !m_bIsGettingSupply && EscapeProjectiles(pLocal))
		bHasObjective = true;

	// === EscapeDanger - flee from enemies/sentries ===
	// Skip if getting supplies — committed to supply run, don't flip-flop
	if (!bHasObjective && !m_bIsGettingSupply && EscapeDanger(pLocal))
		bHasObjective = true;

	// === Supply cancellation checks (ported from TF2 bot's CTFBotGetHealth/CTFBotGetAmmo) ===
	// TF2 bots cancel their supply path when:
	// 1. The item is gone (picked up by someone else, or hidden with EF_NODRAW)
	// 2. Health/ammo is already full (someone healed us, or we picked up a different pack)
	// 3. A medic is healing us (no need for health kit)
	// 4. An enemy is closer to the item than us (they'll grab it first)
	// 5. For dispensers: we're near enough and can see it — wait, don't run over it
	// 6. For dispensers: we're in combat and not critically low — fight instead of waiting
	if (m_bIsGettingSupply && m_pSupplyEntity)
	{
		bool bCancelSupply = false;
		const char* szCancelReason = nullptr;

		// 1. Item is gone — entity is null, dormant too long, no longer valid, or picked up (EF_NODRAW)
		//    TF2 bot: "if ( m_healthKit == NULL || m_healthKit->IsEffectActive( EF_NODRAW ) )"
		if (!H::Entities->SafeIsEntityValid(m_pSupplyEntity, m_iSupplyEntityIdx) || m_pSupplyEntity->IsDormant() ||
			(m_pSupplyEntity->m_fEffects() & EF_NODRAW))
		{
			bCancelSupply = true;
			szCancelReason = "Supply item is gone";
		}

		// 1b. Dispenser became non-operational (sapped, building, carrying)
		//     TF2 bot skips dispensers that are placing/building/sapped in both CHealthFilter and CAmmoFilter
		if (!bCancelSupply && m_bSupplyIsDispenser)
		{
			auto pBuilding = m_pSupplyEntity->As<C_BaseObject>();
			if (pBuilding && (pBuilding->m_bPlacing() || pBuilding->m_bBuilding() || pBuilding->m_bHasSapper() || pBuilding->m_bCarryDeploy()))
			{
				bCancelSupply = true;
				szCancelReason = "Dispenser is no longer operational";
			}
		}

		// 2. Health is full — someone healed us or we picked up a different pack
		//    TF2 bot: "if ( me->GetHealth() >= me->GetMaxHealth() ) return Done"
		if (!bCancelSupply && ShouldSearchHealth(pLocal) == false && m_pSupplyEntity)
		{
			// Check if this was a health supply by checking if we no longer need health
			// and we were pathing to a health priority
			if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_GETHEALTH ||
				G_NavEngine.m_eCurrentPriority == NAV_PRIO_FOLLOW_SUPPLY)
			{
				// We no longer need health — we must have been healed
				// But only cancel if we don't also need ammo
				if (!ShouldSearchAmmo(pLocal))
				{
					bCancelSupply = true;
					szCancelReason = "Health/ammo already obtained";
				}
			}
		}

		// 3. A medic is healing us — no need for health kit
		//    TF2 bot: "if ( i < me->m_Shared.GetNumHealers() ) return Done( 'A Medic is healing me' )"
		if (!bCancelSupply && (pLocal->m_nPlayerCond() & (1 << 21)))  // TFCond_Healing
		{
			// A medic (not dispenser) is healing us — cancel health search
			if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_GETHEALTH)
			{
				bCancelSupply = true;
				szCancelReason = "A Medic is healing me";
			}
		}

		// 4. Enemy is closer to the item than us
		//    TF2 bot: "if ( close.m_closePlayer && !me->InSameTeam( close.m_closePlayer ) ) return Done"
		if (!bCancelSupply && H::Entities->SafeIsEntityValid(m_pSupplyEntity, m_iSupplyEntityIdx))
		{
			Vec3 vSupplyPos = m_pSupplyEntity->m_vecOrigin();
			Vec3 vLocalPos = pLocal->m_vecOrigin();
			float flOurDist = vLocalPos.DistTo(vSupplyPos);

			const int nMaxClients = I::EngineClient->GetMaxClients();
			for (int i = 1; i <= nMaxClients; i++)
			{
				if (i == I::EngineClient->GetLocalPlayer())
					continue;
				auto pClientEntity = I::ClientEntityList->GetClientEntity(i);
				if (!pClientEntity)
					continue;
				auto pPlayer = pClientEntity->As<C_TFPlayer>();
				if (!pPlayer || !pPlayer->IsAlive() || pPlayer->m_iTeamNum() == pLocal->m_iTeamNum())
					continue;
				if (pPlayer->IsDormant())
					continue;

				float flEnemyDist = pPlayer->m_vecOrigin().DistTo(vSupplyPos);
				if (flEnemyDist < flOurDist * 0.8f)  // Enemy is significantly closer
				{
					bCancelSupply = true;
					szCancelReason = "An enemy is closer to the supply";
					break;
				}
			}
		}

		// 5. Dispenser proximity behavior — wait near it, don't run over it
		//    TF2 bot: "if ( (me->GetAbsOrigin() - m_ammo->GetAbsOrigin()).IsLengthLessThan( nearRange ) )"
		if (!bCancelSupply && m_bSupplyIsDispenser && H::Entities->SafeIsEntityValid(m_pSupplyEntity, m_iSupplyEntityIdx))
		{
			Vec3 vDispenserPos = m_pSupplyEntity->m_vecOrigin();
			Vec3 vLocalPos = pLocal->m_vecOrigin();
			float flDistToDispenser = vLocalPos.DistTo(vDispenserPos);

			if (flDistToDispenser < 75.0f)
			{
				// We're near the dispenser — check if we can see it
				// TF2 bot: "if ( me->GetVisionInterface()->IsLineOfSightClearToEntity( m_ammo ) )"
				Vec3 vStart = vLocalPos; vStart.z += PLAYER_CROUCHED_JUMP_HEIGHT;
				Vec3 vEnd = vDispenserPos; vEnd.z += 40.0f;  // Approximate dispenser center
				bool bCanSee = H::AimUtils->TracePositionWorld(vStart, vEnd);

				if (bCanSee)
				{
					// 6. Combat priority: if we're in combat and not critically low, fight instead
					//    TF2 bot: "if ( !me->IsAmmoLow() && me->GetVisionInterface()->GetPrimaryKnownThreat() )
					//              return Done( 'No time to wait for more ammo, I must fight' )"
					bool bInCombat = (G::nTargetIndex != -1);  // Aimbot has a target
					bool bCriticallyLow = ShouldSearchHealth(pLocal) &&
						(static_cast<float>(pLocal->m_iHealth()) / pLocal->GetMaxHealth() < 0.3f);

					if (bInCombat && !bCriticallyLow)
					{
						bCancelSupply = true;
						szCancelReason = "No time to wait at dispenser, must fight";
					}
					else
					{
						// Near dispenser, can see it, not in combat — wait for it to refill us
						// Cancel the nav path so we don't keep trying to walk INTO the dispenser
						// Just stand near it. The supply flag will clear when we're full.
						m_bWaitingAtDispenser = true;
						if (G_NavEngine.IsPathing())
						{
							G_NavEngine.CancelPath();
						}
						// Stop moving every tick while waiting at dispenser
						pCmd->forwardmove = 0.0f;
						pCmd->sidemove = 0.0f;
					}
				}
			}
		}

		if (bCancelSupply)
		{
			m_bIsGettingSupply = false;
			m_pSupplyEntity = nullptr;
			m_iSupplyEntityIdx = -1;
			m_bSupplyIsDispenser = false;
			m_bWaitingAtDispenser = false;
			if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_GETHEALTH ||
				G_NavEngine.m_eCurrentPriority == NAV_PRIO_GETAMMO ||
				G_NavEngine.m_eCurrentPriority == NAV_PRIO_FOLLOW_SUPPLY)
			{
				G_NavEngine.CancelPath();
			}
		}
	}
	else if (m_bIsGettingSupply && !ShouldSearchHealth(pLocal) && !ShouldSearchAmmo(pLocal))
	{
		// Supply entity was null but we no longer need supplies — clear flag
		m_bIsGettingSupply = false;
		m_pSupplyEntity = nullptr;
		m_iSupplyEntityIdx = -1;
		m_bSupplyIsDispenser = false;
		m_bWaitingAtDispenser = false;
	}

	// === Non-following supply path cancellation (NAV_PRIO_GETHEALTH/GETAMMO) ===
	// When NOT following a player, check if we should cancel an active health/ammo path
	// Ported from TF2 bot's CTFBotGetHealth::Update and CTFBotGetAmmo::Update
	if (!m_bIsGettingSupply && m_pSupplyEntity && m_iSupplyEntityIdx > 0 && G_NavEngine.IsPathing() &&
		(G_NavEngine.m_eCurrentPriority == NAV_PRIO_GETHEALTH || G_NavEngine.m_eCurrentPriority == NAV_PRIO_GETAMMO))
	{
		bool bCancelSupply = false;
		const char* szCancelReason = nullptr;

		// Item is gone — entity is invalid, dormant, or picked up (EF_NODRAW)
		if (!H::Entities->SafeIsEntityValid(m_pSupplyEntity, m_iSupplyEntityIdx) || m_pSupplyEntity->IsDormant() ||
			(m_pSupplyEntity->m_fEffects() & EF_NODRAW))
		{
			bCancelSupply = true;
			szCancelReason = "Supply item is gone";
		}

		// Dispenser became non-operational (sapped, building, carrying)
		if (!bCancelSupply && m_bSupplyIsDispenser)
		{
			auto pBuilding = m_pSupplyEntity->As<C_BaseObject>();
			if (pBuilding && (pBuilding->m_bPlacing() || pBuilding->m_bBuilding() || pBuilding->m_bHasSapper() || pBuilding->m_bCarryDeploy()))
			{
				bCancelSupply = true;
				szCancelReason = "Dispenser is no longer operational";
			}
		}

		// Health/ammo is full — we got what we needed
		if (!bCancelSupply && !ShouldSearchHealth(pLocal) && !ShouldSearchAmmo(pLocal))
		{
			bCancelSupply = true;
			szCancelReason = "Health/ammo already obtained";
		}

		// A medic is healing us — cancel health search
		if (!bCancelSupply && (pLocal->m_nPlayerCond() & (1 << 21)) &&
			G_NavEngine.m_eCurrentPriority == NAV_PRIO_GETHEALTH)
		{
			bCancelSupply = true;
			szCancelReason = "A Medic is healing me";
		}

		// Enemy is closer to the item
		if (!bCancelSupply && H::Entities->SafeIsEntityValid(m_pSupplyEntity, m_iSupplyEntityIdx))
		{
			Vec3 vSupplyPos = m_pSupplyEntity->m_vecOrigin();
			Vec3 vLocalPos = pLocal->m_vecOrigin();
			float flOurDist = vLocalPos.DistTo(vSupplyPos);

			const int nMaxClients = I::EngineClient->GetMaxClients();
			for (int i = 1; i <= nMaxClients; i++)
			{
				if (i == I::EngineClient->GetLocalPlayer())
					continue;
				auto pClientEntity = I::ClientEntityList->GetClientEntity(i);
				if (!pClientEntity)
					continue;
				auto pPlayer = pClientEntity->As<C_TFPlayer>();
				if (!pPlayer || !pPlayer->IsAlive() || pPlayer->m_iTeamNum() == pLocal->m_iTeamNum())
					continue;
				if (pPlayer->IsDormant())
					continue;

				float flEnemyDist = pPlayer->m_vecOrigin().DistTo(vSupplyPos);
				if (flEnemyDist < flOurDist * 0.8f)
				{
					bCancelSupply = true;
					szCancelReason = "An enemy is closer to the supply";
					break;
				}
			}
		}

		// Dispenser proximity behavior
		if (!bCancelSupply && m_bSupplyIsDispenser && H::Entities->SafeIsEntityValid(m_pSupplyEntity, m_iSupplyEntityIdx))
		{
			Vec3 vDispenserPos = m_pSupplyEntity->m_vecOrigin();
			Vec3 vLocalPos = pLocal->m_vecOrigin();
			float flDistToDispenser = vLocalPos.DistTo(vDispenserPos);

			if (flDistToDispenser < 75.0f)
			{
				Vec3 vStart = vLocalPos; vStart.z += PLAYER_CROUCHED_JUMP_HEIGHT;
				Vec3 vEnd = vDispenserPos; vEnd.z += 40.0f;
				bool bCanSee = H::AimUtils->TracePositionWorld(vStart, vEnd);

				if (bCanSee)
				{
					bool bInCombat = (G::nTargetIndex != -1);
					bool bCriticallyLow = ShouldSearchHealth(pLocal) &&
						(static_cast<float>(pLocal->m_iHealth()) / pLocal->GetMaxHealth() < 0.3f);

					if (bInCombat && !bCriticallyLow)
					{
						bCancelSupply = true;
						szCancelReason = "No time to wait at dispenser, must fight";
					}
					else
					{
						if (G_NavEngine.IsPathing())
						{
							G_NavEngine.CancelPath();
							pCmd->forwardmove = 0.0f;
							pCmd->sidemove = 0.0f;
						}
					}
				}
			}
		}

		if (bCancelSupply)
		{
			m_pSupplyEntity = nullptr;
			m_iSupplyEntityIdx = -1;
			m_bSupplyIsDispenser = false;
			m_bWaitingAtDispenser = false;
			G_NavEngine.CancelPath();
		}
	}

	// === Follow Teammates (ported from Amalgam's FollowBot) ===
	// Skip if we're currently getting supplies (don't let follow override supply path)
	if (!bHasObjective && !m_bIsGettingSupply && FollowTeammates(pLocal))
		bHasObjective = true;

	// === Follow Tagged Players (follow players with "Follow Player" tag) ===
	// Skip if we're currently getting supplies (don't let follow override supply path)
	if (!bHasObjective && !m_bIsGettingSupply && FollowTaggedPlayer(pLocal))
		bHasObjective = true;

	// === Supply search while following (ported from Amalgam's GetSupplies) ===
	// When following a player, supply search uses a HIGHER priority (NAV_PRIO_FOLLOW_SUPPLY)
	// so the follow path doesn't override it. m_bIsGettingSupply pauses the follow functions.
	// When NOT following, supply search uses normal priorities and doesn't need special handling.
	bool bIsFollowing = m_bIsFollowingTaggedPlayer || m_bIsFollowingTeammate || m_bIsGettingSupply || (G_NavEngine.m_eCurrentPriority == NAV_PRIO_FOLLOW_TEAMMATE) || (G_NavEngine.m_eCurrentPriority == NAV_PRIO_FOLLOW_TAGGED) || (G_NavEngine.m_eCurrentPriority == NAV_PRIO_FOLLOW_SUPPLY);

	// Supply search while following - uses NAV_PRIO_FOLLOW_SUPPLY (higher than follow)
	if (bIsFollowing && (ShouldSearchHealth(pLocal) || ShouldSearchAmmo(pLocal)))
	{
		// If we already have a valid supply entity we're pathing to, stick with it
		// instead of re-evaluating FindSupply every tick (causes flip-flop repathing)
		bool bNeedHealth = ShouldSearchHealth(pLocal);
		Vec3 vSupplyPos;

		bool bHasValidSupply = m_bIsGettingSupply && m_pSupplyEntity && m_iSupplyEntityIdx > 0 &&
			H::Entities->SafeIsEntityValid(m_pSupplyEntity, m_iSupplyEntityIdx) && !m_pSupplyEntity->IsDormant() &&
			!(m_pSupplyEntity->m_fEffects() & EF_NODRAW);

		if (bHasValidSupply)
		{
			vSupplyPos = m_pSupplyEntity->GetCenter();
			m_vTargetPos = vSupplyPos;
			bHasObjective = true;

			// Don't restart path if we're waiting at a dispenser
			if (!m_bWaitingAtDispenser)
			{
				// On repath timer, re-evaluate FindSupply to check if a closer supply appeared
				if (flCurTime >= m_flNextRepathTime)
				{
					Vec3 vNewSupplyPos;
					if (FindSupply(pLocal, vNewSupplyPos, bNeedHealth))
					{
						float flNewDist = vLocalPos.DistTo(vNewSupplyPos);
						float flCurDist = vLocalPos.DistTo(vSupplyPos);
						// Switch if new supply is significantly closer (at least 25% closer)
						if (flNewDist < flCurDist * 0.75f)
						{
							m_pSupplyEntity = m_pTargetEntity;
							m_iSupplyEntityIdx = m_pTargetEntity->entindex();
							m_bSupplyIsDispenser = (m_pSupplyEntity->GetClassId() == ETFClassIds::CObjectDispenser);
							vSupplyPos = vNewSupplyPos;
							m_vTargetPos = vSupplyPos;
							G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_FOLLOW_SUPPLY);
							m_flNextRepathTime = flCurTime + 0.5f;
						}
					}
				}

				if (!G_NavEngine.IsPathing() || G_NavEngine.m_eCurrentPriority < NAV_PRIO_FOLLOW_SUPPLY)
				{
					G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_FOLLOW_SUPPLY);
					m_flNextRepathTime = flCurTime + 0.5f;
				}
				else if (flCurTime >= m_flNextRepathTime)
				{
					float flDistFromDest = G_NavEngine.GetDestination().DistTo(vSupplyPos);
					if (flDistFromDest > 100.0f)
					{
						G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_FOLLOW_SUPPLY);
						m_flNextRepathTime = flCurTime + 0.5f;
					}
				}
			}
		}
		else if (FindSupply(pLocal, vSupplyPos, bNeedHealth) && IsReachable(pLocal, vSupplyPos))
		{
			m_bIsGettingSupply = true;
			m_vTargetPos = vSupplyPos;
			bHasObjective = true;

			if (!G_NavEngine.IsPathing() || G_NavEngine.m_eCurrentPriority < NAV_PRIO_FOLLOW_SUPPLY)
			{
				G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_FOLLOW_SUPPLY);
				m_flNextRepathTime = flCurTime + 0.5f;
			}
			else if (flCurTime >= m_flNextRepathTime)
			{
				float flDistFromDest = G_NavEngine.GetDestination().DistTo(vSupplyPos);
				if (flDistFromDest > 100.0f)
				{
					G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_FOLLOW_SUPPLY);
					m_flNextRepathTime = flCurTime + 0.5f;
				}
			}
		}
		else if (m_bIsGettingSupply)
		{
			// Can't find a supply within range but still flagged as getting supply
			// This means the supply was picked up or went out of range - resume follow
			m_bIsGettingSupply = false;
			m_pSupplyEntity = nullptr;
			m_iSupplyEntityIdx = -1;
			m_bSupplyIsDispenser = false;
			m_bWaitingAtDispenser = false;
		}
	}

	// === Normal supply search (when NOT following a player) ===
	bool bAllowSupplySearch = !bHasObjective && !bIsFollowing;
	if (bAllowSupplySearch && ShouldSearchHealth(pLocal))
	{
		// If we already have a valid supply entity, stick with it
		bool bHasValidSupply = m_pSupplyEntity && m_iSupplyEntityIdx > 0 &&
			H::Entities->SafeIsEntityValid(m_pSupplyEntity, m_iSupplyEntityIdx) && !m_pSupplyEntity->IsDormant() &&
			!(m_pSupplyEntity->m_fEffects() & EF_NODRAW);

		Vec3 vSupplyPos;
		if (bHasValidSupply)
		{
			vSupplyPos = m_pSupplyEntity->GetCenter();
			m_vTargetPos = vSupplyPos;
			bHasObjective = true;

			// Don't restart path if we're waiting at a dispenser
			if (!m_bWaitingAtDispenser)
			{
				// On repath timer, re-evaluate FindSupply to check if a closer supply appeared
				if (flCurTime >= m_flNextRepathTime)
				{
					Vec3 vNewSupplyPos;
					if (FindSupply(pLocal, vNewSupplyPos, true))
					{
						float flNewDist = vLocalPos.DistTo(vNewSupplyPos);
						float flCurDist = vLocalPos.DistTo(vSupplyPos);
						if (flNewDist < flCurDist * 0.75f)
						{
							m_pSupplyEntity = m_pTargetEntity;
							m_iSupplyEntityIdx = m_pTargetEntity->entindex();
							m_bSupplyIsDispenser = (m_pSupplyEntity->GetClassId() == ETFClassIds::CObjectDispenser);
							vSupplyPos = vNewSupplyPos;
							m_vTargetPos = vSupplyPos;
							G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_GETHEALTH);
							m_flNextRepathTime = flCurTime + 0.5f;
						}
					}
				}

				if (!G_NavEngine.IsPathing() || G_NavEngine.m_eCurrentPriority < NAV_PRIO_GETHEALTH)
				{
					G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_GETHEALTH);
					m_flNextRepathTime = flCurTime + 0.5f;
				}
				else if (flCurTime >= m_flNextRepathTime)
				{
					float flDistFromDest = G_NavEngine.GetDestination().DistTo(vSupplyPos);
					if (flDistFromDest > 100.0f)
					{
						G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_GETHEALTH);
						m_flNextRepathTime = flCurTime + 0.5f;
					}
				}
			}
		}
		else if (FindSupply(pLocal, vSupplyPos, true) && IsReachable(pLocal, vSupplyPos))
		{
			m_vTargetPos = vSupplyPos;
			bHasObjective = true;

			if (!G_NavEngine.IsPathing())
			{
				G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_GETHEALTH);
				m_flNextRepathTime = flCurTime + 0.5f;
			}
			else if (flCurTime >= m_flNextRepathTime)
			{
				float flDistFromDest = G_NavEngine.GetDestination().DistTo(vSupplyPos);
				if (flDistFromDest > 100.0f)
				{
					G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_GETHEALTH);
					m_flNextRepathTime = flCurTime + 0.5f;
				}
			}
		}
	}

	// Try to find objective if preference is enabled
	// But skip if we just escaped danger — don't walk right back into the sentry zone
	if (CFG::NavBot_CaptureObjectives && I::GlobalVars->curtime >= m_flEscapeCooldownEnd)
	{
		Vec3 vObjectivePos;
		if (FindObjective(pLocal, vObjectivePos))
		{
			// Validate objective position (not zero) and reachable via navmesh
			if (!vObjectivePos.IsZero() && IsReachable(pLocal, vObjectivePos))
			{
				m_vTargetPos = vObjectivePos;
				bHasObjective = true;

				// Only repath if:
				// 1. Not currently pathing, OR
				// 2. The objective entity changed (different target), OR
				// 3. The objective moved significantly (>200 units for moving objectives like payload)
				// Also throttle repathing (like Amalgam's Timer - don't path every tick)
				bool bNeedRepath = false;

				if (!G_NavEngine.IsPathing())
				{
					bNeedRepath = true;
				}
				else if (flCurTime >= m_flNextRepathTime)
				{
					// Check if objective entity changed or moved significantly
					float flDistFromDest = G_NavEngine.GetDestination().DistTo(vObjectivePos);
					if (flDistFromDest > 200.0f)
					{
						bNeedRepath = true;
					}
				}

				if (bNeedRepath)
				{
					G_NavEngine.NavTo(vObjectivePos, NAV_PRIO_CAPTURE);
					m_flNextRepathTime = flCurTime + 0.5f;
				}
			}
		}
	}

	// === Ammo search (lower priority than objectives, higher than wandering) ===
	// Only runs when NOT following a player (following ammo is handled above with NAV_PRIO_FOLLOW_SUPPLY)
	if (bAllowSupplySearch && !bHasObjective && ShouldSearchAmmo(pLocal))
	{
		// If we already have a valid supply entity, stick with it
		bool bHasValidSupply = m_pSupplyEntity && m_iSupplyEntityIdx > 0 &&
			H::Entities->SafeIsEntityValid(m_pSupplyEntity, m_iSupplyEntityIdx) && !m_pSupplyEntity->IsDormant() &&
			!(m_pSupplyEntity->m_fEffects() & EF_NODRAW);

		Vec3 vSupplyPos;
		if (bHasValidSupply)
		{
			vSupplyPos = m_pSupplyEntity->GetCenter();
			m_vTargetPos = vSupplyPos;
			bHasObjective = true;

			// Don't restart path if we're waiting at a dispenser
			if (!m_bWaitingAtDispenser)
			{
				// On repath timer, re-evaluate FindSupply to check if a closer supply appeared
				if (flCurTime >= m_flNextRepathTime)
				{
					Vec3 vNewSupplyPos;
					if (FindSupply(pLocal, vNewSupplyPos, false))
					{
						float flNewDist = vLocalPos.DistTo(vNewSupplyPos);
						float flCurDist = vLocalPos.DistTo(vSupplyPos);
						if (flNewDist < flCurDist * 0.75f)
						{
							m_pSupplyEntity = m_pTargetEntity;
							m_iSupplyEntityIdx = m_pTargetEntity->entindex();
							m_bSupplyIsDispenser = (m_pSupplyEntity->GetClassId() == ETFClassIds::CObjectDispenser);
							vSupplyPos = vNewSupplyPos;
							m_vTargetPos = vSupplyPos;
							G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_GETAMMO);
							m_flNextRepathTime = flCurTime + 0.5f;
						}
					}
				}

				if (!G_NavEngine.IsPathing() || G_NavEngine.m_eCurrentPriority < NAV_PRIO_GETAMMO)
				{
					G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_GETAMMO);
					m_flNextRepathTime = flCurTime + 0.5f;
				}
				else if (flCurTime >= m_flNextRepathTime)
				{
					float flDistFromDest = G_NavEngine.GetDestination().DistTo(vSupplyPos);
					if (flDistFromDest > 100.0f)
					{
						G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_GETAMMO);
						m_flNextRepathTime = flCurTime + 0.5f;
					}
				}
			}
		}
		else if (FindSupply(pLocal, vSupplyPos, false) && IsReachable(pLocal, vSupplyPos))
		{
			m_vTargetPos = vSupplyPos;
			bHasObjective = true;

			if (!G_NavEngine.IsPathing())
			{
				G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_GETAMMO);
				m_flNextRepathTime = flCurTime + 0.5f;
			}
			else if (flCurTime >= m_flNextRepathTime)
			{
				float flDistFromDest = G_NavEngine.GetDestination().DistTo(vSupplyPos);
				if (flDistFromDest > 100.0f)
				{
					G_NavEngine.NavTo(vSupplyPos, NAV_PRIO_GETAMMO);
					m_flNextRepathTime = flCurTime + 0.5f;
				}
			}
		}
	}

	// === StayNear - stalk enemy players (ported from Amalgam's StayNear) ===
	// Higher priority than wandering, lower than ammo
	if (!bHasObjective && CFG::NavBot_StalkEnemies)
	{
		if (StayNear(pLocal))
		{
			bHasObjective = true;
		}
	}

	// === Sniper Spot - move to navmesh sniper sightline spots (ported from TF2 bot's CTFBotSniperLurk) ===
	// Same priority as wandering (PATROL) — only activates for sniper class or when scoped.
	// Does NOT override follow tagged, objectives, supply, or escape.
	if (!bHasObjective && CFG::NavBot_SniperSpots)
	{
		if (SniperSpot(pLocal))
		{
			bHasObjective = true;
		}
	}

	// If no objective found and wandering is enabled, wander (like Amalgam's Roam)
	// But DON'T wander if we have a follow target — the player might just be out of range temporarily.
	// When they come back in range, the follow functions will pick them up.
	if (!bHasObjective && CFG::NavBot_WanderWhenIdle && !bIsFollowing)
	{
		// If we're already pathing (even with higher priority like StayNear), keep following it
		// Don't try to override higher-priority paths with PATROL - that causes spam
		if (G_NavEngine.IsPathing() && G_NavEngine.m_eCurrentPriority > NAV_PRIO_PATROL)
		{
			// Following a higher-priority path (StayNear, etc.) - just keep going
		}
		else if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_PATROL && G_NavEngine.IsPathing())
		{
			// Already wandering to a target - just keep going
		}
		else
		{
			// Need a new wander target - but only check every 0.5s (like Amalgam's tRoamTimer)
			if (flCurTime >= m_flNextWanderTime || m_vWanderTarget.IsZero())
			{
				// PickRandomWanderTarget validates the path via NavTo internally
				// so we don't need to call NavTo again here
				m_flNextWanderTime = flCurTime + 0.5f;
				PickRandomWanderTarget(pLocal);

				if (!m_vWanderTarget.IsZero())
					m_vTargetPos = m_vWanderTarget;
			}
		}
	}

	// Follow path for both objectives and wandering
	if (G_NavEngine.IsNavMeshLoaded())
	{
		// Use nav mesh pathfinding
		if (G_NavEngine.IsPathing())
		{
			G_NavEngine.FollowPath(pLocal, pCmd);

			// LookAtPath - make the bot look in the direction it's walking (like Amalgam)
			// Pause when aimbot has a target and is using smooth aim —
			// smooth aimbot sets viewangles via SetViewAngles, which LookAtPath would override
			bool bAimbotHasTarget = (G::nTargetIndex != -1);
			bool bUsingSmoothAim = (CFG::Aimbot_Hitscan_Aim_Type == 2 || CFG::Aimbot_Melee_Aim_Type == 2);

			if (CFG::NavBot_LookAtPath && !(bAimbotHasTarget && bUsingSmoothAim))
			{
				// Look at the next immediate waypoint, not the final destination
				Vec3 vCurrentWaypoint = G_NavEngine.GetCurrentWaypoint();
				LookAtPath(pLocal, pCmd, vCurrentWaypoint);
			}
		}
	}

	// === Auto Scope (ported from Amalgam's BotUtils::AutoScope) ===
	// Scopes in when an enemy is visible to the sniper, unscopes after cancel time
	{
		const auto pWeapon = H::Entities->GetWeapon();
		if (pLocal && pWeapon)
			AutoScope(pLocal, pWeapon, pCmd);
	}

	if (!bHasObjective && CFG::NavBot_WanderWhenIdle && !G_NavEngine.IsPathing())
	{
		// Fallback: Simple direct movement without nav mesh (wander only)
		Vec3 vDirection = m_vWanderTarget - vLocalPos;
		vDirection.z = 0.0f;
		float flDist = vDirection.Length();

		if (flDist > 1.0f)
		{
			vDirection.Normalize();

			// Get view angles
			Vec3 vViewAngles = pCmd->viewangles;

			// Convert direction to forward/side move
			Vec3 vForward, vRight;
			Math::AngleVectors(vViewAngles, &vForward, &vRight, nullptr);
			vForward.z = 0.0f;
			vRight.z = 0.0f;
			vForward.Normalize();
			vRight.Normalize();

			// Calculate movement commands
			float flForwardMove = vDirection.Dot(vForward) * 450.0f;
			float flSideMove = vDirection.Dot(vRight) * 450.0f;

			pCmd->forwardmove = flForwardMove;
			pCmd->sidemove = flSideMove;

			// Auto jump if config enabled
			if (CFG::NavBot_AutoJump)
			{
				float flHeightDiff = m_vWanderTarget.z - vLocalPos.z;
				if (flHeightDiff > 18.0f && pLocal->m_fFlags() & FL_ONGROUND)
				{
					pCmd->buttons |= IN_JUMP;
				}
			}
		}
	}

	m_bActive = true;
}

void CNavBot::Draw()
{
	if (!m_bActive)
		return;
	
	// Draw target position
	if (!m_vTargetPos.IsZero())
	{
		Vec3 vDrawPos = m_vTargetPos;
		vDrawPos.z += 64.0f;
		
		Vec3 vScreen;
		if (H::Draw->W2S(vDrawPos, vScreen))
		{
			Color_t color = Color_t(0, 255, 0, 255);
			const char* szLabel = (m_pTargetEntity != nullptr) ? "OBJECTIVE" : "WANDER";
			H::Draw->String(H::Fonts->Get(EFonts::ESP), static_cast<int>(vScreen.x), static_cast<int>(vScreen.y), color, POS_CENTERXY, szLabel);
		}
	}
	
	// Draw path waypoints if enabled
	if (CFG::NavBot_DrawWaypoints && G_NavEngine.IsPathing())
	{
		const auto& vCrumbs = G_NavEngine.GetCrumbs();
		
		// Draw lines between crumbs
		for (size_t i = 0; i < vCrumbs.size() - 1; i++)
		{
			Vec3 vScreen1, vScreen2;
			if (H::Draw->W2S(vCrumbs[i].m_vPos, vScreen1) && H::Draw->W2S(vCrumbs[i + 1].m_vPos, vScreen2))
			{
				H::Draw->Line(static_cast<int>(vScreen1.x), static_cast<int>(vScreen1.y),
					static_cast<int>(vScreen2.x), static_cast<int>(vScreen2.y),
					Color_t(255, 255, 0, 255));
			}
		}
		
		// Draw crumb markers
		for (const auto& tCrumb : vCrumbs)
		{
			Vec3 vScreen;
			if (H::Draw->W2S(tCrumb.m_vPos, vScreen))
			{
				H::Draw->String(H::Fonts->Get(EFonts::ESP), static_cast<int>(vScreen.x), static_cast<int>(vScreen.y),
					Color_t(255, 255, 0, 255), POS_CENTERXY, "o");
			}
		}
	}
}

void CNavBot::Stop()
{
	m_bActive = false;
	m_pTargetEntity = nullptr;
	m_vTargetPos = Vec3(0, 0, 0);
	m_vWanderTarget = Vec3(0, 0, 0);
	m_vStuckCheckPos = Vec3(0, 0, 0);
	m_nStuckCheckTicks = 0;
	m_flNextWanderTime = 0.0f;
	m_flNextRepathTime = 0.0f;
	m_nStuckCount = 0;
	m_nStuckJumpCooldown = 0;
	m_bStuckWiggle = false;
	m_vLastTickPos = Vec3(0, 0, 0);
	m_bHasLastTickPos = false;
	m_bIsGettingSupply = false;
	m_pSupplyEntity = nullptr;
	m_iSupplyEntityIdx = -1;
	m_bSupplyIsDispenser = false;
	m_bWaitingAtDispenser = false;
	m_vVisitedAreaCenters.clear();
	m_nConsecutiveFails = 0;
	m_flNextVisitedClearTime = 0.0f;
	m_iStayNearTargetIdx = -1;
	m_flNextStayNearCheck = 0.0f;
	m_vSniperSpotPos = Vec3(0, 0, 0);
	m_flSniperPatienceEnd = 0.0f;
	m_flNextSniperSearchTime = 0.0f;
	m_tDormantPositions.clear();
	m_tClosestEnemy = {};
	m_vLastAngles = Vec3(0, 0, 0);
	m_bIsFollowingTaggedPlayer = false;
	m_iFollowTaggedTargetIdx = -1;
	m_bIsFollowingTeammate = false;
	m_iFollowTargetIdx = -1;
	m_bIsGettingSupply = false;
	G_NavEngine.CancelPath();
}

// === LookAtPath - smooth view angle control while pathing ===
// Ported from Amalgam's BotUtils::LookAtPath + DoSlowAim
// Makes the bot look where it's going instead of staring at a wall
void CNavBot::LookAtPath(C_TFPlayer* pLocal, CUserCmd* pCmd, const Vec3& vTarget)
{
	if (!pLocal || !pCmd)
		return;

	Vec3 vEye = pLocal->GetEyePosition();

	// Adjust target height for terrain-following look (like Amalgam)
	// When looking at the ground ahead, don't look straight down
	const float flHeightDelta = std::clamp(vTarget.z - vEye.z, -72.0f, 96.0f);
	const float flPitchFactor = flHeightDelta >= 0.0f ? 0.55f : 0.22f;
	Vec3 vFocus(vTarget.x, vTarget.y, vEye.z + flHeightDelta * flPitchFactor + 6.0f);

	Vec3 vDesired = Math::CalcAngle(vEye, vFocus);
	Math::ClampAngles(vDesired);

	// Smooth interpolation (like Amalgam's DoSlowAim)
	float flSpeed = std::max(1.0f, CFG::NavBot_LookSpeed);
	Vec3 vWish = vDesired;

	// Yaw smoothing
	if (m_vLastAngles.y != vWish.y)
	{
		Vec3 vDelta = vWish - m_vLastAngles;
		// Normalize yaw delta
		while (vDelta.y > 180.0f) vDelta.y -= 360.0f;
		while (vDelta.y < -180.0f) vDelta.y += 360.0f;
		vDelta /= flSpeed;
		vWish = m_vLastAngles + vDelta;
		Math::ClampAngles(vWish);
	}

	// Apply view angles
	pCmd->viewangles = vWish;
	I::EngineClient->SetViewAngles(vWish);
	m_vLastAngles = vWish;
}

// === GetDormantOrigin - get last known position of dormant players ===
// Ported from Amalgam's BotUtils::GetDormantOrigin
// Caches positions when players go dormant so we can still track them
bool CNavBot::GetDormantOrigin(int iEntIndex, Vec3& vOut)
{
	if (iEntIndex <= 0)
		return false;

	auto pClientEntity = I::ClientEntityList->GetClientEntity(iEntIndex);
	if (!pClientEntity)
		return false;

	auto pEntity = pClientEntity->As<C_BaseEntity>();
	if (!pEntity)
		return false;

	// If it's a player, check alive
	auto pPlayer = pEntity->As<C_TFPlayer>();
	if (pPlayer && !pPlayer->IsAlive())
	{
		// Invalidate dormant cache for dead players
		m_tDormantPositions.erase(iEntIndex);
		return false;
	}

	// If not dormant, return current origin and update cache
	if (!pEntity->IsDormant())
	{
		vOut = pEntity->m_vecOrigin();
		m_tDormantPositions[iEntIndex] = { vOut, I::GlobalVars->curtime, true };
		return true;
	}

	// If dormant, check cache
	auto it = m_tDormantPositions.find(iEntIndex);
	if (it != m_tDormantPositions.end() && it->second.m_bValid)
	{
		// Only use cached position if it's recent (within 5 seconds)
		if (I::GlobalVars->curtime - it->second.m_flTime < 5.0f)
		{
			vOut = it->second.m_vOrigin;
			return true;
		}
	}

	return false;
}

// === UpdateCloseEnemies - find and track nearest enemies ===
// Ported from Amalgam's BotUtils::UpdateCloseEnemies
void CNavBot::UpdateCloseEnemies(C_TFPlayer* pLocal)
{
	if (!pLocal)
		return;

	m_tClosestEnemy = {};

	for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PLAYERS_ENEMIES))
	{
		auto pPlayer = pEntity->As<C_TFPlayer>();
		if (!pPlayer || !pPlayer->IsAlive())
			continue;

		int iEntIndex = pPlayer->entindex();
		Vec3 vOrigin;
		if (!GetDormantOrigin(iEntIndex, vOrigin))
			continue;

		float flDist = pLocal->m_vecOrigin().DistTo(vOrigin);
		if (flDist < m_tClosestEnemy.m_flDist)
		{
			m_tClosestEnemy = { iEntIndex, pPlayer, flDist };
		}
	}
}

// === Sniper Spot - move to navmesh sniper sightline spots ===
// Ported from TF2 bot's CTFBotSniperLurk behavior
// Uses CHidingSpot IDEAL_SNIPER_SPOT/GOOD_SNIPER_SPOT flags generated by navmesh analysis.
// TF2 sniper bot: picks a vantage spot, stays there for a patience duration, then picks a new one.
// This is a low-priority idle behavior — does NOT override follow, objectives, supply, or escape.
bool CNavBot::SniperSpot(C_TFPlayer* pLocal)
{
	if (!pLocal || !G_NavEngine.IsNavMeshLoaded())
		return false;

	// Only use sniper spots when holding a sniper rifle
	auto pWeapon = pLocal->m_hActiveWeapon();
	if (!pWeapon)
		return false;

	// Only activate for sniper class or when actively scoped
	bool bIsSniperClass = (pLocal->m_iClass() == TF_CLASS_SNIPER);
	bool bIsScoped = pLocal->InCond(TF_COND_ZOOMED);
	if (!bIsSniperClass && !bIsScoped)
		return false;

	float flCurTime = I::GlobalVars->curtime;

	// If we have a valid sniper spot and haven't exceeded patience, keep it
	if (!m_vSniperSpotPos.IsZero() && flCurTime < m_flSniperPatienceEnd)
	{
		// Check if we've reached the spot — if so, just hold position
		Vec3 vLocalPos = pLocal->m_vecOrigin();
		float flDistToSpot = vLocalPos.DistTo(m_vSniperSpotPos);

		if (flDistToSpot < 50.0f)
		{
			// At the sniper spot — hold position, don't wander away
			// TF2 bot: "m_isHomePositionValid = true" — stays at home position until patience expires
			m_vTargetPos = m_vSniperSpotPos;
			return true;
		}

		// Still pathing to the spot — keep going
		if (G_NavEngine.IsPathing() && G_NavEngine.m_eCurrentPriority >= NAV_PRIO_PATROL)
			return true;

		// Lost our path — re-path (only if still reachable)
		if (!IsReachable(pLocal, m_vSniperSpotPos))
			return false;
		G_NavEngine.NavTo(m_vSniperSpotPos, NAV_PRIO_PATROL);
		return true;
	}

	// Patience expired or no spot — search for a new one (throttled)
	if (flCurTime < m_flNextSniperSearchTime)
		return false;

	m_flNextSniperSearchTime = flCurTime + 2.0f;  // Search every 2s

	Vec3 vLocalPos = pLocal->m_vecOrigin();
	int iTeam = pLocal->m_iTeamNum();

	// Find a sniper spot — prefer ideal (long sightlines), fall back to good
	Vec3 vSpot = G_NavEngine.m_pMap->FindSniperSpot(vLocalPos, 2000.0f, iTeam);
	if (vSpot.IsZero())
		return false;  // No sniper spots on this map or within range

	// Don't go to the same spot we just came from
	if (!m_vSniperSpotPos.IsZero() && vSpot.DistTo(m_vSniperSpotPos) < 100.0f)
		return false;

	m_vSniperSpotPos = vSpot;
	// TF2 bot patience: tf_bot_sniper_patience_duration = 10s
	m_flSniperPatienceEnd = flCurTime + 10.0f;

	m_vTargetPos = vSpot;
	if (!IsReachable(pLocal, vSpot))
		return false;
	G_NavEngine.NavTo(vSpot, NAV_PRIO_PATROL);
	return true;
}

// === StayNear - stalk enemy players ===
// Ported from Amalgam's NavBotJobs/StayNear.cpp
// Finds a nearby enemy and navigates to a position near them (not too close, not too far)
bool CNavBot::StayNear(C_TFPlayer* pLocal)
{
	if (!pLocal || !G_NavEngine.IsNavMeshLoaded())
		return false;

	float flCurTime = I::GlobalVars->curtime;

	// Throttle how often we check for new targets (like Amalgam's Timer)
	if (flCurTime < m_flNextStayNearCheck)
	{
		// Still tracking existing target
		if (m_iStayNearTargetIdx > 0)
		{
			Vec3 vOrigin;
			if (GetDormantOrigin(m_iStayNearTargetIdx, vOrigin))
			{
				// Repath to target if needed
				if (G_NavEngine.IsPathing() && flCurTime >= m_flNextRepathTime)
				{
					float flDistFromDest = G_NavEngine.GetDestination().DistTo(vOrigin);
					if (flDistFromDest > 200.0f)
					{
						G_NavEngine.NavTo(vOrigin, NAV_PRIO_STAYNEAR);
						m_flNextRepathTime = flCurTime + 0.5f;
					}
				}
				return true;
			}
			// Target lost
			m_iStayNearTargetIdx = -1;
		}
		return false;
	}

	m_flNextStayNearCheck = flCurTime + 0.3f; // Check every 0.3s (like Amalgam's cooldown)

	// Find best enemy to stalk - prefer closest non-dormant enemy
	// Use 2D distance so we don't pick an enemy on a different floor
	C_TFPlayer* pBestTarget = nullptr;
	float flBestDist2D = FLT_MAX;
	int iBestIdx = -1;

	for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PLAYERS_ENEMIES))
	{
		auto pPlayer = pEntity->As<C_TFPlayer>();
		if (!pPlayer || !pPlayer->IsAlive())
			continue;

		int iEntIndex = pPlayer->entindex();
		Vec3 vOrigin;
		if (!GetDormantOrigin(iEntIndex, vOrigin))
			continue;

		Vec3 vDelta = pLocal->m_vecOrigin() - vOrigin;
		float flDist2D = vDelta.Length2D();

		// Prefer non-dormant enemies (they're visible and we can track them better)
		if (pPlayer->IsDormant() && flBestDist2D < FLT_MAX)
			continue;

		if (flDist2D < flBestDist2D)
		{
			flBestDist2D = flDist2D;
			pBestTarget = pPlayer;
			iBestIdx = iEntIndex;
		}
	}

	if (!pBestTarget || iBestIdx < 0)
		return false;

	// Get target position
	Vec3 vTargetPos;
	if (!GetDormantOrigin(iBestIdx, vTargetPos))
		return false;

	// Don't stalk enemies inside enemy spawn rooms (ported from Amalgam's spawn room checks)
	{
		auto pTargetArea = G_NavEngine.m_pMap->FindClosestNavArea(vTargetPos);
		if (pTargetArea)
		{
			int iLocalTeam = pLocal->m_iTeamNum();
			bool bInEnemySpawn = (iLocalTeam == TF_TEAM_RED && (pTargetArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_BLUE)) ||
								(iLocalTeam == TF_TEAM_BLUE && (pTargetArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_RED));
			if (bInEnemySpawn)
				return false;  // Skip this target - they're in their spawn room
		}
	}

	// Class-based distance config (ported from Amalgam's CONFIG_SHORT/MID/LONG_RANGE)
	float flMinDist = 300.0f;  // Don't get closer than this
	float flMaxDist = 1000.0f;  // Don't go further than this

	switch (pLocal->m_iClass())
	{
	case TF_CLASS_SCOUT:
	case TF_CLASS_HEAVYWEAPONS:
		flMinDist = 200.0f;
		flMaxDist = 600.0f;
		break;
	case TF_CLASS_SNIPER:
		flMinDist = 600.0f;
		flMaxDist = 1500.0f;
		break;
	case TF_CLASS_ENGINEER:
	case TF_CLASS_MEDIC:
		flMinDist = 300.0f;
		flMaxDist = 800.0f;
		break;
	default:
		flMinDist = 300.0f;
		flMaxDist = 1000.0f;
		break;
	}

	// Already at good distance from target (2D + height check)
	// Must be on the same floor to count as "good distance"
	float flHeightDiff = std::abs(pLocal->m_vecOrigin().z - vTargetPos.z);
	bool bOnSameFloor = flHeightDiff < 72.0f;

	if (flBestDist2D >= flMinDist && flBestDist2D <= flMaxDist && bOnSameFloor)
	{
		m_iStayNearTargetIdx = iBestIdx;
		m_pTargetEntity = pBestTarget;
		m_vTargetPos = vTargetPos;

		// Repath if target moved significantly
		if (G_NavEngine.IsPathing() && flCurTime >= m_flNextRepathTime)
		{
			float flDistFromDest = G_NavEngine.GetDestination().DistTo(vTargetPos);
			if (flDistFromDest > 200.0f)
			{
				if (!IsReachable(pLocal, vTargetPos))
					return false;
				G_NavEngine.NavTo(vTargetPos, NAV_PRIO_STAYNEAR);
				m_flNextRepathTime = flCurTime + 0.5f;
			}
		}
		else if (!G_NavEngine.IsPathing())
		{
			if (!IsReachable(pLocal, vTargetPos))
				return false;
			G_NavEngine.NavTo(vTargetPos, NAV_PRIO_STAYNEAR);
			m_flNextRepathTime = flCurTime + 0.5f;
		}

		return true;
	}

	// Too close or too far - navigate to a good position near them
	// Use nav area center for pathing destination to respect floors
	if (G_NavEngine.IsNavMeshLoaded())
	{
		CNavArea* pTargetArea = G_NavEngine.m_pMap->FindClosestNavArea(vTargetPos);
		if (pTargetArea)
		{
			Vec3 vAreaCenter = pTargetArea->m_vCenter;
			vAreaCenter.z = vTargetPos.z;
			vTargetPos = vAreaCenter;
		}
	}

	m_iStayNearTargetIdx = iBestIdx;
	m_pTargetEntity = pBestTarget;
	m_vTargetPos = vTargetPos;

	if (!IsReachable(pLocal, vTargetPos))
		return false;

	if (!G_NavEngine.IsPathing() || flCurTime >= m_flNextRepathTime)
	{
		G_NavEngine.NavTo(vTargetPos, NAV_PRIO_STAYNEAR);
		m_flNextRepathTime = flCurTime + 0.5f;
	}

	return true;
}

// === UpdateDangerBlacklist - pre-compute danger zones on nav areas ===
// Ported from Amalgam's UpdateEnemyBlacklist + Map::UpdateIgnores
void CNavBot::UpdateDangerBlacklist(C_TFPlayer* pLocal)
{
	if (!pLocal || !G_NavEngine.IsNavMeshLoaded())
		return;

	// Check if any danger type is enabled
	if (!CFG::NavBot_DangerBL_Players && !CFG::NavBot_DangerBL_Sentries &&
		!CFG::NavBot_DangerBL_Stickies)
		return;

	static float flLastUpdateTime = 0.0f;
	float flCurTime = I::GlobalVars->curtime;
	if (flCurTime - flLastUpdateTime < 1.0f)
		return;
	flLastUpdateTime = flCurTime;

	auto pBlacklist = G_NavEngine.GetDangerBlacklist();
	if (!pBlacklist) return;
	G_NavEngine.ClearDangerBlacklist();

	bool bStrongClass = (pLocal->m_iClass() == TF_CLASS_HEAVYWEAPONS || pLocal->m_iClass() == TF_CLASS_SOLDIER);

	// === Enemy players ===
	if (CFG::NavBot_DangerBL_Players)
	{
		if (!CFG::NavBot_DangerBL_DormantThreats)
			G_NavEngine.ClearDangerBlacklist(DANGER_ENEMY_DORMANT);

		if (CFG::NavBot_DangerBL_NormalThreats)
		{
			for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PLAYERS_ENEMIES))
			{
				auto pPlayer = pEntity->As<C_TFPlayer>();
				if (!pPlayer || !pPlayer->IsAlive()) continue;

				bool bDormant = pPlayer->IsDormant();
				Vec3 vOrigin;
				if (bDormant) {
					if (!CFG::NavBot_DangerBL_DormantThreats) continue;
					if (!GetDormantOrigin(pPlayer->entindex(), vOrigin)) continue;
				}
				else vOrigin = pPlayer->m_vecOrigin();
				vOrigin.z += PLAYER_CROUCHED_JUMP_HEIGHT;

				if (pPlayer->IsInvulnerable())
				{
					for (auto& tArea : G_NavEngine.m_pMap->m_navfile.m_vAreas)
					{
						if (tArea.m_vCenter.DistTo(vOrigin) < 1000.0f)
						{
							Vec3 vAreaPos = tArea.m_vCenter; vAreaPos.z += PLAYER_CROUCHED_JUMP_HEIGHT;
							if (H::AimUtils->TracePositionWorld(vOrigin, vAreaPos))
								pBlacklist->insert_or_assign(&tArea, DangerBlacklistEntry_t(DANGER_ENEMY_INVULN));
						}
					}
					continue;
				}

				EDangerReason eReason = bDormant ? DANGER_ENEMY_DORMANT : DANGER_ENEMY_NORMAL;
				float flDangerDist = bDormant ? 300.0f : 500.0f;
				for (auto& tArea : G_NavEngine.m_pMap->m_navfile.m_vAreas)
				{
					float flDist = tArea.m_vCenter.DistTo(vOrigin);
					if (flDist < flDangerDist)
					{
						Vec3 vAreaPos = tArea.m_vCenter; vAreaPos.z += PLAYER_CROUCHED_JUMP_HEIGHT;
						if (H::AimUtils->TracePositionWorld(vOrigin, vAreaPos))
						{
							auto it = pBlacklist->find(&tArea);
							if (it == pBlacklist->end() || GetDangerPenalty(eReason) > GetDangerPenalty(it->second.m_eReason))
								pBlacklist->insert_or_assign(&tArea, DangerBlacklistEntry_t(eReason));
						}
					}
				}
			}
		}
	}

	// === Enemy sentries ===
	if (CFG::NavBot_DangerBL_Sentries)
	{
		for (const auto pEntity : H::Entities->GetGroup(EEntGroup::BUILDINGS_ENEMIES))
		{
			if (!pEntity || pEntity->IsDormant()) continue;
			auto pBuilding = pEntity->As<C_BaseObject>();
			if (!pBuilding || pBuilding->m_bPlacing()) continue;
			if (pEntity->GetClassId() != ETFClassIds::CObjectSentrygun) continue;
			auto pSentry = pBuilding->As<C_ObjectSentrygun>();
			if (!pSentry) continue;
			if (pSentry->m_iState() == 0) continue;
			if (bStrongClass && (pSentry->m_bMiniBuilding() || pSentry->m_iUpgradeLevel() == 1)) continue;
			int iBullets = pSentry->m_iAmmoShells(); int iRockets = pSentry->m_iAmmoRockets();
			if (iBullets == 0 && (pSentry->m_iUpgradeLevel() != 3 || iRockets == 0)) continue;
			if (pSentry->m_bHasSapper() || pSentry->m_bBuilding() || pSentry->m_bCarryDeploy()) continue;

			Vec3 vSentryPos = pSentry->m_vecOrigin(); vSentryPos.z += PLAYER_CROUCHED_JUMP_HEIGHT;
			constexpr float flHighDangerRange = 900.0f, flMediumDangerRange = 1050.0f, flLowDangerRange = 1200.0f;

			for (auto& tArea : G_NavEngine.m_pMap->m_navfile.m_vAreas)
			{
				float flDist = tArea.m_vCenter.DistTo(vSentryPos);
				EDangerReason eReason = DANGER_NONE;
				if (flDist < flHighDangerRange) eReason = DANGER_SENTRY;
				else if (flDist < flMediumDangerRange) eReason = DANGER_SENTRY_MEDIUM;
				else if (flDist < flLowDangerRange && !bStrongClass) eReason = DANGER_SENTRY_LOW;
				if (eReason != DANGER_NONE)
				{
					Vec3 vAreaPos = tArea.m_vCenter; vAreaPos.z += PLAYER_CROUCHED_JUMP_HEIGHT;
					if (H::AimUtils->TracePositionWorld(vSentryPos, vAreaPos))
					{
						auto it = pBlacklist->find(&tArea);
						if (it == pBlacklist->end() || GetDangerPenalty(eReason) > GetDangerPenalty(it->second.m_eReason))
							pBlacklist->insert_or_assign(&tArea, DangerBlacklistEntry_t(eReason));
					}
				}
			}
		}
	}

	// === Enemy stickies ===
	if (CFG::NavBot_DangerBL_Stickies)
	{
		constexpr float flStickyRadius = 130.0f + HALF_PLAYER_WIDTH;
		constexpr int iStickyIgnoreTicks = 300;
		for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PROJECTILES_ENEMIES))
		{
			if (!pEntity) continue;
			if (pEntity->GetClassId() != ETFClassIds::CTFGrenadePipebombProjectile) continue;
			auto pSticky = pEntity->As<C_TFGrenadePipebombProjectile>();
			if (!pSticky || pSticky->m_iType() != TF_GL_MODE_REMOTE_DETONATE) continue;
			if (pSticky->m_vecVelocity().Length() > 1.0f) continue;
			Vec3 vStickyPos = pSticky->m_vecOrigin(); vStickyPos.z += PLAYER_JUMP_HEIGHT / 2.0f;
			int iExpireTick = I::GlobalVars->tickcount + iStickyIgnoreTicks;
			for (auto& tArea : G_NavEngine.m_pMap->m_navfile.m_vAreas)
			{
				if (tArea.m_vCenter.DistTo(vStickyPos) < flStickyRadius)
				{
					auto it = pBlacklist->find(&tArea);
					if (it == pBlacklist->end() || GetDangerPenalty(DANGER_STICKY) > GetDangerPenalty(it->second.m_eReason))
						pBlacklist->insert_or_assign(&tArea, DangerBlacklistEntry_t(DANGER_STICKY, 0.0f, iExpireTick));
				}
			}
		}
	}
}

// === EscapeDanger - flee from enemies/sentries when taking fire ===
// Ported from Amalgam's EscapeDanger.cpp with blacklist-based detection
// Key improvement: when escaping while on an objective (Capture), try to stay
// near the objective so the bot doesn't oscillate between escape and capture.
bool CNavBot::EscapeDanger(C_TFPlayer* pLocal)
{
	if (!pLocal || !G_NavEngine.IsNavMeshLoaded()) return false;
	if (!CFG::NavBot_EscapeDanger) return false;
	if (G_NavEngine.m_eCurrentPriority > NAV_PRIO_ESCAPE_DANGER) return false;

	auto pLocalArea = G_NavEngine.GetLocalNavArea();
	if (pLocalArea && (pLocalArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_RED ||
		pLocalArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_BLUE))
		return false;

	auto pBlacklist = G_NavEngine.GetDangerBlacklist();
	bool bInHighDanger = false, bInMediumDanger = false, bInLowDanger = false;

	if (pBlacklist && pLocalArea && pBlacklist->count(pLocalArea))
	{
		auto& entry = (*pBlacklist)[pLocalArea];
		if (entry.m_eReason == DANGER_BAD_BUILD_SPOT) return false;

		switch (entry.m_eReason)
		{
		case DANGER_SENTRY: case DANGER_STICKY: case DANGER_ENEMY_INVULN: bInHighDanger = true; break;
		case DANGER_SENTRY_MEDIUM: case DANGER_ENEMY_NORMAL: bInMediumDanger = true; break;
		case DANGER_SENTRY_LOW: case DANGER_ENEMY_DORMANT: bInLowDanger = true; break;
		default: break;
		}

		bool bShouldEscape = bInHighDanger || (bInMediumDanger && pLocal->m_iHealth() < pLocal->GetMaxHealth() * 0.5f);

		// Amalgam-style: don't escape from medium danger if on an important task (Capture, GetHealth, Engineer)
		bool bImportantTask = (G_NavEngine.m_eCurrentPriority == NAV_PRIO_CAPTURE ||
		                      G_NavEngine.m_eCurrentPriority == NAV_PRIO_GETHEALTH);
		if (!bInHighDanger && bImportantTask)
			bShouldEscape = false;

		if (bInLowDanger && !bInMediumDanger && !bInHighDanger && G_NavEngine.m_eCurrentPriority != NAV_PRIO_NONE)
			return false;

		if (!bShouldEscape)
		{
			if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_ESCAPE_DANGER)
			{
				G_NavEngine.CancelPath();
				// Set cooldown so Capture doesn't immediately walk back into danger
				m_flEscapeCooldownEnd = I::GlobalVars->curtime + 3.0f;
			}
			return false;
		}

		// Already escaping - validate that escape target is still safe AND we're making progress
		if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_ESCAPE_DANGER && m_pEscapeTargetArea)
		{
			if (!pBlacklist->count(m_pEscapeTargetArea))
			{
				float flDistToTarget = pLocal->m_vecOrigin().DistTo(m_pEscapeTargetArea->m_vCenter);
				if (flDistToTarget > 100.0f)
					return true;
			}
			// Target is now in danger or we're stuck - need to repath
		}

		// === Amalgam-style reference position ===
		// If we were pursuing an objective, try to stay near it while escaping.
		// This prevents the bot from fleeing far away, then walking right back into danger.
		Vec3 vReferencePosition = pLocal->m_vecOrigin();
		bool bHasObjective = false;

		if (G_NavEngine.m_eCurrentPriority != NAV_PRIO_NONE &&
		    G_NavEngine.m_eCurrentPriority != NAV_PRIO_ESCAPE_DANGER &&
		    G_NavEngine.IsPathing())
		{
			// Use the destination of our current path as the reference position
			vReferencePosition = G_NavEngine.GetDestination();
			bHasObjective = true;
			// Save it so we remember where we were going even after escape overrides the path
			m_vPreEscapeDestination = vReferencePosition;
		}
		else if (!m_vPreEscapeDestination.IsZero())
		{
			// We just started escaping — use the saved destination from before
			vReferencePosition = m_vPreEscapeDestination;
			bHasObjective = true;
		}

		// Find safe areas - prioritize getting AWAY from danger source while staying near objective
		Vec3 vDangerSource = pLocal->m_vecOrigin();
		if (m_tClosestEnemy.m_pPlayer && H::Entities->IsEntityValid(m_tClosestEnemy.m_pPlayer) && m_tClosestEnemy.m_flDist < FLT_MAX)
			vDangerSource = m_tClosestEnemy.m_pPlayer->m_vecOrigin();

		// === Cover spot priority (ported from TF2 bot's hiding spot usage) ===
		// TF2 bots use navmesh hiding spots (IN_COVER) to take cover from sentries
		// and hide behind walls when healing. Try to find a nearby cover spot first.
		if (G_NavEngine.m_pMap)
		{
			Vec3 vCoverSpot = G_NavEngine.m_pMap->FindCoverSpot(
				pLocal->m_vecOrigin(), 1500.0f, pLocal->m_iTeamNum());
			if (!vCoverSpot.IsZero())
			{
				// Make sure the cover spot isn't in the danger zone
				auto pCoverArea = G_NavEngine.m_pMap->FindClosestNavArea(vCoverSpot);
				if (pCoverArea && !pBlacklist->count(pCoverArea))
				{
					// Prefer cover spots that are away from the danger source
					float flDistFromDanger = vCoverSpot.DistTo(vDangerSource);
					if (flDistFromDanger > 200.0f)
					{
						if (G_NavEngine.NavTo(vCoverSpot, NAV_PRIO_ESCAPE_DANGER, true))
						{
							m_pEscapeTargetArea = pCoverArea;
							return true;
						}
					}
				}
			}
		}

		std::vector<std::pair<CNavArea*, float>> vSafeAreas;
		for (auto& tArea : G_NavEngine.m_pMap->m_navfile.m_vAreas)
		{
			Vec3 vAreaCenter = tArea.m_vCenter;
			float flDistToUs = vAreaCenter.DistTo(pLocal->m_vecOrigin());
			if (flDistToUs < 200.0f || flDistToUs > 1500.0f) continue;

			// Skip ALL blacklisted areas when escaping
			if (pBlacklist->count(&tArea)) continue;

			// Amalgam-style scoring: if we have an objective, prioritize staying near it
			// Otherwise, just get away from danger
			float flScore;
			if (bHasObjective)
			{
				// Score = distance to objective (closer is better, like Amalgam)
				flScore = vAreaCenter.DistTo(vReferencePosition);
			}
			else
			{
				// No objective — just get away from danger
				float flDistFromDanger = vAreaCenter.DistTo(vDangerSource);
				flScore = flDistToUs - (flDistFromDanger * 0.5f);
			}
			vSafeAreas.push_back({ &tArea, flScore });
		}

		std::sort(vSafeAreas.begin(), vSafeAreas.end(), [](const auto& a, const auto& b) { return a.second < b.second; });

		int iAttempts = 0;
		for (auto& tPair : vSafeAreas)
		{
			if (++iAttempts > 10) break;
			bool bIsSafe = true;
			for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PLAYERS_ENEMIES))
			{
				auto pPlayer = pEntity->As<C_TFPlayer>();
				if (!pPlayer || !pPlayer->IsAlive() || pPlayer->IsDormant()) continue;
				if (tPair.first->m_vCenter.DistTo(pPlayer->m_vecOrigin()) < 300.0f) { bIsSafe = false; break; }
			}
			if (!bIsSafe) continue;
			if (G_NavEngine.NavTo(tPair.first->m_vCenter, NAV_PRIO_ESCAPE_DANGER, true)) { m_pEscapeTargetArea = tPair.first; return true; }
		}

		// Fallback: try any completely safe area, sorted by distance from danger source
		if (bInHighDanger)
		{
			std::vector<std::pair<CNavArea*, float>> vFallbackAreas;
			for (auto& tArea : G_NavEngine.m_pMap->m_navfile.m_vAreas)
			{
				if (pBlacklist->count(&tArea)) continue;
				// If we have an objective, sort by distance to it (stay near objective)
				// Otherwise, sort by distance from danger (get far away)
				float flScore = bHasObjective
					? tArea.m_vCenter.DistTo(vReferencePosition)
					: -tArea.m_vCenter.DistTo(vDangerSource);
				vFallbackAreas.push_back({ &tArea, flScore });
			}
			std::sort(vFallbackAreas.begin(), vFallbackAreas.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
			int iFallbackAttempts = 0;
			for (auto& tPair : vFallbackAreas)
			{
				if (++iFallbackAttempts > 10) break;
				if (G_NavEngine.NavTo(tPair.first->m_vCenter, NAV_PRIO_ESCAPE_DANGER, true)) { m_pEscapeTargetArea = tPair.first; return true; }
			}
		}

		if (m_tClosestEnemy.m_pPlayer && H::Entities->IsEntityValid(m_tClosestEnemy.m_pPlayer) && m_tClosestEnemy.m_flDist < FLT_MAX)
		{
			Vec3 vEnemyPos = m_tClosestEnemy.m_pPlayer->m_vecOrigin();
			Vec3 vAwayDir = pLocal->m_vecOrigin() - vEnemyPos; vAwayDir.z = 0.0f;
			if (vAwayDir.Length() > 1.0f) { vAwayDir.Normalize(); Vec3 vFleeTarget = pLocal->m_vecOrigin() + vAwayDir * 500.0f; if (G_NavEngine.NavTo(vFleeTarget, NAV_PRIO_ESCAPE_DANGER, true)) return true; }
		}
	}
	else if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_ESCAPE_DANGER)
	{
		// No longer in danger — cancel escape path and set cooldown
		G_NavEngine.CancelPath();
		m_vPreEscapeDestination = Vec3(0, 0, 0);
		m_flEscapeCooldownEnd = I::GlobalVars->curtime + 3.0f;
	}

	return false;
}

// === EscapeProjectiles - flee from incoming rockets and stickies ===
// Ported from Amalgam's EscapeProjectiles
bool CNavBot::EscapeProjectiles(C_TFPlayer* pLocal)
{
	if (!pLocal || !G_NavEngine.IsNavMeshLoaded()) return false;
	if (!CFG::NavBot_DangerBL_Stickies && !CFG::NavBot_DangerBL_Projectiles) return false;
	if (G_NavEngine.m_eCurrentPriority > NAV_PRIO_ESCAPE_DANGER) return false;

	Vec3 vLocalPos = pLocal->m_vecOrigin();
	bool bUnsafe = false;

	for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PROJECTILES_ENEMIES))
	{
		if (!pEntity) continue;
		auto nClassId = pEntity->GetClassId();
		// Check for stickies
		if (CFG::NavBot_DangerBL_Stickies && nClassId == ETFClassIds::CTFGrenadePipebombProjectile)
		{
			auto pSticky = pEntity->As<C_TFGrenadePipebombProjectile>();
			if (!pSticky || pSticky->m_iType() != TF_GL_MODE_REMOTE_DETONATE) continue;
			if (pSticky->m_vecVelocity().Length() > 1.0f) continue;
			if (vLocalPos.DistTo(pSticky->m_vecOrigin()) < 200.0f) { bUnsafe = true; break; }
		}
		// Check for rockets and pipes
		if (CFG::NavBot_DangerBL_Projectiles && (nClassId == ETFClassIds::CTFProjectile_Rocket ||
			(nClassId == ETFClassIds::CTFGrenadePipebombProjectile &&
				pEntity->As<C_TFGrenadePipebombProjectile>()->m_iType() == TF_GL_MODE_REGULAR)))
		{
			if (vLocalPos.DistTo(pEntity->m_vecOrigin()) < 300.0f) { bUnsafe = true; break; }
		}
	}

	if (!bUnsafe)
	{
		if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_ESCAPE_DANGER) G_NavEngine.CancelPath();
		return false;
	}

	std::vector<std::pair<CNavArea*, float>> vSafeAreas;
	auto pDangerBlacklist = G_NavEngine.GetDangerBlacklist();
	for (auto& tArea : G_NavEngine.m_pMap->m_navfile.m_vAreas)
	{
		float flDist = tArea.m_vCenter.DistTo(vLocalPos);
		if (flDist > 1000.0f || flDist < 50.0f) continue;
		if (pDangerBlacklist && pDangerBlacklist->count(&tArea)) continue;
		bool bAreaSafe = true;
		for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PROJECTILES_ENEMIES))
		{
			if (!pEntity) continue;
			if (tArea.m_vCenter.DistTo(pEntity->m_vecOrigin()) < 200.0f) { bAreaSafe = false; break; }
		}
		if (bAreaSafe) vSafeAreas.push_back({ &tArea, flDist });
	}

	std::sort(vSafeAreas.begin(), vSafeAreas.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
	for (auto& tPair : vSafeAreas)
	{
		if (G_NavEngine.NavTo(tPair.first->m_vCenter, NAV_PRIO_ESCAPE_DANGER, true)) return true;
	}
	return false;
}

// === EscapeSpawn - navigate out of spawn rooms ===
// Ported from Amalgam's EscapeSpawn
bool CNavBot::EscapeSpawn(C_TFPlayer* pLocal)
{
	if (!pLocal || !G_NavEngine.IsNavMeshLoaded()) return false;

	auto pLocalArea = G_NavEngine.GetLocalNavArea();
	if (!pLocalArea || !(pLocalArea->m_iTFAttributeFlags & (TF_NAV_SPAWN_ROOM_RED | TF_NAV_SPAWN_ROOM_BLUE)))
	{
		if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_ESCAPE_SPAWN) G_NavEngine.CancelPath();
		return false;
	}

	static float flLastSpawnEscapeTime = 0.0f;
	float flCurTime = I::GlobalVars->curtime;
	if (flCurTime - flLastSpawnEscapeTime < 2.0f)
		return G_NavEngine.m_eCurrentPriority == NAV_PRIO_ESCAPE_SPAWN;
	flLastSpawnEscapeTime = flCurTime;

	Vec3 vLocalPos = pLocal->m_vecOrigin();
	CNavArea* pClosestExit = nullptr;
	float flBestDist = FLT_MAX;

	// Use cached exit areas from UpdateRespawnRooms (more efficient)
	auto pExitAreas = G_NavEngine.GetRespawnRoomExitAreas();
	if (pExitAreas && !pExitAreas->empty())
	{
		for (auto pArea : *pExitAreas)
		{
			float flDist = pArea->m_vCenter.DistTo(vLocalPos);
			if (flDist < flBestDist) { flBestDist = flDist; pClosestExit = pArea; }
		}
	}
	else
	{
		// Fallback: scan all areas for TF_NAV_SPAWN_ROOM_EXIT flag
		for (auto& tArea : G_NavEngine.m_pMap->m_navfile.m_vAreas)
		{
			if (tArea.m_iTFAttributeFlags & (TF_NAV_SPAWN_ROOM_RED | TF_NAV_SPAWN_ROOM_BLUE)) continue;
			if (tArea.m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_EXIT)
			{
				float flDist = tArea.m_vCenter.DistTo(vLocalPos);
				if (flDist < flBestDist) { flBestDist = flDist; pClosestExit = &tArea; }
			}
		}
	}

	if (!pClosestExit)
	{
		for (auto& tArea : G_NavEngine.m_pMap->m_navfile.m_vAreas)
		{
			if (tArea.m_iTFAttributeFlags & (TF_NAV_SPAWN_ROOM_RED | TF_NAV_SPAWN_ROOM_BLUE)) continue;
			float flDist = tArea.m_vCenter.DistTo(vLocalPos);
			if (flDist < flBestDist) { flBestDist = flDist; pClosestExit = &tArea; }
		}
	}

	if (pClosestExit)
	{
		if (G_NavEngine.NavTo(pClosestExit->m_vCenter, NAV_PRIO_ESCAPE_SPAWN, true)) return true;
	}
	return false;
}

// === Weapon switching (ported from Amalgam's BotUtils::UpdateBestSlot and SetSlot) ===
// Determines the best weapon slot based on player class, ammo, and enemy distance
void CNavBot::UpdateBestSlot(C_TFPlayer* pLocal)
{
	if (!pLocal)
		return;

	m_iBestSlot = -1;

	// Weapon preference (ported from Amalgam's WeaponSlot enum)
	// 0=Off (don't switch), 1=Best (auto), 2=Primary, 3=Secondary, 4=Melee
	int iPreference = CFG::NavBot_WeaponPreference;

	if (iPreference == 0)
	{
		m_iBestSlot = -1;  // Off - don't switch weapons
		return;
	}

	if (iPreference != 1)
	{
		// Force a specific slot (2=Primary, 3=Secondary, 4=Melee)
		m_iBestSlot = iPreference - 2;  // Maps to WEAPON_SLOT_PRIMARY=0, SECONDARY=1, MELEE=2
		return;
	}

	// iPreference == 1: Best (auto-select based on class/ammo/distance)

	// Helper: get clip and reserve for a weapon slot
	auto fnGetClip = [&](int iSlot) -> int {
		auto pWep = pLocal->GetWeaponFromSlot(iSlot);
		if (!pWep) return 0;
		return pWep->m_iClip1();
	};
	auto fnGetReserve = [&](int iSlot) -> int {
		auto pWep = pLocal->GetWeaponFromSlot(iSlot);
		if (!pWep) return 0;
		return pLocal->GetAmmoCount(pWep->m_iPrimaryAmmoType());
	};

	float flEnemyDist = m_tClosestEnemy.m_flDist;
	bool bEnemyClose = m_tClosestEnemy.m_pPlayer && flEnemyDist <= 300.0f;

	int iPrimaryClip = fnGetClip(WEAPON_SLOT_PRIMARY);
	int iSecondaryClip = fnGetClip(WEAPON_SLOT_SECONDARY);

	switch (pLocal->m_iClass())
	{
	case TF_CLASS_SCOUT:
	{
		if (!iPrimaryClip && !iSecondaryClip && bEnemyClose)
			m_iBestSlot = WEAPON_SLOT_MELEE;
		else if (iSecondaryClip && (flEnemyDist > 750.0f || !iPrimaryClip))
			m_iBestSlot = WEAPON_SLOT_SECONDARY;
		else if (iPrimaryClip)
			m_iBestSlot = WEAPON_SLOT_PRIMARY;
		break;
	}
	case TF_CLASS_SOLDIER:
	{
		if (!iPrimaryClip && !iSecondaryClip && bEnemyClose)
			m_iBestSlot = WEAPON_SLOT_MELEE;
		else if (iSecondaryClip && flEnemyDist <= 350.0f && m_tClosestEnemy.m_pPlayer && m_tClosestEnemy.m_pPlayer->m_iHealth() <= 125)
			m_iBestSlot = WEAPON_SLOT_SECONDARY;
		else if (iPrimaryClip)
			m_iBestSlot = WEAPON_SLOT_PRIMARY;
		break;
	}
	case TF_CLASS_PYRO:
	{
		if (!iPrimaryClip && !iSecondaryClip && bEnemyClose)
			m_iBestSlot = WEAPON_SLOT_MELEE;
		else if (iPrimaryClip && flEnemyDist <= 400.0f)
			m_iBestSlot = WEAPON_SLOT_PRIMARY;
		else if (iSecondaryClip)
			m_iBestSlot = WEAPON_SLOT_SECONDARY;
		else if (iPrimaryClip)
			m_iBestSlot = WEAPON_SLOT_PRIMARY;
		break;
	}
	case TF_CLASS_DEMOMAN:
	{
		if (!iPrimaryClip && !iSecondaryClip && bEnemyClose)
			m_iBestSlot = WEAPON_SLOT_MELEE;
		else if (iPrimaryClip && flEnemyDist <= 800.0f)
			m_iBestSlot = WEAPON_SLOT_PRIMARY;
		else if (iSecondaryClip)
			m_iBestSlot = WEAPON_SLOT_SECONDARY;
		break;
	}
	case TF_CLASS_HEAVYWEAPONS:
	{
		if (!iPrimaryClip && !iSecondaryClip)
			m_iBestSlot = WEAPON_SLOT_MELEE;
		else if (iPrimaryClip)
			m_iBestSlot = WEAPON_SLOT_PRIMARY;
		break;
	}
	case TF_CLASS_ENGINEER:
	{
		if (!iPrimaryClip && !iSecondaryClip && bEnemyClose)
			m_iBestSlot = WEAPON_SLOT_MELEE;
		else if (iPrimaryClip && flEnemyDist <= 1000.0f)
			m_iBestSlot = WEAPON_SLOT_PRIMARY;
		else if (iSecondaryClip)
			m_iBestSlot = WEAPON_SLOT_SECONDARY;
		break;
	}
	case TF_CLASS_MEDIC:
	{
		auto pSecondary = pLocal->GetWeaponFromSlot(WEAPON_SLOT_SECONDARY);
		if (pSecondary)
			m_iBestSlot = WEAPON_SLOT_SECONDARY;  // Keep medigun out by default
		else if (!iPrimaryClip || bEnemyClose)
			m_iBestSlot = WEAPON_SLOT_MELEE;
		else
			m_iBestSlot = WEAPON_SLOT_PRIMARY;
		break;
	}
	case TF_CLASS_SNIPER:
	{
		if (!iPrimaryClip && !iSecondaryClip || (bEnemyClose && flEnemyDist <= 200.0f))
			m_iBestSlot = WEAPON_SLOT_MELEE;
		else if (iSecondaryClip && flEnemyDist <= 300.0f)
			m_iBestSlot = WEAPON_SLOT_SECONDARY;
		else if (iPrimaryClip)
			m_iBestSlot = WEAPON_SLOT_PRIMARY;
		break;
	}
	case TF_CLASS_SPY:
	{
		if (bEnemyClose)
			m_iBestSlot = WEAPON_SLOT_MELEE;  // Knife for backstabs
		else if (iPrimaryClip)
			m_iBestSlot = WEAPON_SLOT_PRIMARY;  // Revolver at range
		break;
	}
	default:
		break;
	}

	// Avoid Melee: when WeaponPreference=Best and AvoidMelee is checked, don't choose melee
	if (CFG::NavBot_AvoidMelee && m_iBestSlot == WEAPON_SLOT_MELEE)
	{
		if (fnGetClip(WEAPON_SLOT_PRIMARY) > 0)
			m_iBestSlot = WEAPON_SLOT_PRIMARY;
		else if (fnGetClip(WEAPON_SLOT_SECONDARY) > 0)
			m_iBestSlot = WEAPON_SLOT_SECONDARY;
		else
			m_iBestSlot = -1;
	}
}

void CNavBot::SwitchWeapon(C_TFPlayer* pLocal)
{
	if (!pLocal || !CFG::NavBot_SwitchWeapons)
		return;

	// Throttle weapon switching (like Amalgam's Timer - 0.2s)
	static float flLastSwitchTime = 0.0f;
	float flCurTime = I::GlobalVars->curtime;
	if (flCurTime - flLastSwitchTime < 0.2f)
		return;

	// Update current slot
	auto pActiveWep = pLocal->m_hActiveWeapon().Get();
	if (pActiveWep)
	{
		auto pWeaponBase = pActiveWep->As<C_TFWeaponBase>();
		m_iCurrentSlot = pWeaponBase ? pWeaponBase->GetSlot() : -1;
	}
	else
		m_iCurrentSlot = -1;

	// Determine best slot
	UpdateBestSlot(pLocal);

	// Switch if needed (like Amalgam's SetSlot)
	if (m_iBestSlot >= 0 && m_iCurrentSlot != m_iBestSlot)
	{
		auto sCommand = "slot" + std::to_string(m_iBestSlot + 1);
		I::EngineClient->ClientCmd_Unrestricted(sCommand.c_str());
		m_iCurrentSlot = m_iBestSlot;
		flLastSwitchTime = flCurTime;
	}
}

// === Follow Teammates (ported from Amalgam's FollowBot) ===
// Finds closest teammate and paths to them, following their movement
// Uses 2D distance + height awareness so the bot doesn't think it's "close"
// to a teammate on a different floor directly above/below.
bool CNavBot::FollowTeammates(C_TFPlayer* pLocal)
{
	if (!pLocal || !CFG::NavBot_FollowTeammates)
	{
		m_bIsFollowingTeammate = false;
		m_iFollowTargetIdx = -1;
		return false;
	}

	float flCurTime = I::GlobalVars->curtime;
	Vec3 vLocalPos = pLocal->m_vecOrigin();

	// Find closest valid teammate (using 2D distance for selection)
	C_TFPlayer* pBestTarget = nullptr;
	float flBestDist2D = FLT_MAX;

	for (int i = 1; i <= I::ClientEntityList->GetMaxEntities(); i++)
	{
		auto pEntity = I::ClientEntityList->GetClientEntity(i);
		if (!pEntity)
			continue;

		auto pPlayer = pEntity->As<C_TFPlayer>();
		if (!pPlayer || !pPlayer->IsAlive() || pPlayer->IsDormant() || pPlayer == pLocal)
			continue;

		// Only follow teammates
		if (pPlayer->m_iTeamNum() != pLocal->m_iTeamNum())
			continue;

		// Skip ghosts/taunting players
		if (IsAGhost(pPlayer))
			continue;

		// Skip teammates in spawn rooms (they're AFK or just spawned — following them gets us stuck)
		if (G_NavEngine.IsNavMeshLoaded())
		{
			CNavArea* pPlayerArea = G_NavEngine.m_pMap->FindClosestNavArea(pPlayer->m_vecOrigin());
			if (pPlayerArea)
			{
				int iLocalTeam = pLocal->m_iTeamNum();
				bool bInOwnSpawn = (iLocalTeam == TF_TEAM_RED && (pPlayerArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_RED)) ||
									(iLocalTeam == TF_TEAM_BLUE && (pPlayerArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_BLUE));
				if (bInOwnSpawn)
					continue;
			}
		}

		// Skip teammates near engineer buildings (they cluster around them and get us stuck)
		bool bNearBuilding = false;
		Vec3 vPlayerPos = pPlayer->m_vecOrigin();
		for (auto pBuildingEntity : H::Entities->GetGroup(EEntGroup::BUILDINGS_TEAMMATES))
		{
			if (!pBuildingEntity)
				continue;
			auto pBuilding = pBuildingEntity->As<C_BaseObject>();
			if (!pBuilding || pBuilding->m_bPlacing())
				continue;
			if (vPlayerPos.DistTo(pBuilding->m_vecOrigin()) < 300.0f)
			{
				bNearBuilding = true;
				break;
			}
		}
		if (bNearBuilding)
			continue;

		// Skip teammates standing on already-captured control points
		bool bOnCapturedCP = false;
		{
			C_BaseEntity* pObjectiveResource = nullptr;
			for (int j = 1; j <= I::ClientEntityList->GetMaxEntities(); j++)
			{
				auto pCE = I::ClientEntityList->GetClientEntity(j);
				if (!pCE) continue;
				auto pCN = pCE->GetClientNetworkable();
				if (!pCN) continue;
				auto pCC = pCN->GetClientClass();
				if (!pCC) continue;
				if (static_cast<ETFClassIds>(pCC->m_ClassID) == ETFClassIds::CTFObjectiveResource)
				{
					pObjectiveResource = pCE->As<C_BaseEntity>();
					break;
				}
			}
			if (pObjectiveResource)
			{
				static int nNumCPsOffset = NetVars::GetNetVar("CTFObjectiveResource", "m_iNumControlPoints");
				static int nCPPositionsOffset = NetVars::GetNetVar("CTFObjectiveResource", "m_vCPPositions[0]");
				static int nOwnerOffset = NetVars::GetNetVar("CTFObjectiveResource", "m_iOwner");
				if (nNumCPsOffset && nCPPositionsOffset && nOwnerOffset)
				{
					auto pBase = reinterpret_cast<std::uintptr_t>(pObjectiveResource);
					int nNumCPs = *reinterpret_cast<int*>(pBase + nNumCPsOffset);
					int nLocalTeam = pLocal->m_iTeamNum();
					for (int j = 0; j < nNumCPs && j < 8; j++)
					{
						int nOwner = *reinterpret_cast<int*>(pBase + nOwnerOffset + j * sizeof(int));
						if (nOwner == nLocalTeam)  // Already captured by our team
						{
							Vec3 vCPPos = *reinterpret_cast<Vec3*>(pBase + nCPPositionsOffset + j * sizeof(Vec3));
							if (vPlayerPos.DistTo(vCPPos) < 300.0f)
							{
								bOnCapturedCP = true;
								break;
							}
						}
					}
				}
			}
		}
		if (bOnCapturedCP)
			continue;

		// Use 2D distance for teammate selection (ignore Z so we don't
		// pick a teammate on a different floor just because they're "closer" in 3D)
		Vec3 vDelta = vLocalPos - pPlayer->m_vecOrigin();
		float flDist2D = vDelta.Length2D();
		if (flDist2D < flBestDist2D)
		{
			flBestDist2D = flDist2D;
			pBestTarget = pPlayer;
			m_iFollowTargetIdx = i;
		}
	}

	// No teammate found or too far away (2D)
	if (!pBestTarget || flBestDist2D > 1500.0f)
	{
		m_iFollowTargetIdx = -1;
		m_bIsFollowingTeammate = false;
		// Cancel follow path if we were following
		if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_FOLLOW_TEAMMATE)
			G_NavEngine.CancelPath();
		return false;
	}

	m_bIsFollowingTeammate = true;
	m_iFollowTargetIdx = pBestTarget->entindex();

	Vec3 vTargetPos = pBestTarget->m_vecOrigin();

	// === Height-aware proximity check ===
	float flHeightDiff = std::abs(vLocalPos.z - vTargetPos.z);
	bool bOnSameFloor = flHeightDiff < 72.0f;

	// === Ported from TF2 medic bot's ChasePath approach ===
	// Stop-follow range: how close before we stop actively pathing (stay near)
	// Start-follow range: how far before we start actively chasing
	const float flStopFollowRange = CFG::NavBot_FollowDistance;
	const float flStartFollowRange = CFG::NavBot_FollowDistance * 3.0f;

	// Don't stand idle next to a teammate in spawn — let EscapeSpawn take over
	if (flBestDist2D <= flStopFollowRange && bOnSameFloor)
	{
		if (G_NavEngine.IsNavMeshLoaded())
		{
			CNavArea* pTargetArea = G_NavEngine.m_pMap->FindClosestNavArea(vTargetPos);
			if (pTargetArea)
			{
				int iLocalTeam = pLocal->m_iTeamNum();
				bool bInOwnSpawn = (iLocalTeam == TF_TEAM_RED && (pTargetArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_RED)) ||
									(iLocalTeam == TF_TEAM_BLUE && (pTargetArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_BLUE));
				if (bInOwnSpawn)
				{
					m_bIsFollowingTeammate = false;
					m_iFollowTargetIdx = -1;
					if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_FOLLOW_TEAMMATE)
						G_NavEngine.CancelPath();
					return false;
				}
			}
		}

		// Close enough — cancel active pathing but stay in follow state
		// Like TF2 medic staying near patient without actively chasing
		if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_FOLLOW_TEAMMATE && G_NavEngine.IsPathing())
			G_NavEngine.CancelPath();
		return true;
	}

	// === Position prediction (ported from TF2 bot's ChasePath::PredictSubjectPosition) ===
	Vec3 vPathTarget = vTargetPos;
	Vec3 vTargetVelocity = pBestTarget->m_vecVelocity();
	float flSpeed2D = vTargetVelocity.Length2D();

	if (flSpeed2D > 50.0f && flBestDist2D <= flStartFollowRange * 3.0f)
	{
		float flRunSpeed = 300.0f;
		float flLeadTime = 0.5f + (flBestDist2D / (flRunSpeed + 0.001f));

		Vec3 vLead = vTargetVelocity * flLeadTime;
		vLead.z = 0.0f;

		Vec3 vToTarget = vTargetPos - vLocalPos;
		vToTarget.z = 0.0f;
		float flToTargetLen = vToTarget.Length();
		if (flToTargetLen > 1.0f)
		{
			Vec3 vToTargetNorm = vToTarget / flToTargetLen;
			float flDot = vToTargetNorm.Dot(vLead);
			if (flDot < 0.0f)
			{
				Vec3 vPerp = vLead - vToTargetNorm * flDot;
				vLead = vPerp;
			}
		}

		vPathTarget = vTargetPos + vLead;
	}

	// === Use nav area center as pathing destination ===
	if (G_NavEngine.IsNavMeshLoaded())
	{
		CNavArea* pTargetArea = G_NavEngine.m_pMap->FindClosestNavArea(vPathTarget);
		if (pTargetArea)
		{
			Vec3 vAreaCenter = pTargetArea->m_vCenter;
			vAreaCenter.z = vPathTarget.z;
			vPathTarget = vAreaCenter;
		}
	}

	// === Repath logic (ported from TF2 bot's ChasePath::RefreshPath) ===
	bool bNeedRepath = false;

	if (!G_NavEngine.IsPathing() || G_NavEngine.m_eCurrentPriority != NAV_PRIO_FOLLOW_TEAMMATE)
	{
		bNeedRepath = true;
	}
	else
	{
		float flDistFromDest = G_NavEngine.GetDestination().DistTo(vPathTarget);
		if (flDistFromDest > 50.0f)
		{
			// Target moved significantly — repath immediately
			bNeedRepath = true;
		}
	}

	if (bNeedRepath)
	{
		if (G_NavEngine.NavTo(vPathTarget, NAV_PRIO_FOLLOW_TEAMMATE))
		{
			m_flNextRepathTime = flCurTime + 0.2f;
			return true;
		}
	}

	// Already pathing to teammate
	if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_FOLLOW_TEAMMATE && G_NavEngine.IsPathing())
		return true;

	return false;
}

// === Auto Join Team (ported from Amalgam's AutoJoin) ===
// Automatically joins the selected team when enabled
void CNavBot::AutoJoinTeam(C_TFPlayer* pLocal)
{
	if (!pLocal)
		return;

	int iDesiredTeam = CFG::NavBot_AutoJoinTeam;
	if (!iDesiredTeam)
		return;

	float flCurTime = I::GlobalVars->curtime;
	if (flCurTime < m_flNextAutoJoinTeamTime)
		return;

	// Already on the desired team - no need to rejoin
	int iCurrentTeam = 0;
	if (pLocal->IsInValidTeam(&iCurrentTeam))
	{
		// Already on a valid team (Red or Blue) - check if it matches desired team
		if (iDesiredTeam == 4)  // Random — any valid team is fine
		{
			m_iLastRandomTeamAttempt = 0;  // Reset random tracking when on valid team
			return;  // Already on a team, no need to rejoin
		}
		else if (iDesiredTeam != 3)  // Not spectator
		{
			// Map CFG values to TF team values: 1=Blue(TF_TEAM_BLUE=3), 2=Red(TF_TEAM_RED=2)
			int iDesiredTFTeam = (iDesiredTeam == 1) ? 3 : 2;  // Blue=3, Red=2
			if (iCurrentTeam == iDesiredTFTeam)
			{
				m_iLastRandomTeamAttempt = 0;  // Reset random tracking when on valid team
				return;  // Already on the correct team
			}
		}
		else if (iCurrentTeam == 1)  // TF_TEAM_SPECTATOR = 1
			return;  // Already spectator
	}

	m_flNextAutoJoinTeamTime = flCurTime + 1.0f;  // Throttle to once per second

	switch (iDesiredTeam)
	{
	case 1:  // Blue
		I::EngineClient->ClientCmd_Unrestricted("jointeam blue");
		break;
	case 2:  // Red
		I::EngineClient->ClientCmd_Unrestricted("jointeam red");
		break;
	case 3:  // Spectator
		I::EngineClient->ClientCmd_Unrestricted("jointeam spectator");
		break;
	case 4:  // Random (Red or Blue only, never spectator) — alternate if first attempt fails
	{
		// If we tried a team last time and still aren't on a team, try the other one
		int iTeam = 0;
		if (m_iLastRandomTeamAttempt == 1)
			iTeam = 2;  // Tried Blue last, try Red
		else if (m_iLastRandomTeamAttempt == 2)
			iTeam = 1;  // Tried Red last, try Blue
		else
			iTeam = (std::rand() % 2 == 0) ? 1 : 2;  // First attempt: pick randomly

		m_iLastRandomTeamAttempt = iTeam;
		I::EngineClient->ClientCmd_Unrestricted(iTeam == 1 ? "jointeam blue" : "jointeam red");
		break;
	}
	}
	I::EngineClient->ClientCmd_Unrestricted("menuclosed");
}

bool CNavBot::IsReachable(C_TFPlayer* pLocal, const Vec3& vDestination)
{
	if (!pLocal || !G_NavEngine.IsNavMeshLoaded())
		return false;

	auto pMap = G_NavEngine.m_pMap.get();
	if (!pMap || pMap->m_eState != NavState::Active)
		return false;

	// === Step 1: Quick check - is there a nav area at/under the destination? ===
	// If the destination isn't on any nav area, there's no way to walk there.
	// This is a cheap O(1) check that eliminates most invalid destinations.
	CNavArea* pDestArea = pMap->FindClosestNavArea(vDestination);
	if (!pDestArea)
		return false;

	// FindClosestNavArea can return a fallback area that's just the closest center,
	// but the destination itself might not actually be on any nav area.
	// Verify the destination is overlapping its closest nav area.
	bool bDestOverlapping = pDestArea->IsOverlapping(vDestination) &&
	                       vDestination.z >= pDestArea->m_flMinZ && vDestination.z <= pDestArea->m_flMaxZ + 100.0f;
	if (!bDestOverlapping)
		return false;

	// === Step 2: Full check - is there a connected path from local to destination? ===
	// This runs A* pathfinding to verify all nav areas along the route are valid.
	Vec3 vLocalPos = pLocal->m_vecOrigin();
	CNavArea* pLocalArea = pMap->FindClosestNavArea(vLocalPos);
	if (!pLocalArea)
		return false;

	// Same-area case: local and destination are on the same nav area
	if (pLocalArea == pDestArea)
		return true;

	// Run A* pathfinding to verify connectivity
	int iResult = 0;
	std::vector<void*> vPath = pMap->FindPath(pLocalArea, pDestArea, &iResult);

	// Empty path with START_END_SAME means same area (already handled above)
	// Empty path with no result means no path exists
	if (vPath.empty() && iResult != micropather::MicroPather::START_END_SAME)
		return false;

	return true;
}

void CNavBot::ResetAutoJoinTimers()
{
	m_flNextAutoJoinTeamTime = 0.0f;
	m_flNextAutoJoinClassTime = 0.0f;
	m_iLastRandomTeamAttempt = 0;
}

// === Auto Join Class (ported from Amalgam's AutoJoin) ===
// Automatically joins the selected class when enabled
void CNavBot::AutoJoinClass(C_TFPlayer* pLocal)
{
	if (!pLocal)
		return;

	int iDesiredClass = CFG::NavBot_AutoJoinClass;
	if (!iDesiredClass)
		return;

	// Must be on a team first before joining a class
	if (!pLocal->IsInValidTeam())
		return;

	// Already the desired class - no need to rejoin
	if (pLocal->m_iClass() == iDesiredClass)
		return;

	float flCurTime = I::GlobalVars->curtime;
	if (flCurTime < m_flNextAutoJoinClassTime)
		return;

	m_flNextAutoJoinClassTime = flCurTime + 1.0f;  // Throttle to once per second (like Amalgam)

	static const char* szClassNames[] = {
		"scout", "sniper", "soldier", "demoman",
		"medic", "heavyweapons", "pyro", "spy", "engineer"
	};

	int iClassIndex = iDesiredClass - 1;  // 0-based index into class names

	I::EngineClient->ClientCmd_Unrestricted(std::format("joinclass {}", szClassNames[iClassIndex]).c_str());
	I::EngineClient->ClientCmd_Unrestricted("menuclosed");
}

// === Follow Tagged Players ===
// Follows players that have the "Follow Player" tag set in the player list
// Uses 2D distance + height awareness so the bot doesn't think it's "close"
// to a tagged player on a different floor directly above/below.
bool CNavBot::FollowTaggedPlayer(C_TFPlayer* pLocal)
{
	if (!pLocal || !CFG::NavBot_FollowTaggedPlayers)
	{
		m_bIsFollowingTaggedPlayer = false;
		m_iFollowTaggedTargetIdx = -1;
		return false;
	}

	float flCurTime = I::GlobalVars->curtime;
	Vec3 vLocalPos = pLocal->m_vecOrigin();

	// Find the closest tagged player that is alive
	C_TFPlayer* pBestTarget = nullptr;
	float flBestDist2D = FLT_MAX;

	const int nMaxEntities = I::ClientEntityList->GetHighestEntityIndex();
	for (int i = 1; i <= nMaxEntities; i++)
	{
		IClientEntity* pClientEntity = I::ClientEntityList->GetClientEntity(i);
		if (!pClientEntity)
			continue;

		auto pNetworkable = pClientEntity->GetClientNetworkable();
		if (!pNetworkable)
			continue;

		auto pClientClass = pNetworkable->GetClientClass();
		if (!pClientClass || static_cast<ETFClassIds>(pClientClass->m_ClassID) != ETFClassIds::CTFPlayer)
			continue;

		auto pPlayer = pClientEntity->As<C_TFPlayer>();
		if (!pPlayer || pPlayer == pLocal || !pPlayer->IsAlive())
			continue;

		// Check if this player has the FollowPlayer tag
		PlayerPriority info{};
		if (!F::Players->GetInfo(i, info) || !info.FollowPlayer)
			continue;

		Vec3 vPlayerPos = pPlayer->m_vecOrigin();
		Vec3 vDelta = vLocalPos - vPlayerPos;
		float flDist2D = vDelta.Length2D();

		if (flDist2D < flBestDist2D)
		{
			flBestDist2D = flDist2D;
			pBestTarget = pPlayer;
		}
	}

	// No tagged player found
	if (!pBestTarget)
	{
		m_bIsFollowingTaggedPlayer = false;
		m_iFollowTaggedTargetIdx = -1;
		return false;
	}

	m_bIsFollowingTaggedPlayer = true;
	m_iFollowTaggedTargetIdx = pBestTarget->entindex();

	Vec3 vTargetPos = pBestTarget->m_vecOrigin();

	// === Height-aware proximity check ===
	float flHeightDiff = std::abs(vLocalPos.z - vTargetPos.z);
	bool bOnSameFloor = flHeightDiff < 72.0f;

	// === Ported from TF2 medic bot's ChasePath approach ===
	// The TF2 medic NEVER stops following — it uses ChasePath which continuously
	// updates the path to the moving target every tick. When close, it stops moving
	// but keeps the follow state active so it immediately follows when the target moves.

	// Stop-follow range: how close before we stop actively pathing (stay near)
	// Start-follow range: how far before we start actively chasing
	const float flStopFollowRange = CFG::NavBot_FollowDistance;
	const float flStartFollowRange = CFG::NavBot_FollowDistance * 3.0f;

	// If we're close enough and on the same floor, we don't need to move
	// BUT we keep the follow state active — don't cancel the path entirely
	if (flBestDist2D <= flStopFollowRange && bOnSameFloor)
	{
		// We're close enough — cancel active pathing but stay in follow state
		// This is like the TF2 medic staying near the patient without actively chasing
		if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_FOLLOW_TAGGED && G_NavEngine.IsPathing())
			G_NavEngine.CancelPath();
		return true;
	}

	// === Position prediction (ported from TF2 bot's ChasePath::PredictSubjectPosition) ===
	// Estimate where the target will be when we arrive, based on their velocity.
	// This makes the bot "lead" moving targets instead of always chasing where they were.
	Vec3 vPathTarget = vTargetPos;
	Vec3 vTargetVelocity = pBestTarget->m_vecVelocity();
	float flSpeed2D = vTargetVelocity.Length2D();

	if (flSpeed2D > 50.0f && flBestDist2D <= flStartFollowRange * 3.0f)
	{
		// Estimate time to reach target based on our run speed (~300 units/s)
		float flRunSpeed = 300.0f;
		float flLeadTime = 0.5f + (flBestDist2D / (flRunSpeed + 0.001f));

		// Predict where the target will be
		Vec3 vLead = vTargetVelocity * flLeadTime;
		vLead.z = 0.0f;  // Only lead horizontally

		// If target is moving towards us, only use perpendicular velocity for leading
		Vec3 vToTarget = vTargetPos - vLocalPos;
		vToTarget.z = 0.0f;
		float flToTargetLen = vToTarget.Length();
		if (flToTargetLen > 1.0f)
		{
			Vec3 vToTargetNorm = vToTarget / flToTargetLen;
			float flDot = vToTargetNorm.Dot(vLead);
			if (flDot < 0.0f)
			{
				// Target moving towards us — use perpendicular component only
				Vec3 vPerp = vLead - vToTargetNorm * flDot;
				vLead = vPerp;
			}
		}

		vPathTarget = vTargetPos + vLead;
	}

	// === Use nav area center as pathing destination ===
	if (G_NavEngine.IsNavMeshLoaded())
	{
		CNavArea* pTargetArea = G_NavEngine.m_pMap->FindClosestNavArea(vPathTarget);
		if (pTargetArea)
		{
			Vec3 vAreaCenter = pTargetArea->m_vCenter;
			vAreaCenter.z = vPathTarget.z;
			vPathTarget = vAreaCenter;
		}
	}

	// === Repath logic (ported from TF2 bot's ChasePath::RefreshPath) ===
	// The TF2 medic continuously re-paths every tick when the target moves.
	// We repath immediately when:
	// 1. We're not currently pathing to the target
	// 2. The target has moved significantly from our current destination
	// 3. We were close but the target moved out of stop-follow range
	bool bNeedRepath = false;

	if (!G_NavEngine.IsPathing() || G_NavEngine.m_eCurrentPriority != NAV_PRIO_FOLLOW_TAGGED)
	{
		// Not currently following — start pathing
		bNeedRepath = true;
	}
	else
	{
		// Check if target moved enough to warrant a repath
		float flDistFromDest = G_NavEngine.GetDestination().DistTo(vPathTarget);
		if (flDistFromDest > 50.0f)
		{
			// Target moved significantly — repath immediately (like TF2 medic ChasePath)
			bNeedRepath = true;
		}
	}

	if (bNeedRepath)
	{
		if (G_NavEngine.NavTo(vPathTarget, NAV_PRIO_FOLLOW_TAGGED))
		{
			// Short repath interval for follow — the TF2 medic re-paths every tick
			m_flNextRepathTime = flCurTime + 0.2f;
			return true;
		}
	}

	// Already pathing to tagged player
	if (G_NavEngine.m_eCurrentPriority == NAV_PRIO_FOLLOW_TAGGED && G_NavEngine.IsPathing())
		return true;

	return false;
}

// === AutoScope - automatically scope in when enemies are visible ===
// Ported from Amalgam's BotUtils::AutoScope
// Two modes: Simple (velocity prediction) and MoveSim (full movement simulation)
// Scopes in when a visible enemy is found, unscopes after cancel time with no enemy
void CNavBot::AutoScope(C_TFPlayer* pLocal, C_TFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	if (!pLocal || !pWeapon || !pCmd)
		return;

	bool bIsClassic = pWeapon->GetWeaponID() == TF_WEAPON_SNIPERRIFLE_CLASSIC;

	// Only run for sniper rifles
	if (!CFG::NavBot_AutoScope
		|| (pWeapon->GetWeaponID() != TF_WEAPON_SNIPERRIFLE
			&& !bIsClassic
			&& pWeapon->GetWeaponID() != TF_WEAPON_SNIPERRIFLE_DECAP))
	{
		m_bAutoScopeKeep = false;
		m_mAutoScopeCache.clear();
		return;
	}

	// Clear cache every other tick (like Amalgam's bShouldClearCache logic)
	static bool bShouldClearCache = false;
	bShouldClearCache = !bShouldClearCache;
	if (bShouldClearCache)
		m_mAutoScopeCache.clear();

	float flCurTime = I::GlobalVars->curtime;

	// === Wait After Shot (sniper only) ===
	// After firing a scoped shot, block IN_ATTACK until the wait time expires
	// and we've re-scoped. This prevents the aimbot from firing unscoped shots
	// at the next enemy before AutoScope can re-scope.
	bool bIsSniperRifle = (pWeapon->GetWeaponID() == TF_WEAPON_SNIPERRIFLE
		|| bIsClassic
		|| pWeapon->GetWeaponID() == TF_WEAPON_SNIPERRIFLE_DECAP);
	bool bCurrentlyScoped = pLocal->InCond(TF_COND_ZOOMED);

	if (CFG::NavBot_AutoScopeWaitAfterShot > 0.0f && bIsSniperRifle)
	{
		// Detect the scope-to-unscope transition — a scoped shot was just fired
		if (m_bAutoScopeWasScopedLastTick && !bCurrentlyScoped && m_flAutoScopeLastShotTime == 0.0f)
			m_flAutoScopeLastShotTime = flCurTime;

		// During the wait period, set flag so aimbot won't fire
		if (m_flAutoScopeLastShotTime > 0.0f)
		{
			float flElapsed = flCurTime - m_flAutoScopeLastShotTime;
			if (flElapsed < CFG::NavBot_AutoScopeWaitAfterShot || !bCurrentlyScoped)
			{
				// Still waiting or not yet re-scoped — tell aimbot not to fire
				G::bAutoScopeWaitActive = true;
			}
			else
			{
				// Wait over and we're scoped again — allow firing
				m_flAutoScopeLastShotTime = 0.0f;
			}
		}
	}

	m_bAutoScopeWasScopedLastTick = bCurrentlyScoped;

	// === Handle keep state for Classic rifle ===
	if (bIsClassic)
	{
		if (m_bAutoScopeKeep)
		{
			if (!(pCmd->buttons & IN_ATTACK))
				pCmd->buttons |= IN_ATTACK;
			if (flCurTime - m_flAutoScopeTimer >= CFG::NavBot_AutoScopeCancelTime)
				pCmd->buttons |= IN_JUMP;  // Cancel classic charge
		}
		if (!IsOnGround(pLocal) && !(pCmd->buttons & IN_ATTACK))
			m_bAutoScopeKeep = false;
	}
	else
	{
		// === Handle keep state for regular sniper rifles ===
		if (m_bAutoScopeKeep)
		{
			if (pLocal->InCond(TF_COND_ZOOMED))
			{
				if (flCurTime - m_flAutoScopeTimer >= CFG::NavBot_AutoScopeCancelTime)
				{
					m_bAutoScopeKeep = false;
					pCmd->buttons |= IN_ATTACK2;  // Unscope
					return;
				}
			}
		}
	}

	// === Find enemies sorted by distance ===
	Vec3 vLocalOrigin = pLocal->m_vecOrigin();

	// Get the nav area we're heading toward (for tracing from destination position)
	CNavArea* pCurrentDestinationArea = nullptr;
	const auto& vCrumbs = G_NavEngine.GetCrumbs();
	if (vCrumbs.size() > 4)
		pCurrentDestinationArea = vCrumbs[4].m_pNavArea;

	auto pLocalNav = pCurrentDestinationArea ? pCurrentDestinationArea : G_NavEngine.GetLocalNavArea();
	if (!pLocalNav && G_NavEngine.m_pMap)
		pLocalNav = G_NavEngine.m_pMap->FindClosestNavArea(vLocalOrigin);
	if (!pLocalNav)
		return;

	Vec3 vFrom = pLocalNav->m_vCenter;
	vFrom.z += PLAYER_JUMP_HEIGHT;

	std::vector<std::pair<C_BaseEntity*, float>> vEnemiesSorted;
	for (const auto pEntity : H::Entities->GetGroup(EEntGroup::PLAYERS_ENEMIES))
	{
		if (!pEntity || !H::Entities->IsEntityValid(pEntity))
			continue;
		auto pPlayer = pEntity->As<C_TFPlayer>();
		if (!pPlayer || !pPlayer->IsAlive() || pPlayer->IsDormant())
			continue;
		vEnemiesSorted.emplace_back(pEntity, pPlayer->m_vecOrigin().DistToSqr(vLocalOrigin));
	}

	// Also check enemy buildings
	for (const auto pEntity : H::Entities->GetGroup(EEntGroup::BUILDINGS_ENEMIES))
	{
		if (!pEntity || !H::Entities->IsEntityValid(pEntity))
			continue;
		if (pEntity->IsDormant())
			continue;
		vEnemiesSorted.emplace_back(pEntity, pEntity->GetAbsOrigin().DistToSqr(vLocalOrigin));
	}

	if (vEnemiesSorted.empty())
		return;

	std::sort(vEnemiesSorted.begin(), vEnemiesSorted.end(),
		[](const auto& a, const auto& b) { return a.second < b.second; });

	// === Visibility check lambda ===
	auto CheckVisibility = [&](const Vec3& vTo, int iEntIndex) -> bool
	{
		CGameTrace trace = {};
		CTraceFilterWorldAndPropsOnlyAmalgam filter = {};

		// Trace from local pos first
		Vec3 vLocalEye = Vec3(vLocalOrigin.x, vLocalOrigin.y, vLocalOrigin.z + PLAYER_JUMP_HEIGHT);
		H::AimUtils->Trace(vLocalEye, vTo, MASK_SHOT | CONTENTS_GRATE, &filter, &trace);
		bool bHit = trace.fraction == 1.0f;
		if (!bHit)
		{
			// Try from our destination pos
			H::AimUtils->Trace(vFrom, vTo, MASK_SHOT | CONTENTS_GRATE, &filter, &trace);
			bHit = trace.fraction == 1.0f;
		}

		if (iEntIndex != -1)
			m_mAutoScopeCache[iEntIndex] = bHit;

		if (bHit)
		{
			if (bIsClassic)
				pCmd->buttons |= IN_ATTACK;
			else if (!pLocal->InCond(TF_COND_ZOOMED) && !(pCmd->buttons & IN_ATTACK2))
				pCmd->buttons |= IN_ATTACK2;

			m_flAutoScopeTimer = flCurTime;
			m_bAutoScopeKeep = true;
			return true;
		}
		return false;
	};

	bool bSimple = (CFG::NavBot_AutoScope == 1);  // 1=Simple, 2=MoveSim
	int iMaxTicks = TIME_TO_TICKS(0.5f);

	for (auto& [pEnemy, _] : vEnemiesSorted)
	{
		int iEntIndex = pEnemy->entindex();

		// Check cache first
		if (m_mAutoScopeCache.contains(iEntIndex))
		{
			if (m_mAutoScopeCache[iEntIndex])
			{
				if (bIsClassic)
					pCmd->buttons |= IN_ATTACK;
				else if (!pLocal->InCond(TF_COND_ZOOMED) && !(pCmd->buttons & IN_ATTACK2))
					pCmd->buttons |= IN_ATTACK2;

				m_flAutoScopeTimer = flCurTime;
				m_bAutoScopeKeep = true;
				break;
			}
			continue;
		}

		// Check non-predicted position first
		Vec3 vNonPredictedPos = pEnemy->GetAbsOrigin();
		vNonPredictedPos.z += PLAYER_JUMP_HEIGHT;
		if (CheckVisibility(vNonPredictedPos, iEntIndex))
			return;

		// === MoveSim mode: predict where the enemy will be ===
		if (!bSimple)
		{
			// Only simulate players — buildings can't be cast to C_TFPlayer
			if (!IsPlayer(pEnemy))
				continue;

			auto pEnemyPlayer = pEnemy->As<C_TFPlayer>();
			if (!pEnemyPlayer || !pEnemyPlayer->IsAlive() || !H::Entities->IsEntityValid(pEnemyPlayer))
				continue;

			F::MovementSimulation->Initialize(pEnemyPlayer);
			if (F::MovementSimulation->HasFailed())
			{
				F::MovementSimulation->Restore();
				continue;
			}

			for (int i = 0; i < iMaxTicks; i++)
				F::MovementSimulation->RunTick();
		}

		bool bResult = false;
		Vec3 vPredictedPos = bSimple
			? pEnemy->GetAbsOrigin() + pEnemy->GetAbsVelocity() * TICKS_TO_TIME(iMaxTicks)
			: F::MovementSimulation->GetOrigin();

		CNavArea* pTargetNav = nullptr;
		if (G_NavEngine.m_pMap)
			pTargetNav = G_NavEngine.m_pMap->FindClosestNavArea(vPredictedPos);
		if (pTargetNav)
		{
			Vec3 vTo = pTargetNav->m_vCenter;

			// If player is in the air, don't snap to nav area below them
			if (IsPlayer(pEnemy) && !IsOnGround(pEnemy->As<C_TFPlayer>()) && vTo.DistToSqr(vPredictedPos) >= 400.0f * 400.0f)
				vTo = vPredictedPos;

			vTo.z += PLAYER_JUMP_HEIGHT;
			bResult = CheckVisibility(vTo, iEntIndex);
		}
		if (!bSimple)
			F::MovementSimulation->Restore();

		if (bResult)
			break;
	}
}
