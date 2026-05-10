#include "Map.h"
#include "../../../../SDK/SDK.h"
#include "../../CFG.h"

// Must come after SDK.h to avoid Windows min/max macro pollution
#undef min
#undef max
#include <algorithm>
#include <cmath>

void CMap::AdjacentCost(void* pArea, std::vector<micropather::StateCost>* pAdjacent)
{
	if (!pArea)
		return;

	CNavArea* pCurrentArea = reinterpret_cast<CNavArea*>(pArea);

	// Get local player for team-based filtering
	auto pLocal = I::ClientEntityList->GetClientEntity(I::EngineClient->GetLocalPlayer());
	int iTeam = 0;
	if (pLocal)
	{
		auto pLocalPlayer = pLocal->As<C_TFPlayer>();
		if (pLocalPlayer)
			iTeam = pLocalPlayer->m_iTeamNum();
	}

	// Iterate through all connections from this area
	for (NavConnect_t& tConnection : pCurrentArea->m_vConnections)
	{
		CNavArea* pNextArea = tConnection.m_pArea;
		if (!pNextArea || pNextArea == pCurrentArea)
			continue;

		// Determine if this is a one-way connection
		bool bIsOneWay = IsOneWay(pCurrentArea, pNextArea);

		// Determine navigation points
		NavPoints_t tPoints = DeterminePoints(pCurrentArea, pNextArea, bIsOneWay);

		// Handle dropdown/vertical navigation
		DropdownHint_t tDropdown = HandleDropdown(tPoints.m_vCenter, tPoints.m_vNext, bIsOneWay);
		tPoints.m_vCenter = tDropdown.m_vAdjustedPos;

		// === Ported from TF2 bot's CTFBotPathCost::operator() and IsAreaTraversable ===
		// These checks determine if an area is completely impassable (skip it)
		// vs. just costly (add penalty). The TF2 bot returns -1.0f for impassable,
		// which we emulate by continuing (skipping the connection).

		// TF_NAV_BLOCKED check (ported from TF2 bot's IsAreaTraversable)
		// Areas can be blocked by doors, capture point logic, etc.
		if (pNextArea->m_iTFAttributeFlags & TF_NAV_BLOCKED)
			continue;

		// Enemy spawn room check (ported from TF2 bot's CTFBotPathCost)
		// TF2 bot returns -1.0f (impassable) for enemy spawn rooms unless round is won
		if (iTeam == TF_TEAM_RED && (pNextArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_BLUE))
			continue;
		if (iTeam == TF_TEAM_BLUE && (pNextArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_RED))
			continue;

		// Setup gate check (ported from TF2 bot's IsAreaTraversable)
		// During setup, enemy setup gates are impassable barriers
		if (iTeam == TF_TEAM_RED && (pNextArea->m_iTFAttributeFlags & TF_NAV_BLUE_SETUP_GATE))
			continue;
		if (iTeam == TF_TEAM_BLUE && (pNextArea->m_iTFAttributeFlags & TF_NAV_RED_SETUP_GATE))
			continue;

		// Point-capture blocked areas (ported from TF2 bot's nav mesh logic)
		// These areas are blocked until/unless a specific point is captured
		if (pNextArea->m_iTFAttributeFlags & TF_NAV_BLOCKED_UNTIL_POINT_CAPTURE)
			continue;
		if (pNextArea->m_iTFAttributeFlags & TF_NAV_BLOCKED_AFTER_POINT_CAPTURE)
			continue;

		// Check height difference at the shared edge between areas (ported from TF2 bot)
		// TF2 bot uses ComputeAdjacentConnectionHeightChange which checks the height at the
		// boundary where you actually step, NOT center-to-center. For stairs, the edge height
		// is much smaller than the center height — center-to-center incorrectly blocks stairs.
		float flEdgeHeightDiff = pCurrentArea->ComputeAdjacentConnectionHeightChange(pNextArea);

		// On stairs, skip the height block entirely — the nav mesh marks stairs as walkable,
		// and the physics engine handles step-ups automatically (like TF2 bot's PathFollower).
		bool bStairConnection = (pCurrentArea->m_iAttributeFlags & NAV_MESH_STAIRS) ||
		                        (pNextArea->m_iAttributeFlags & NAV_MESH_STAIRS);

		if (!bStairConnection)
		{
			// Block if the step-up at the edge exceeds max jump height
			// TF2 bot: if deltaZ >= GetMaxJumpHeight() return -1 (impassable)
			if (flEdgeHeightDiff > PLAYER_CROUCHED_JUMP_HEIGHT)
				continue;
		}

		// Death drop height check (ported from TF2 bot's CTFBotPathCost)
		// TF2 bot's GetDeathDropHeight() returns 1000.0f — drops beyond this are lethal
		// Use edge-based height for drops too (more accurate than center-to-center)
		if (flEdgeHeightDiff < -1000.0f)
			continue;

		// === Two-way connection reliability check ===
		// Navmesh generation can mark two areas as two-way connected even when there's
		// a significant horizontal gap + height difference between them that makes the
		// connection impassable on foot (and sometimes even with jumping).
		// If the gap between the connection points is large AND there's a height step,
		// add a heavy penalty to make the pathfinder avoid this unreliable connection.
		float flUnreliablePenalty = 0.0f;
		if (!bIsOneWay)
		{
			Vec3 vGap = tPoints.m_vCenterNext - tPoints.m_vCenter;
			float flGapHorizontal = Vec3(vGap.x, vGap.y, 0.0f).Length();
			float flGapHeight = std::abs(vGap.z);

			// Player can step up 18 units normally. If the gap is wider than the player
			// AND there's a height difference beyond step height, the connection is suspect.
			if (flGapHorizontal > PLAYER_WIDTH && flGapHeight > 18.0f)
			{
				// If the gap is extremely large + height beyond jump, skip entirely (truly impassable)
				if (flGapHorizontal > PLAYER_WIDTH * 4.0f && flGapHeight > PLAYER_JUMP_HEIGHT)
					continue;

				// Scale penalty with gap size — wider gaps with more height are worse
				flUnreliablePenalty = flGapHorizontal * flGapHeight * 0.5f;
				flUnreliablePenalty = (std::max)(flUnreliablePenalty, 5000.0f);
			}
		}

		// Calculate cost using advanced cost function
		float flCost = EvaluateConnectionCost(pCurrentArea, pNextArea, tPoints, tDropdown, iTeam);

		// Apply unreliable connection penalty
		if (flUnreliablePenalty > 0.0f)
			flCost += flUnreliablePenalty;

		// Stuck blacklist penalty (ported from Amalgam)
		// If this area is blacklisted due to being stuck, add a massive penalty
		float flCurTime = I::GlobalVars->curtime;
		if (auto itBlacklist = m_mStuckBlacklist.find(pNextArea); itBlacklist != m_mStuckBlacklist.end())
		{
			if (itBlacklist->second.m_flExpireTime <= 0.0f || itBlacklist->second.m_flExpireTime > flCurTime)
				flCost += itBlacklist->second.m_flPenalty;
			else
				m_mStuckBlacklist.erase(itBlacklist);  // Expired, remove
		}

		// Danger blacklist penalty (ported from Amalgam's FreeBlacklist)
		if (auto itDanger = m_mDangerBlacklist.find(pNextArea); itDanger != m_mDangerBlacklist.end())
		{
			auto& entry = itDanger->second;
			bool bExpired = false;
			if (entry.m_iExpireTick > 0 && I::GlobalVars->tickcount > entry.m_iExpireTick)
				bExpired = true;
			if (entry.m_flExpireTime > 0.0f && flCurTime > entry.m_flExpireTime)
				bExpired = true;

			if (bExpired)
				m_mDangerBlacklist.erase(itDanger);
			else
				flCost += GetDangerPenalty(entry.m_eReason);
		}

		// Connection stuck time penalty (ported from Amalgam)
		// If we've been stuck on this specific connection before, add a penalty
		if (auto itFrom = m_mConnectionStuckTime.find(pCurrentArea); itFrom != m_mConnectionStuckTime.end())
		{
			if (auto itTo = itFrom->second.find(pNextArea); itTo != itFrom->second.end())
			{
				if (itTo->second.m_flExpireTime <= 0.0f || itTo->second.m_flExpireTime > flCurTime)
					flCost += itTo->second.m_iStuckTicks * 50.0f;  // Penalty scales with stuck duration
				else
					itFrom->second.erase(itTo);  // Expired, remove
			}
		}

		// Route variety system (ported from TF2 bot's CTFBotPathCost DEFAULT_ROUTE)
		// Adds a time-varying per-area preference penalty so the bot chooses different routes
		// over time, but keeps the same route long enough for repaths to work.
		// Without this, the bot always picks the mathematically shortest path and never varies.
		if (CFG::NavBot_RouteVariety)
		{
			// Time modulus changes every ~10 seconds, causing route preference to shift
			int iTimeMod = static_cast<int>(I::GlobalVars->curtime / 10.0f) + 1;
			// Use local player entindex and area ID as seeds (like TF2 bot's entindex * areaID)
			int iLocalPlayerIndex = I::EngineClient->GetLocalPlayer();
			// Hash the area pointer for a stable per-area value
			auto nAreaID = reinterpret_cast<uintptr_t>(pNextArea);
			nAreaID = (nAreaID >> 4) ^ (nAreaID & 0xF);  // Mix bits
			float flPreference = 1.0f + 50.0f * (1.0f + cosf(static_cast<float>(iLocalPlayerIndex * nAreaID * iTimeMod)));
			flCost *= flPreference;
		}

		// Skip if cost is invalid
		if (!std::isfinite(flCost) || flCost <= 0.f)
			continue;

		// Add to adjacent list
		pAdjacent->push_back({ reinterpret_cast<void*>(pNextArea), flCost });
	}
}

DropdownHint_t CMap::HandleDropdown(const Vec3& vCurrentPos, const Vec3& vNextPos, bool bIsOneWay)
{
	DropdownHint_t tHint{};
	tHint.m_vAdjustedPos = vCurrentPos;

	Vec3 vToTarget = vNextPos - vCurrentPos;
	const float flHeightDiff = vToTarget.z;

	Vec3 vHorizontal = vToTarget;
	vHorizontal.z = 0.f;
	const float flHorizontalLength = vHorizontal.Length();

	const float kSmallDropGrace = 18.f;

	if (flHeightDiff < 0.f)
	{
		const float flDropDistance = -flHeightDiff;
		if (flDropDistance > kSmallDropGrace && flHorizontalLength > 1.f)
		{
			tHint.m_bRequiresDrop = true;
			tHint.m_flDropHeight = flDropDistance;
			tHint.m_vApproachDir = vHorizontal / flHorizontalLength;

			if (!bIsOneWay)
			{
				Vec3 vDirection = tHint.m_vApproachDir;

				// === TF2 NextBot-style edge detection (ported from NextBotPath.cpp) ===
				// The TF2 bot traces a hull downward at incremental positions to find
				// where there's actual clearance to drop. The navmesh area boundary
				// often doesn't align with the real geometry edge, so we need to
				// push the drop position outward until we find open air.
				//
				// Original TF2 code:
				//   const float inc = 10.0f;
				//   const float maxPushDist = 2.0f * hullWidth;
				//   for (pushDist = 0.0f; pushDist <= maxPushDist; pushDist += inc)
				//   {
				//       UTIL_TraceHull(pos, lowerPos, hullMin, hullMax, mask, filter, &result);
				//       if (result.fraction >= 1.0f) break;  // found clearance
				//   }
				float flPushDist = 0.0f;
				const float flInc = 10.0f;
				const float flMaxPushDist = 2.0f * PLAYER_WIDTH;  // ~98 units
				const float flHalfWidth = PLAYER_WIDTH / 2.0f;     // ~24.5 units
				const float flHullHeight = PLAYER_HEIGHT;           // 83 units
				const float flStepHeight = 18.0f;

				// Use world-only trace filter — we only care about solid geometry, not players/entities
				CTraceFilterWorldAndPropsOnlyAmalgam traceFilter;
				traceFilter.pSkip = nullptr;

				for (flPushDist = 0.0f; flPushDist <= flMaxPushDist; flPushDist += flInc)
				{
					// Candidate position: pushed outward from the current pos along approach direction
					Vec3 vCandidate(vCurrentPos.x + flPushDist * vDirection.x,
					                vCurrentPos.y + flPushDist * vDirection.y,
					                vCurrentPos.z);

					// Lower position: same XY, but at the next area's Z height
					Vec3 vLower(vCandidate.x, vCandidate.y, vNextPos.z);

					trace_t tr;
					Vec3 vHullMin(-flHalfWidth, -flHalfWidth, flStepHeight);
					Vec3 vHullMax(flHalfWidth, flHalfWidth, flHullHeight);

					H::AimUtils->TraceHull(vCandidate, vLower, vHullMin, vHullMax,
						MASK_PLAYERSOLID, &traceFilter, &tr);

					if (tr.fraction >= 1.0f && !tr.startsolid)
					{
						// Found clearance to drop — this is the real edge
						break;
					}
				}

				// Set the approach distance to the push distance we found
				// If we couldn't find clearance (flPushDist > maxPushDist), use the max
				flPushDist = (std::min)(flPushDist, flMaxPushDist);
				tHint.m_flApproachDistance = (std::max)(flPushDist, 0.f);

				// Adjusted position: pushed outward to the real geometry edge
				tHint.m_vAdjustedPos = vCurrentPos + vDirection * tHint.m_flApproachDistance;
				tHint.m_vAdjustedPos.z = vCurrentPos.z;
			}
		}
	}
	else if (!bIsOneWay && flHeightDiff > 0.f && flHorizontalLength > 1.f)
	{
		Vec3 vDirection = vHorizontal / flHorizontalLength;

		// Step back slightly to help with climbing onto the next area.
		const float retreat = (std::clamp)(flHeightDiff * 0.35f, PLAYER_WIDTH * 0.3f, PLAYER_WIDTH);
		tHint.m_vAdjustedPos = vCurrentPos - vDirection * retreat;
		tHint.m_vAdjustedPos.z = vCurrentPos.z;
		tHint.m_vApproachDir = -vDirection;
		tHint.m_flApproachDistance = retreat;
	}

	return tHint;
}

NavPoints_t CMap::DeterminePoints(CNavArea* pCurrentArea, CNavArea* pNextArea, bool bIsOneWay)
{
	auto vCurrentCenter = pCurrentArea->m_vCenter;
	auto vNextCenter = pNextArea->m_vCenter;
	
	// Gets a vector on the edge of the current area that is as close as possible to the center of the next area
	auto vCurrentClosest = pCurrentArea->GetNearestPoint(Vector2D(vNextCenter.x, vNextCenter.y));
	// Do the same for the other area
	auto vNextClosest = pNextArea->GetNearestPoint(Vector2D(vCurrentCenter.x, vCurrentCenter.y));

	// Use one of them as a center point, the one that is either x or y aligned with a center
	// Of the areas. This will avoid walking into walls.
	auto vClosest = vCurrentClosest;

	// Determine if aligned, if not, use the other one as the center point
	if (vClosest.x != vCurrentCenter.x && vClosest.y != vCurrentCenter.y && vClosest.x != vNextCenter.x && vClosest.y != vNextCenter.y)
	{
		vClosest = vNextClosest;
		// Use the point closest to next_closest on the "original" mesh for z
		vClosest.z = pCurrentArea->GetNearestPoint(Vector2D(vNextClosest.x, vNextClosest.y)).z;
	}

	// If safepathing is enabled, adjust points to stay more centered and avoid corners
	// (ported from Amalgam's SafePathing — conditional, not always-on)
	if (!bIsOneWay && CFG::NavBot_SafePathing)
	{
		// Calculate center point as a weighted average between area centers
		// Use a 60/40 split to favor the current area more
		vClosest = vCurrentCenter + (vNextCenter - vCurrentCenter) * 0.4f;

		// Add extra safety margin near corners
		float flCornerMargin = PLAYER_WIDTH * 0.75f;

		// Check if we're near a corner by comparing distances to area edges
		bool bNearCorner = false;
		Vec3 vCurrentMins = pCurrentArea->m_vNwCorner;
		Vec3 vCurrentMaxs = pCurrentArea->m_vSeCorner;

		if (vClosest.x - vCurrentMins.x < flCornerMargin ||
			vCurrentMaxs.x - vClosest.x < flCornerMargin ||
			vClosest.y - vCurrentMins.y < flCornerMargin ||
			vCurrentMaxs.y - vClosest.y < flCornerMargin)
			bNearCorner = true;

		// If near corner, move point more towards center
		if (bNearCorner)
		{
			Vec3 vToCenter = vCurrentCenter - vClosest;
			float flLen = vToCenter.Length();
			if (flLen > 0.f)
				vClosest = vClosest + (vToCenter / flLen) * flCornerMargin;
		}

		// Ensure the point is within the current area
		vClosest = pCurrentArea->GetNearestPoint(Vector2D(vClosest.x, vClosest.y));
	}

	// Nearest point to center on "next" (like Amalgam — no extra corner margin)
	auto vCenterNext = pNextArea->GetNearestPoint(Vector2D(vClosest.x, vClosest.y));

	return NavPoints_t(vCurrentCenter, vClosest, vCenterNext, vNextCenter);
}

bool CMap::IsOneWay(CNavArea* pFrom, CNavArea* pTo) const
{
	if (!pFrom || !pTo)
		return true;

	for (auto& tBackConnection : pTo->m_vConnections)
	{
		if (tBackConnection.m_pArea == pFrom)
			return false;
	}

	return true;
}

float CMap::EvaluateConnectionCost(CNavArea* pCurrentArea, CNavArea* pNextArea, const NavPoints_t& tPoints, const DropdownHint_t& tDropdown, int iTeam) const
{
	auto HorizontalDistance = [](const Vec3& vStart, const Vec3& vEnd) -> float
	{
		Vec3 vFlat = vEnd - vStart;
		vFlat.z = 0.f;
		float flLen = vFlat.Length();
		return flLen > 0.f ? flLen : 0.f;
	};

	float flForwardDistance = (std::max)(HorizontalDistance(tPoints.m_vCurrent, tPoints.m_vNext), 1.f);
	float flDeviationStart = HorizontalDistance(tPoints.m_vCurrent, tPoints.m_vCenter);
	float flDeviationEnd = HorizontalDistance(tPoints.m_vCenter, tPoints.m_vNext);

	// Use edge-based height difference (ported from TF2 bot's ComputeAdjacentConnectionHeightChange)
	// Center-to-center height is inaccurate for stairs — the edge where you step is much lower
	float flHeightDiff = pCurrentArea->ComputeAdjacentConnectionHeightChange(pNextArea);

	// Base cost is forward distance
	float flCost = flForwardDistance;
	
	// Add deviation penalties (SafePathing cost)
	flCost += flDeviationStart * 0.3f;
	flCost += flDeviationEnd * 0.2f;

	// Height penalties — reduced for stairs (physics engine handles step-ups automatically)
	bool bStairConnection = (pCurrentArea->m_iAttributeFlags & NAV_MESH_STAIRS) ||
	                        (pNextArea->m_iAttributeFlags & NAV_MESH_STAIRS);
	if (flHeightDiff > 0.f)
	{
		// On stairs, height penalty is minimal — just walking up steps
		float flHeightMultiplier = bStairConnection ? 0.2f : 1.8f;
		flCost += flHeightDiff * flHeightMultiplier;
	}
	else if (flHeightDiff < -8.f)
		flCost += std::abs(flHeightDiff) * 0.9f;

	// Dropdown penalties
	if (tDropdown.m_bRequiresDrop)
	{
		flCost += tDropdown.m_flDropHeight * 2.2f;
		flCost += tDropdown.m_flApproachDistance * 0.45f;
	}
	else if (tDropdown.m_flApproachDistance > 0.f)
		flCost += tDropdown.m_flApproachDistance * 0.25f;

	// Turn penalty - prefer straighter paths
	Vec3 vForward = tPoints.m_vCenter - tPoints.m_vCurrent;
	Vec3 vForwardNext = tPoints.m_vNext - tPoints.m_vCenter;
	vForward.z = 0.f;
	vForwardNext.z = 0.f;
	float flLen1 = vForward.Length();
	float flLen2 = vForwardNext.Length();
	if (flLen1 > 1.f && flLen2 > 1.f)
	{
		vForward = vForward / flLen1;
		vForwardNext = vForwardNext / flLen2;
		float flDot = (std::clamp)(vForward.Dot(vForwardNext), -1.f, 1.f);
		float flTurnPenalty = (1.f - flDot) * 30.f;
		flCost += flTurnPenalty;
	}

	// Bonus for larger areas (more room to maneuver)
	Vec3 vAreaExtent = pNextArea->m_vSeCorner - pNextArea->m_vNwCorner;
	vAreaExtent.z = 0.f;
	float flAreaSize = vAreaExtent.Length();
	if (flAreaSize > 0.f)
		flCost -= (std::clamp)(flAreaSize * 0.01f, 0.f, 12.f);

	// Spawn room penalty (ported from TF2 bot's CTFBotPathCost)
	// Enemy spawn rooms are already blocked in AdjacentCost (impassable),
	// so here we only penalize own-team spawn rooms to discourage loitering
	const bool bRedSpawn = pNextArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_RED;
	const bool bBlueSpawn = pNextArea->m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_BLUE;
	if (bRedSpawn || bBlueSpawn)
	{
		// Penalty for own spawn — don't loiter near spawn
		if ((iTeam == TF_TEAM_RED && bRedSpawn) || (iTeam == TF_TEAM_BLUE && bBlueSpawn))
			flCost += 100.f;
	}

	// Jump area penalty (ported from TF2 bot's CTFBotPathCost)
	// TF2 bot: "jumping is slower than flat ground" — dist *= 2.0f
	if (pNextArea->m_iAttributeFlags & NAV_MESH_JUMP)
		flCost *= 2.0f;

	// Cliff area penalty (NAV_MESH_CLIFF — adjacent to a lethal drop)
	// Penalize heavily to discourage walking near cliff edges
	if (pNextArea->m_iAttributeFlags & NAV_MESH_CLIFF)
		flCost += flForwardDistance * 3.0f;

	// Sentry danger area penalty (ported from TF2 bot's CTFBotPathCost SAFEST_ROUTE)
	// TF2 bot: sentryDangerCost = 5.0f, applied when enemy sentry can fire on this area
	if (iTeam == TF_TEAM_RED && (pNextArea->m_iTFAttributeFlags & TF_NAV_BLUE_SENTRY_DANGER))
		flCost *= 5.0f;
	else if (iTeam == TF_TEAM_BLUE && (pNextArea->m_iTFAttributeFlags & TF_NAV_RED_SENTRY_DANGER))
		flCost *= 5.0f;

	// AVOID flag penalty
	if (pNextArea->m_iAttributeFlags & NAV_MESH_AVOID)
		flCost += 100000.f;

	// Crouch area penalty
	if (pNextArea->m_iAttributeFlags & NAV_MESH_CROUCH)
		flCost += flForwardDistance * 5.f;

	// === Cover spot preference (ported from TF2 bot's hiding spot usage) ===
	// TF2 bots use IN_COVER hiding spots to take cover from sentries and heal safely.
	// When the local player is low on health, prefer areas that have cover spots.
	// This makes the pathfinder route through safe spots when seeking health.
	if (!m_mDangerBlacklist.empty() || (iTeam != 0 && pNextArea->m_uHidingSpotCount > 0))
	{
		// Check if this area has cover spots
		bool bHasCover = false;
		for (const auto& tSpot : pNextArea->m_vHidingSpots)
		{
			if (tSpot.HasGoodCover())
			{
				bHasCover = true;
				break;
			}
		}

		if (bHasCover)
		{
			// Discount for areas with cover — makes pathfinder prefer routes through cover
			// Larger discount when in danger (escaping), smaller discount otherwise
			bool bInDanger = false;
			if (auto itDanger = m_mDangerBlacklist.find(pNextArea); itDanger != m_mDangerBlacklist.end())
				bInDanger = false; // This area IS in danger, no discount
			else
				bInDanger = !m_mDangerBlacklist.empty(); // Other areas are in danger

			if (bInDanger)
				flCost -= flForwardDistance * 0.5f; // Strong preference when escaping
			else
				flCost -= flForwardDistance * 0.15f; // Mild preference otherwise
		}

		// Penalty for exposed spots — avoid walking through the open when in danger
		bool bHasExposed = false;
		for (const auto& tSpot : pNextArea->m_vHidingSpots)
		{
			if (tSpot.IsExposed())
			{
				bHasExposed = true;
				break;
			}
		}
		if (bHasExposed && !m_mDangerBlacklist.empty())
			flCost += flForwardDistance * 0.3f; // Avoid exposed areas when in danger
	}

	return (std::max)(flCost, 1.f);
}

// Simple cost calculation (fallback, not used with new system)
float CMap::EvaluateConnectionCost(CNavArea* pCurrentArea, CNavArea* pNextArea) const
{
	// Simple cost calculation based on distance
	float flDistance = pCurrentArea->m_vCenter.DistTo(pNextArea->m_vCenter);
	float flCost = flDistance;

	// Add penalty for height differences
	float flHeightDiff = pNextArea->m_vCenter.z - pCurrentArea->m_vCenter.z;
	if (flHeightDiff > 0.f)
	{
		// Going up costs more
		flCost += flHeightDiff * 2.0f;
	}
	else if (flHeightDiff < -8.f)
	{
		// Going down costs a bit
		flCost += std::abs(flHeightDiff) * 0.5f;
	}

	// Add penalty for areas that should be avoided
	if (pNextArea->m_iAttributeFlags & NAV_MESH_AVOID)
		flCost += 10000.f;

	// Add penalty for crouch areas
	if (pNextArea->m_iAttributeFlags & NAV_MESH_CROUCH)
		flCost += flDistance * 2.0f;

	// Sniper spot area discount (ported from TF2 bot's CTFBotPathCost)
	// TF2 bot: areas with TF_NAV_SNIPER_SPOT are preferred when pathing as sniper
	if (pNextArea->m_iTFAttributeFlags & TF_NAV_SNIPER_SPOT)
		flCost -= flDistance * 0.3f;

	return (std::max)(flCost, 1.f);
}

CNavArea* CMap::FindClosestNavArea(const Vec3& vPos, float flMaxDist)
{
	// Like TF2 NextBot's GetNearestNavArea: prefer areas the position is actually inside (IsOverlapping),
	// falling back to closest center distance if no overlapping area is found.
	// Returns nullptr if no area is within flMaxDist of the position.
	// TF2 uses 500.0f as the default max search distance.

	const float flMaxDistSqr = flMaxDist * flMaxDist;
	float flBestDist = FLT_MAX;
	CNavArea* pBestArea = nullptr;        // Best overlapping area
	CNavArea* pBestFallbackArea = nullptr; // Fallback: closest center distance (with Z check)

	for (auto& tArea : m_navfile.m_vAreas)
	{
		float flDist = tArea.m_vCenter.DistToSqr(vPos);

		// Skip areas too far away
		if (flDist > flMaxDistSqr)
			continue;

		// Check if position is actually inside this area's 2D bounds
		if (tArea.IsOverlapping(vPos) && vPos.z >= tArea.m_flMinZ && vPos.z <= tArea.m_flMaxZ + 100.0f)
		{
			// Prefer the overlapping area that's closest by center
			if (!pBestArea || flDist < pBestArea->m_vCenter.DistToSqr(vPos))
			{
				pBestArea = &tArea;
			}
		}

		// Track closest by center (fallback) — but ONLY if the area is on the same floor.
		// Without this Z check, the fallback can pick areas on different floors or across gaps,
		// causing the bot to path through non-navmesh space or oscillate between areas.
		float flHeightDiff = std::abs(tArea.m_vCenter.z - vPos.z);
		if (flHeightDiff < 100.0f && flDist < flBestDist)
		{
			flBestDist = flDist;
			pBestFallbackArea = &tArea;
		}
	}

	// Prefer overlapping area, fall back to closest center on same floor
	return pBestArea ? pBestArea : pBestFallbackArea;
}

Vec3 CMap::FindHidingSpot(const Vec3& vFrom, float flMaxDist, unsigned char uRequiredFlags, int iTeam)
{
	if (m_eState != NavState::Active)
		return Vec3(0, 0, 0);

	float flBestDist = FLT_MAX;
	Vec3 vBestSpot = Vec3(0, 0, 0);

	float flCurTime = I::GlobalVars->curtime;

	for (auto& tArea : m_navfile.m_vAreas)
	{
		// Skip enemy spawn rooms
		if (iTeam == TF_TEAM_RED && (tArea.m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_BLUE))
			continue;
		if (iTeam == TF_TEAM_BLUE && (tArea.m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_RED))
			continue;

		// Skip blocked areas
		if (tArea.m_iTFAttributeFlags & TF_NAV_BLOCKED)
			continue;

		// Skip areas with active danger blacklist entries
		if (auto itDanger = m_mDangerBlacklist.find(&tArea); itDanger != m_mDangerBlacklist.end())
		{
			auto& entry = itDanger->second;
			bool bExpired = false;
			if (entry.m_iExpireTick > 0 && I::GlobalVars->tickcount > entry.m_iExpireTick)
				bExpired = true;
			if (entry.m_flExpireTime > 0.0f && flCurTime > entry.m_flExpireTime)
				bExpired = true;
			if (!bExpired)
				continue;
		}

		// Skip areas that don't allow hiding
		if (tArea.m_iAttributeFlags & NAV_MESH_DONT_HIDE)
			continue;

		float flAreaDist = tArea.m_vCenter.DistTo(vFrom);
		if (flAreaDist > flMaxDist)
			continue;

		// Search hiding spots in this area
		for (auto& tSpot : tArea.m_vHidingSpots)
		{
			// Check if this spot has the required flags
			if (!(tSpot.m_fFlags & uRequiredFlags))
				continue;

			float flSpotDist = tSpot.m_vPos.DistTo(vFrom);
			if (flSpotDist < flBestDist)
			{
				flBestDist = flSpotDist;
				vBestSpot = tSpot.m_vPos;
			}
		}
	}

	return vBestSpot;
}

Vec3 CMap::FindCoverSpot(const Vec3& vFrom, float flMaxDist, int iTeam)
{
	// First try to find a spot with good hard cover (IN_COVER)
	// This is what TF2 bots use for hiding from sentries and healing behind walls
	Vec3 vResult = FindHidingSpot(vFrom, flMaxDist, CHidingSpot::IN_COVER, iTeam);
	if (!vResult.IsZero())
		return vResult;

	// No IN_COVER spot found — fall back to any non-exposed spot
	// (IN_COVER | GOOD_SNIPER_SPOT | IDEAL_SNIPER_SPOT — anything that isn't EXPOSED)
	vResult = FindHidingSpot(vFrom, flMaxDist,
		CHidingSpot::IN_COVER | CHidingSpot::GOOD_SNIPER_SPOT | CHidingSpot::IDEAL_SNIPER_SPOT, iTeam);
	return vResult;
}

Vec3 CMap::FindSniperSpot(const Vec3& vFrom, float flMaxDist, int iTeam)
{
	// Prefer ideal sniper spots (can see very far or a large area)
	Vec3 vResult = FindHidingSpot(vFrom, flMaxDist, CHidingSpot::IDEAL_SNIPER_SPOT, iTeam);
	if (!vResult.IsZero())
		return vResult;

	// Fall back to good sniper spots
	vResult = FindHidingSpot(vFrom, flMaxDist, CHidingSpot::GOOD_SNIPER_SPOT, iTeam);
	return vResult;
}
