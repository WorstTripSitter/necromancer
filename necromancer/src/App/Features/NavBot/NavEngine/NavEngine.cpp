#include "NavEngine.h"
#include "../../../../SDK/SDK.h"
#include "../../CFG.h"
#include "../../amalgam_port/AmalgamCompat.h"

#undef min
#undef max
#include <algorithm>
#include <string>
#include <unordered_set>

bool CNavEngine::Init(const char* szMapName)
{
	if (!szMapName || !szMapName[0])
	{
		return false;
	}

	// GetLevelName() returns "maps/mapname.bsp", we need to convert to full path to .nav file
	// Example: "maps/cp_dustbowl.bsp" -> "tf/maps/cp_dustbowl.nav"
	std::string sMapPath(szMapName);
	
	
	// Remove "maps/" prefix if present
	size_t mapsPos = sMapPath.find("maps/");
	if (mapsPos != std::string::npos)
		sMapPath = sMapPath.substr(mapsPos + 5); // Skip "maps/"
	
	// Replace .bsp with .nav
	size_t bspPos = sMapPath.find(".bsp");
	if (bspPos != std::string::npos)
		sMapPath = sMapPath.substr(0, bspPos) + ".nav";
	
	// Construct full path: "tf/maps/mapname.nav"
	std::string sNavPath = "tf/maps/" + sMapPath;
	

	// Create map instance with nav file
	m_pMap = std::make_unique<CMap>(sNavPath.c_str());
	
	if (!m_pMap || m_pMap->m_eState != NavState::Active)
	{
		if (m_pMap)
		m_pMap.reset();
		return false;
	}

	m_sLastMapName = sMapPath;
	return true;
}

bool CNavEngine::NavTo(const Vec3& vDestination, ENavPriority ePriority, bool bShouldRepath)
{
	if (!m_pMap || m_pMap->m_eState != NavState::Active)
	{
		return false;
	}

	float flCurTime = I::GlobalVars->curtime;

	// Minimum repath interval — prevents NavTo from being called every tick
	// which causes rapid route switching and oscillation.
	// Allow immediate pathing if: no current path, higher priority, or enough time elapsed.
	float flMinRepathInterval = 0.3f;  // Minimum 300ms between repaths to same destination
	bool bIsHigherPriority = ePriority > m_eCurrentPriority;
	bool bIsNewDest = vDestination.DistTo(m_vDestination) > 50.0f;
	bool bNoCurrentPath = !IsPathing();
	if (!bIsHigherPriority && !bIsNewDest && !bNoCurrentPath && (flCurTime - m_flLastPathTime) < flMinRepathInterval)
		return false;

	// Route variety cache invalidation (ported from TF2 bot's CTFBotPathCost)
	// When the time modulus changes (every ~10s), path costs shift and cached paths become stale.
	// Reset the pather so the new route preferences take effect.
	if (CFG::NavBot_RouteVariety)
	{
		int iTimeMod = static_cast<int>(flCurTime / 10.0f) + 1;
		if (m_iLastRouteVarietyTimeMod >= 0 && iTimeMod != m_iLastRouteVarietyTimeMod)
		{
			m_pMap->m_pather.Reset();
		}
		m_iLastRouteVarietyTimeMod = iTimeMod;
	}

	// Don't path if priority is too low (like Amalgam)
	if (ePriority < m_eCurrentPriority)
	{
		return false;
	}

	// Find closest nav area to destination
	CNavArea* pDestArea = m_pMap->FindClosestNavArea(vDestination);
	if (!pDestArea)
	{
		return false;
	}

	// Validate that the destination is actually on a nav area.
	// FindClosestNavArea can return a fallback area that's just the closest center,
	// but the destination itself might not be on any nav area at all.
	// If the destination isn't overlapping its closest nav area, it's in non-navmesh
	// space — don't path there. "If the road is dark, don't drive where you can't see."
	{
		bool bDestOverlapping = pDestArea->IsOverlapping(vDestination) &&
		                        vDestination.z >= pDestArea->m_flMinZ && vDestination.z <= pDestArea->m_flMaxZ + 100.0f;
		if (!bDestOverlapping)
		{
			// Destination is in non-navmesh space — cancel
			return false;
		}
	}

	// Get local player to find starting area
	auto pLocal = I::ClientEntityList->GetClientEntity(I::EngineClient->GetLocalPlayer());
	if (!pLocal)
	{
		return false;
	}

	auto pLocalPlayer = pLocal->As<C_TFPlayer>();
	if (!pLocalPlayer)
	{
		return false;
	}

	Vec3 vLocalPos = pLocalPlayer->m_vecOrigin();
	CNavArea* pLocalArea = m_pMap->FindClosestNavArea(vLocalPos);
	if (!pLocalArea)
	{
		return false;
	}

	// Validate that the starting area is actually reachable from the bot's position.
	// The bot can be slightly off the navmesh edge (IsOverlapping returns false),
	// so we allow a 200-unit tolerance. But if the bot is far from any nav area,
	// don't try to path through non-navmesh space.
	Vec3 vDistToArea = pLocalArea->m_vCenter - vLocalPos;
	vDistToArea.z = 0.0f;
	float flDistToAreaCenter = vDistToArea.Length();
	bool bIsOverlapping = pLocalArea->IsOverlapping(vLocalPos) &&
	                      vLocalPos.z >= pLocalArea->m_flMinZ && vLocalPos.z <= pLocalArea->m_flMaxZ + 100.0f;
	if (!bIsOverlapping && flDistToAreaCenter > 200.0f)
	{
		return false;
	}


	// Find path using MicroPather
	int iResult = 0;
	std::vector<void*> vAreaPath = m_pMap->FindPath(pLocalArea, pDestArea, &iResult);
	
	// Handle single-area path (start and end in same area)
	if (vAreaPath.empty() && iResult == micropather::MicroPather::START_END_SAME)
	{
		// Validate that the direct path is actually passable with a hull trace.
		// A large nav area can span across a wall/obstacle, so a straight-line path
		// within the same area can still cross walls.
		const Vec3 vHullMin(-20.0f, -20.0f, 0.0f);
		const Vec3 vHullMax(20.0f, 20.0f, 72.0f);
		const Vec3 vStepHeight(0.0f, 0.0f, 18.0f);
		CTraceFilterWorldAndPropsOnlyAmalgam filter = {};
		CGameTrace trace = {};
		H::AimUtils->TraceHull(vLocalPos + vStepHeight, vDestination + vStepHeight, vHullMin, vHullMax, MASK_PLAYERSOLID_BRUSHONLY, &filter, &trace);

		if (trace.fraction >= 1.0f || trace.startsolid)
		{
			// Path is clear (or startsolid which means we're inside geometry, just proceed)
			m_vCrumbs.clear();
			Crumb_t tStartCrumb = {};
			tStartCrumb.m_vPos = vLocalPos;
			tStartCrumb.m_pNavArea = pLocalArea;
			m_vCrumbs.push_back(tStartCrumb);
			Crumb_t tEndCrumb = {};
			tEndCrumb.m_vPos = vDestination;
			tEndCrumb.m_pNavArea = pLocalArea;
			m_vCrumbs.push_back(tEndCrumb);
			m_vDestination = vDestination;
			m_vLastDestination = vDestination;
			m_nCurrentCrumbIndex = 0;
			m_eCurrentPriority = ePriority;
			m_bRepathOnFail = bShouldRepath;
			m_flLastPathTime = flCurTime;
			return true;
		}

		// Direct path is blocked by a wall within the same area.
		// Try to find a multi-area path by resetting the pather and searching again.
		m_pMap->m_pather.Reset();
		vAreaPath = m_pMap->FindPath(pLocalArea, pDestArea, &iResult);
		if (vAreaPath.empty())
		{
			return false;
		}
	}

	// No path found
	if (vAreaPath.empty())
	{
		return false;
	}

	// If this is a new destination (not a repath to same place), reset vischeck failure tracking
	bool bIsNewDestination = vDestination.DistTo(m_vDestination) > 1.0f || m_eCurrentPriority == NAV_PRIO_NONE;

	// Set destination BEFORE building path so BuildWorldPath can append it correctly
	m_vDestination = vDestination;
	m_vLastDestination = vDestination;
	m_bRepathOnFail = bShouldRepath;

	if (bIsNewDestination)
	{
		m_nVischeckFailCount = 0;
		m_flVischeckCooldownUntil = 0.0f;
	}

	// Convert area path to crumbs (TF2-style edge point navigation)
	BuildWorldPath(vAreaPath);

	// Insert a start crumb at the player's actual position.
	// The first edge crumb can be ahead of the bot but in a different direction
	// than the bot is facing — without a start crumb, the bot might walk
	// backward or sideways to reach it. The start crumb ensures smooth
	// departure from the current position.
	if (!m_vCrumbs.empty())
	{
		Crumb_t tStartCrumb = {};
		tStartCrumb.m_vPos = vLocalPos;
		tStartCrumb.m_pNavArea = m_vCrumbs[0].m_pNavArea;  // Same area as first edge crumb
		m_vCrumbs.insert(m_vCrumbs.begin(), tStartCrumb);
	}

	// Skip any crumbs the bot has already passed (dot product check).
	// This prevents walking back to crumbs that are behind the bot's forward direction.
	if (m_vCrumbs.size() >= 2)
	{
		Vec3 vForward, vRight;
		Math::AngleVectors(pLocalPlayer->GetEyeAngles(), &vForward, &vRight, nullptr);
		vForward.z = 0.0f;
		vForward.Normalize();

		// Skip crumbs that are behind us relative to our facing direction
		while (m_vCrumbs.size() >= 2)
		{
			Vec3 vDirToNext = m_vCrumbs[1].m_vPos - vLocalPos;
			vDirToNext.z = 0.0f;
			float flLen = vDirToNext.Length();
			if (flLen < 1.0f)
			{
				m_vCrumbs.erase(m_vCrumbs.begin());
				continue;
			}
			vDirToNext /= flLen;

			// If next crumb is ahead of us, stop skipping
			float flDot = vForward.Dot(vDirToNext);
			if (flDot > -0.1f)  // Not behind us (allow slight sideways)
				break;

			// Next crumb is behind us - skip it
			m_vCrumbs.erase(m_vCrumbs.begin());
		}
	}

	m_nCurrentCrumbIndex = 0;
	m_eCurrentPriority = ePriority;
	m_flLastPathTime = flCurTime;

	return !m_vCrumbs.empty();
}

void CNavEngine::BuildWorldPath(const std::vector<void*>& vAreaPath)
{
	m_vCrumbs.clear();

	if (vAreaPath.empty())
		return;

	// Build crumbs for TF2-style path following.
	// Unlike Amalgam which uses area centers as waypoints, the TF2 bot navigates
	// through edge connection points. Area centers can be in spots that require
	// walking through walls for large/irregular areas, causing the bot to walk
	// on non-navmeshed space. Edge points are where the bot actually needs to turn.
	//
	// For each area pair, we create one crumb at the connection edge point
	// (with dropdown/approach metadata). The last area gets the destination.
	for (size_t i = 0; i < vAreaPath.size(); i++)
	{
		CNavArea* pArea = reinterpret_cast<CNavArea*>(vAreaPath[i]);
		if (!pArea)
			continue;

		// All entries besides the last need a crumb at the connection edge
		if (i != vAreaPath.size() - 1)
		{
			CNavArea* pNextArea = reinterpret_cast<CNavArea*>(vAreaPath[i + 1]);
			if (!pNextArea)
			{
				Crumb_t tFallback = {};
				tFallback.m_vPos = pArea->m_vCenter;
				tFallback.m_pNavArea = pArea;
				m_vCrumbs.push_back(tFallback);
				continue;
			}

			bool bIsOneWay = m_pMap->IsOneWay(pArea, pNextArea);
			NavPoints_t tPoints = m_pMap->DeterminePoints(pArea, pNextArea, bIsOneWay);
			DropdownHint_t tDropdown = m_pMap->HandleDropdown(tPoints.m_vCenter, tPoints.m_vNext, bIsOneWay);
			tPoints.m_vCenter = tDropdown.m_vAdjustedPos;

			// Single crumb at the edge connection point (like TF2 bot's path waypoints)
			// This is where the bot needs to actually turn — not the area center.
			Crumb_t tEdgeCrumb = {};
			tEdgeCrumb.m_pNavArea = pArea;
			tEdgeCrumb.m_vPos = tPoints.m_vCenter;
			tEdgeCrumb.m_bRequiresDrop = tDropdown.m_bRequiresDrop;
			tEdgeCrumb.m_flDropHeight = tDropdown.m_flDropHeight;
			tEdgeCrumb.m_flApproachDistance = tDropdown.m_flApproachDistance;
			tEdgeCrumb.m_vApproachDir = tDropdown.m_vApproachDir;
			m_vCrumbs.push_back(tEdgeCrumb);
		}
		else
		{
			// tEndCrumb: destination (like Amalgam's tEndCrumb)
			Crumb_t tEndCrumb = {};
			tEndCrumb.m_pNavArea = pArea;
			tEndCrumb.m_vPos = m_vDestination;
			m_vCrumbs.push_back(tEndCrumb);
		}
	}

	// Remove consecutive duplicate crumbs (same position)
	for (size_t i = 1; i < m_vCrumbs.size(); )
	{
		if ((m_vCrumbs[i].m_vPos - m_vCrumbs[i - 1].m_vPos).Length2D() < 1.0f)
			m_vCrumbs.erase(m_vCrumbs.begin() + i);
		else
			i++;
	}
}

void CNavEngine::FollowPath(C_TFPlayer* pLocal, CUserCmd* pCmd)
{
	if (!pLocal || !pCmd || m_vCrumbs.empty())
		return;

	// Update respawn room flags once per map load (ported from Amalgam)
	if (!m_bUpdatedRespawnRooms)
		UpdateRespawnRooms();

	// Check if we've reached the end
	if (m_nCurrentCrumbIndex >= m_vCrumbs.size())
	{
		CancelPath();
		return;
	}

	Vec3 vLocalPos = pLocal->m_vecOrigin();
	float flCurTime = I::GlobalVars->curtime;

	// Track inactivity for stuck detection (like Amalgam's m_tInactivityTimer)
	Vec3 vVelocity = pLocal->m_vecVelocity();
	if (vVelocity.Length2D() > 40.0f)
		m_flInactivityTime = flCurTime;

	// Track on-ground state for dropdown nudge logic
	bool bOnGround = (pLocal->m_fFlags() & FL_ONGROUND) != 0;

	// === Stairs detection (ported from TF2 bot's PathFollower) ===
	// The TF2 bot checks NAV_MESH_STAIRS on both the bot's area and the goal area.
	// On stairs, the physics engine handles step-ups automatically — the path follower
	// just walks forward and doesn't abandon/skip crumbs due to height differences.
	bool bOnStairs = false;
	{
		// Cache the local nav area query — FindClosestNavArea can return different areas
		// on consecutive frames when the bot is near area boundaries, causing oscillation.
		// Only re-query every 200ms or when moved more than 50 units.
		CNavArea* pLocalArea = m_pCachedLocalArea;
		if (!pLocalArea || (flCurTime - m_flCachedLocalAreaTime) > 0.2f)
		{
			pLocalArea = m_pMap->FindClosestNavArea(vLocalPos);
			m_pCachedLocalArea = pLocalArea;
			m_flCachedLocalAreaTime = flCurTime;
		}
		if (pLocalArea && (pLocalArea->m_iAttributeFlags & NAV_MESH_STAIRS))
			bOnStairs = true;
		// Check current crumb's nav area (like TF2 bot's PathFollower checks goal area)
		Crumb_t& tActiveCrumb0 = m_vCrumbs[m_nCurrentCrumbIndex];
		if (tActiveCrumb0.m_pNavArea && (tActiveCrumb0.m_pNavArea->m_iAttributeFlags & NAV_MESH_STAIRS))
			bOnStairs = true;
		// Also check next crumb's nav area for stair transitions (like TF2 bot checks upcoming segments)
		if (m_nCurrentCrumbIndex + 1 < m_vCrumbs.size())
		{
			Crumb_t& tNextCrumb0 = m_vCrumbs[m_nCurrentCrumbIndex + 1];
			if (tNextCrumb0.m_pNavArea && (tNextCrumb0.m_pNavArea->m_iAttributeFlags & NAV_MESH_STAIRS))
				bOnStairs = true;
		}
	}

	// === Height deviation check ===
	// Ported from TF2 bot's PathFollower::Update: on stairs, the goal can be higher
	// than the bot's jump height, but the bot should still walk toward it because
	// the physics engine handles step-ups. Don't abandon the path on stairs.
	if (!bOnStairs)
	{
		Crumb_t& tActive = m_vCrumbs[m_nCurrentCrumbIndex];
		float flHeightDiff = std::abs(vLocalPos.z - tActive.m_vPos.z);
		if (flHeightDiff > 100.0f)
		{
			bool bNextCrumbAlsoWrongHeight = true;
			if (m_nCurrentCrumbIndex + 1 < m_vCrumbs.size())
			{
				float flNextHeightDiff = std::abs(vLocalPos.z - m_vCrumbs[m_nCurrentCrumbIndex + 1].m_vPos.z);
				if (flNextHeightDiff <= 100.0f)
					bNextCrumbAlsoWrongHeight = false;
			}

			if (bNextCrumbAlsoWrongHeight)
			{
				AbandonPath("Height deviation (wrong floor)");
				return;
			}
		}
	}

	// === Crumb advancement (ported from Amalgam's FollowCrumbs) ===
	// Amalgam uses reach radii: kDefaultReachRadius=50, kDropReachRadius=28
	constexpr float kDefaultReachRadius = 50.f;
	constexpr float kDropReachRadius = 28.f;

	while (m_nCurrentCrumbIndex < m_vCrumbs.size())
	{
		Crumb_t& tActive = m_vCrumbs[m_nCurrentCrumbIndex];
		Vec3 vDelta = tActive.m_vPos - vLocalPos;
		float flHeightDiff = std::abs(vDelta.z);
		vDelta.z = 0.0f;
		float flDist = vDelta.Length();

		// Use smaller reach radius for drop crumbs (like Amalgam)
		float flReachRadius = tActive.m_bRequiresDrop ? kDropReachRadius : kDefaultReachRadius;

		// Strategy 1: Close enough to current crumb — advance
		// On stairs, relax height check — the physics engine handles step-ups
		// (ported from TF2 bot's PathFollower which doesn't skip ahead on uphill)
		float flHeightTolerance = bOnStairs ? 200.0f : 72.0f;
		if (flDist < flReachRadius && flHeightDiff < flHeightTolerance)
		{
			// === Dropdown edge nudge ===
			// When a crumb requires a drop, the navmesh may not cover the very edge
			// of the structure. The bot reaches the crumb position but hasn't actually
			// fallen because it's not at the real edge. Don't advance — instead nudge
			// forward incrementally until we fall off.
			// Skip this check on stairs (gradual height change is normal on stairs).
			if (tActive.m_bRequiresDrop && bOnGround && !bOnStairs)
			{
				// Check if we've actually dropped — our Z should be lower than the crumb's Z
				float flDropProgress = tActive.m_vPos.z - vLocalPos.z;
				if (flDropProgress < 18.0f) // Haven't dropped at least step height
				{
					// We're at the drop point but haven't fallen — the navmesh edge
					// doesn't match the real geometry edge. Activate nudge mode.
					if (!m_bDropNudgeActive)
					{
						m_bDropNudgeActive = true;
						m_flDropNudgeStartTime = flCurTime;
						m_nDropNudgeAttempts = 0;
						m_flDropNudgeExtraForward = 15.0f; // Start with 15 unit nudge

						// Use the crumb's approach direction, or fall back to direction to next crumb
						m_vDropNudgeDir = tActive.m_vApproachDir;
						m_vDropNudgeDir.z = 0.0f;
						if (m_vDropNudgeDir.Length() < 0.01f && m_nCurrentCrumbIndex + 1 < m_vCrumbs.size())
						{
							m_vDropNudgeDir = m_vCrumbs[m_nCurrentCrumbIndex + 1].m_vPos - tActive.m_vPos;
							m_vDropNudgeDir.z = 0.0f;
							float flDirLen = m_vDropNudgeDir.Length();
							if (flDirLen > 0.01f) m_vDropNudgeDir /= flDirLen;
						}
					}
					// Don't advance past this crumb — let the nudge logic walk us forward
					break;
				}
			}

			// Normal advancement (or we've successfully dropped)
			m_nCurrentCrumbIndex++;
			m_flInactivityTime = flCurTime;
			m_bDropNudgeActive = false;
			m_nDropNudgeAttempts = 0;
			m_flDropNudgeExtraForward = 0.0f;
			continue;
		}

		// Strategy 2: Lookahead — close to next crumb (skip current)
		if (m_nCurrentCrumbIndex + 1 < m_vCrumbs.size())
		{
			Crumb_t& tNext = m_vCrumbs[m_nCurrentCrumbIndex + 1];
			Vec3 vNextDelta = tNext.m_vPos - vLocalPos;
			float flNextHeightDiff = std::abs(vNextDelta.z);
			vNextDelta.z = 0.0f;
			float flNextDist = vNextDelta.Length();

			float flNextReach = tNext.m_bRequiresDrop ? kDropReachRadius : kDefaultReachRadius;

			float flNextHeightTolerance = bOnStairs ? 200.0f : 72.0f;
			if (flNextDist < flNextReach && flNextHeightDiff < flNextHeightTolerance)
			{
				m_nCurrentCrumbIndex++;  // Skip current, land on next (was += 2 for old 2-crumb system)
				m_flInactivityTime = flCurTime;
				continue;
			}

			// Strategy 3: Behind-check — current crumb is behind us along the path
			// If we're past a waypoint (closer to the next one) and the current one is
			// behind us, skip it immediately. Don't walk backward to a waypoint we already passed.
			Vec3 vPathDir = tNext.m_vPos - tActive.m_vPos;
			vPathDir.z = 0.0f;
			float flPathLen = vPathDir.Length();
			if (flPathLen > 1.0f)
			{
				vPathDir /= flPathLen;
				Vec3 vToCrumb = vDelta;
				float flToCrumbLen = vDelta.Length();
				if (flToCrumbLen > 1.0f)
				{
					vToCrumb /= flToCrumbLen;
					float flDot = vPathDir.Dot(vToCrumb);
					// On stairs, allow behind-check even with height differences
					float flBehindHeightTolerance = bOnStairs ? 200.0f : 72.0f;
					// If we're closer to the next crumb than the current crumb,
					// and the current crumb is even slightly behind us, skip it current one.
					// This prevents the bot from walking backward to a waypoint it already passed.
					bool bNextIsCloser = flNextDist < flDist;
					if (bNextIsCloser && flDot < -0.1f && flHeightDiff < flBehindHeightTolerance)
					{
						m_nCurrentCrumbIndex++;
						m_flInactivityTime = flCurTime;
						continue;
					}
				}
			}
		}

		break;
	}

	// Check if we've reached the end after advancing
	if (m_nCurrentCrumbIndex >= m_vCrumbs.size())
	{
		// If we're still far from the actual destination, keep walking toward it.
		// This prevents a path-complete-cancel-repath cycle where the bot reaches
		// the last crumb but hasn't touched the supply entity yet.
		float flDistToDest = (vLocalPos - m_vDestination).Length2D();
		if (flDistToDest > 30.0f)
		{
			// Set approaching flag so IsPathing() returns true — prevents
			// NavBot from re-calling NavTo every tick during this phase
			m_bApproachingDestination = true;
			Vec3 vMoveTarget = m_vDestination;
			vMoveTarget.z = vLocalPos.z;
			MoveTowards(pLocal, pCmd, vMoveTarget);
			return;
		}
		m_bApproachingDestination = false;
		CancelPath();
		return;
	}

	// === Compute move target (ported from Amalgam's FollowCrumbs) ===
	// This is the key difference from the old system: we use the crumb's m_vApproachDir
	// and the direction to the next crumb to determine movement, NOT just walking directly
	// to the current crumb position. This is what prevents cutting through walls at corners.
	Crumb_t& tActiveCrumb = m_vCrumbs[m_nCurrentCrumbIndex];
	size_t uCrumbsSize = m_vCrumbs.size();

	// Determine the movement direction (like Amalgam's vMoveDir)
	// Priority: use m_vApproachDir from the crumb, fallback to direction to next crumb
	Vec3 vMoveDir = tActiveCrumb.m_vApproachDir;
	vMoveDir.z = 0.0f;
	float flDirLen = vMoveDir.Length();
	if (flDirLen < 0.01f && uCrumbsSize > 1 && m_nCurrentCrumbIndex + 1 < uCrumbsSize)
	{
		// No approach dir set — use direction from current crumb to next crumb
		vMoveDir = m_vCrumbs[m_nCurrentCrumbIndex + 1].m_vPos - tActiveCrumb.m_vPos;
		vMoveDir.z = 0.0f;
		flDirLen = vMoveDir.Length();
	}
	if (flDirLen > 0.01f)
		vMoveDir /= flDirLen;
	else
		vMoveDir = {};

	// Store current path direction for external use (like Amalgam's m_vCurrentPathDir)
	m_vCurrentPathDir = vMoveDir;

	// Compute vMoveTarget like Amalgam's FollowCrumbs
	Vec3 vMoveTarget = tActiveCrumb.m_vPos;

	if (!vMoveDir.IsZero())
	{
		// If this crumb has an approach distance (dropdown), push the target forward
		if (tActiveCrumb.m_flApproachDistance > 0.0f)
		{
			vMoveTarget = tActiveCrumb.m_vPos + vMoveDir * tActiveCrumb.m_flApproachDistance;
		}

		// === Cornering logic (ported from TF2 bot's PathFollower) ===
		// The TF2 bot walks directly toward each waypoint and only starts steering
		// toward the next segment when very close. The old Amalgam-style blend
		// started too early (100 units) with an 80-unit look-ahead, which caused
		// the bot to cut corners and walk into walls or outside navmesh areas.
		//
		// New approach: when within reach radius of the current crumb, start
		// blending the move target toward the next crumb. This ensures the bot
		// actually reaches the turn point before cutting.
		Vec3 vToCrumb = tActiveCrumb.m_vPos - vLocalPos;
		vToCrumb.z = 0.0f;
		float flDistToCrumb = vToCrumb.Length();

		if (flDistToCrumb < kDefaultReachRadius && m_nCurrentCrumbIndex + 1 < uCrumbsSize)
		{
			// We're close to the current crumb — start steering toward the next one.
			// Blend factor: at reach radius edge, barely steering; at center, full steer.
			float flBlend = 1.0f - (flDistToCrumb / kDefaultReachRadius);
			flBlend = flBlend * flBlend;  // Quadratic — gradual transition

			// Target: the next crumb position (not a look-ahead past the current crumb)
			Vec3 vNextCrumbPos = m_vCrumbs[m_nCurrentCrumbIndex + 1].m_vPos;
			vNextCrumbPos.z = tActiveCrumb.m_vPos.z;

			// Blend between current crumb and next crumb
			vMoveTarget = tActiveCrumb.m_vPos + (vNextCrumbPos - tActiveCrumb.m_vPos) * flBlend;
		}
	}

	// Reset Z to local height (like Amalgam's bResetHeight logic)
	vMoveTarget.z = vLocalPos.z;

	// === Dropdown edge nudge move override ===
	// When the bot is at a drop crumb but hasn't actually fallen (navmesh doesn't
	// cover the real edge), override the move target to push further forward.
	// Incrementally increase the nudge distance if we don't fall after each attempt.
	if (m_bDropNudgeActive && !m_vDropNudgeDir.IsZero())
	{
		// If we're no longer on ground, we've fallen — deactivate nudge and advance
		if (!bOnGround)
		{
			m_bDropNudgeActive = false;
			m_nDropNudgeAttempts = 0;
			m_flDropNudgeExtraForward = 0.0f;
			m_nCurrentCrumbIndex++;
			m_flInactivityTime = flCurTime;
		}
		else
		{
			// Override move target — walk forward past the navmesh edge
			vMoveTarget = vLocalPos + m_vDropNudgeDir * (50.0f + m_flDropNudgeExtraForward);
			vMoveTarget.z = vLocalPos.z;

			// Check if this nudge attempt has timed out
			float flNudgeTime = flCurTime - m_flDropNudgeStartTime;
			if (flNudgeTime > 0.4f)
			{
				m_nDropNudgeAttempts++;
				m_flDropNudgeExtraForward += 20.0f; // Increase nudge distance each attempt

				if (m_nDropNudgeAttempts > 4)
				{
					// Can't reach the edge after multiple attempts — abandon path
					m_bDropNudgeActive = false;
					m_nDropNudgeAttempts = 0;
					m_flDropNudgeExtraForward = 0.0f;
					AbandonPath("Drop nudge failed - can't reach edge");
					return;
				}

				m_flDropNudgeStartTime = flCurTime; // Reset timer for next attempt
			}
		}
	}

	MoveTowards(pLocal, pCmd, vMoveTarget);

	// Handle smart jump state machine (ported from Amalgam's HandleSmartJump)
	if (CFG::NavBot_AutoJump)
		HandleSmartJump(pLocal, pCmd);
}

void CNavEngine::MoveTowards(C_TFPlayer* pLocal, CUserCmd* pCmd, const Vec3& vTarget)
{
	if (!pLocal || !pCmd)
		return;

	Vec3 vLocalPos = pLocal->m_vecOrigin();
	
	// Calculate direction to target (2D only) — like Amalgam's ComputeMove/WalkTo
	Vec3 vDirection = vTarget - vLocalPos;
	vDirection.z = 0.0f;
	
	float flDist = vDirection.Length();
	if (flDist < 1.0f)
		return;
	
	vDirection.Normalize();
	
	// Get view angles
	Vec3 vViewAngles = pCmd->viewangles;
	
	// Convert direction to forward/side move based on view angles
	Vec3 vForward, vRight;
	Math::AngleVectors(vViewAngles, &vForward, &vRight, nullptr);
	vForward.z = 0.0f;
	vRight.z = 0.0f;
	vForward.Normalize();
	vRight.Normalize();
	
	// Calculate movement commands (like Amalgam's ComputeMove)
	float flForwardMove = vDirection.Dot(vForward) * 450.0f;
	float flSideMove = vDirection.Dot(vRight) * 450.0f;
	
	pCmd->forwardmove = flForwardMove;
	pCmd->sidemove = flSideMove;
}

void CNavEngine::CancelPath()
{
	m_vCrumbs.clear();
	m_nCurrentCrumbIndex = 0;
	m_bApproachingDestination = false;
	m_vCurrentPathDir = {};
	m_eCurrentPriority = NAV_PRIO_NONE;
	m_eJumpState = JUMP_AWAITING;
	m_flInactivityTime = 0.0f;
	m_nVischeckFailCount = 0;
	m_flVischeckCooldownUntil = 0.0f;
	m_bDropNudgeActive = false;
	m_nDropNudgeAttempts = 0;
	m_flDropNudgeExtraForward = 0.0f;
	m_nAbandonCount = 0;
	m_flAbandonCooldown = 0.0f;
	m_pCachedLocalArea = nullptr;
	m_flCachedLocalAreaTime = 0.0f;
}

void CNavEngine::UpdateStuckTime(C_TFPlayer* pLocal)
{
	if (!pLocal || !m_pMap || m_pMap->m_eState != NavState::Active || m_vCrumbs.empty())
		return;

	float flCurTime = I::GlobalVars->curtime;

	// Get current nav area — use cached result from FollowPath if available
	CNavArea* pCurrentArea = m_pCachedLocalArea;
	if (!pCurrentArea || (flCurTime - m_flCachedLocalAreaTime) > 0.2f)
	{
		pCurrentArea = m_pMap->FindClosestNavArea(pLocal->m_vecOrigin());
		m_pCachedLocalArea = pCurrentArea;
		m_flCachedLocalAreaTime = flCurTime;
	}

	// Check if we're making progress (2D velocity check, like Amalgam)
	Vec3 vVelocity = pLocal->m_vecVelocity();
	if (vVelocity.Length2D() > 40.0f)
	{
		m_flInactivityTime = flCurTime;  // Moving, reset inactivity
		m_pLastNavArea = pCurrentArea;
		return;
	}

	// We're stuck (no 2D movement) - track per-connection stuck time
	float flStuckTime = flCurTime - m_flInactivityTime;
	float flTrigger = 1.0f;  // StuckTime / 2 (like Amalgam's default)

	if (flStuckTime > flTrigger && m_pLastNavArea && pCurrentArea)
	{
		// Track stuck time on this specific connection (from→to)
		auto& rFromMap = m_pMap->m_mConnectionStuckTime[m_pLastNavArea];
		auto& rEntry = rFromMap[pCurrentArea];
		rEntry.m_iStuckTicks++;
		rEntry.m_flExpireTime = flCurTime + 10.0f;  // Expires in 10 seconds

		// If stuck for too many ticks on this connection, blacklist it and repath
		// (like Amalgam's iDetectTicks check)
		int iDetectTicks = 30;  // ~0.46s at 66 tick rate
		if (rEntry.m_iStuckTicks > iDetectTicks)
		{
			// Blacklist the destination area (like Amalgam's blacklist)
			m_pMap->m_mStuckBlacklist[pCurrentArea] = {
				flCurTime + 10.0f,  // Blacklist expires in 10 seconds
				2500.0f             // Massive penalty
			};

			// Reset pather so it recalculates with the blacklist
			m_pMap->m_pather.Reset();

			AbandonPath("Stuck (connection blacklisted)");
			return;
		}
	}

	m_pLastNavArea = pCurrentArea;
}

void CNavEngine::AbandonPath(const char* sReason)
{
	float flCurTime = I::GlobalVars->curtime;

	// Check if we're within a cooldown from a previous abandon for the same destination.
	// This breaks the AbandonPath→NavTo→AbandonPath→NavTo oscillation loop.
	Vec3 vLastDest = m_vLastDestination;
	bool bSameDest = !vLastDest.IsZero() && m_vDestination.DistTo(vLastDest) < 50.0f;

	if (bSameDest && m_nAbandonCount > 1 && (flCurTime - m_flLastAbandonTime) < m_flAbandonCooldown)
	{
		// Within cooldown for same destination — clear path but DON'T auto-repath
		m_vCrumbs.clear();
		m_nCurrentCrumbIndex = 0;
		m_vCurrentPathDir = {};
		m_eCurrentPriority = NAV_PRIO_NONE;
		// Keep m_vLastDestination so NavBot can repath when cooldown expires
		return;
	}

	// Track consecutive abandons — if abandoning rapidly, increase cooldown
	if (bSameDest && (flCurTime - m_flLastAbandonTime) < 2.0f)
	{
		m_nAbandonCount++;
		// Exponential cooldown: 0.5s, 1s, 2s, 4s... capped at 5s
		m_flAbandonCooldown = (std::min)(0.5f * (1 << (std::min)(m_nAbandonCount, 4)), 5.0f);
	}
	else
	{
		// Different destination or enough time passed — reset
		m_nAbandonCount = 0;
		m_flAbandonCooldown = 0.3f;  // Small base cooldown
	}

	m_flLastAbandonTime = flCurTime;

	// Save destination and priority before clearing
	ENavPriority eLastPrio = m_eCurrentPriority;
	bool bLastRepath = m_bRepathOnFail;

	// Clear current path
	m_vCrumbs.clear();
	m_nCurrentCrumbIndex = 0;
	m_vCurrentPathDir = {};
	m_eCurrentPriority = NAV_PRIO_NONE;

	// Auto-repath to same destination (like Amalgam's AbandonPath)
	if (bLastRepath && !vLastDest.IsZero())
	{
		NavTo(vLastDest, eLastPrio, true);
	}
}

void CNavEngine::VischeckPath(C_TFPlayer* pLocal)
{
	if (!pLocal || m_vCrumbs.size() < 2)
		return;

	float flCurTime = I::GlobalVars->curtime;

	// If vischeck has failed too many times for this destination, enter cooldown
	// to break the vischeck-abandon-repath infinite loop
	if (flCurTime < m_flVischeckCooldownUntil)
		return;

	// Throttle vischecks (every 0.5s, like Amalgam)
	if (flCurTime < m_flNextVischeckTime)
		return;
	m_flNextVischeckTime = flCurTime + 0.5f;

	// Player hull dimensions (like Amalgam's IsWalkable)
	const Vec3 vHullMin(-20.0f, -20.0f, 0.0f);
	const Vec3 vHullMax(20.0f, 20.0f, 72.0f);
	const Vec3 vStepHeight(0.0f, 0.0f, 18.0f);  // m_flStepSize approximation

	// Only check segments ahead of the current crumb index
	// (no point checking segments we already passed)
	size_t nStartIdx = (m_nCurrentCrumbIndex > 0) ? m_nCurrentCrumbIndex - 1 : 0;

	// Only check segments near the bot (within 200 units).
	// Checking distant segments is unreliable — edge-to-edge traces between adjacent
	// areas can clip through corners on valid paths, causing false positives that
	// trigger unnecessary AbandonPath calls and rapid route switching.
	Vec3 vLocalPos = pLocal->m_vecOrigin();
	const float flVischeckRange = 200.0f;

	for (size_t i = nStartIdx; i < m_vCrumbs.size() - 1; i++)
	{
		// Skip segments that are far from the bot
		Vec3 vSegMid = (m_vCrumbs[i].m_vPos + m_vCrumbs[i + 1].m_vPos) * 0.5f;
		Vec3 vToSeg = vSegMid - vLocalPos;
		vToSeg.z = 0.0f;
		if (vToSeg.Length() > flVischeckRange)
			continue;

		Vec3 vFrom = m_vCrumbs[i].m_vPos;
		Vec3 vTo = m_vCrumbs[i + 1].m_vPos;

		CTraceFilterWorldAndPropsOnlyAmalgam filter = {};
		CGameTrace trace = {};

		H::AimUtils->TraceHull(vFrom + vStepHeight, vTo + vStepHeight, vHullMin, vHullMax, MASK_PLAYERSOLID_BRUSHONLY, &filter, &trace);

		if (trace.fraction < 1.0f && !trace.startsolid)
		{
			// Blacklist the nav areas of the blocked segment so the next path avoids them
			CNavArea* pFromArea = m_vCrumbs[i].m_pNavArea;
			CNavArea* pToArea = m_vCrumbs[i + 1].m_pNavArea;

			if (pToArea)
			{
				m_pMap->m_mStuckBlacklist[pToArea] = {
					flCurTime + 15.0f,  // Blacklist for 15 seconds
					5000.0f             // Heavy penalty
				};
			}
			if (pFromArea && pFromArea != pToArea)
			{
				m_pMap->m_mStuckBlacklist[pFromArea] = {
					flCurTime + 15.0f,
					3000.0f
				};
			}

			// Track vischeck failures for this destination
			m_nVischeckFailCount++;
			if (m_nVischeckFailCount >= 3)
			{
				// Too many failures — enter cooldown to break the loop
				// During cooldown, the bot will just follow the path as-is
				// (the stuck detection system will handle it if the bot actually gets stuck)
				m_flVischeckCooldownUntil = flCurTime + 5.0f;
				return;
			}

			// Reset pather so it recalculates with the new blacklists
			m_pMap->m_pather.Reset();

			AbandonPath("Path blocked by world (hull trace)");
			return;
		}
	}

	// Path is clear — reset failure count
	m_nVischeckFailCount = 0;
}

void CNavEngine::Reset(bool bForced)
{
	CancelPath();

	// Get current map name from engine
	const char* szLevelName = I::EngineClient->GetLevelName();
	if (!szLevelName || !szLevelName[0])
	{
		m_pMap.reset();
		m_sLastMapName.clear();
		return;
	}

	// Extract map name from "maps/mapname.bsp"
	std::string sMapPath(szLevelName);
	size_t mapsPos = sMapPath.find("maps/");
	if (mapsPos != std::string::npos)
		sMapPath = sMapPath.substr(mapsPos + 5);
	size_t bspPos = sMapPath.find(".bsp");
	if (bspPos != std::string::npos)
		sMapPath = sMapPath.substr(0, bspPos) + ".nav";

	// If map hasn't changed and not forced, skip reload
	if (!bForced && sMapPath == m_sLastMapName && m_pMap && m_pMap->m_eState == NavState::Active)
	{
		return;
	}

	// Reload nav mesh for new map
	std::string sNavPath = "tf/maps/" + sMapPath;
	m_pMap = std::make_unique<CMap>(sNavPath.c_str());

	if (!m_pMap || m_pMap->m_eState != NavState::Active)
	{
		m_pMap.reset();
		m_sLastMapName.clear();
		return;
	}

	m_sLastMapName = sMapPath;
	m_vRespawnRoomExitAreas.clear();
	m_bUpdatedRespawnRooms = false;
}

bool CNavEngine::IsReady() const
{
	return m_pMap && m_pMap->m_eState == NavState::Active;
}

CNavArea* CNavEngine::GetLocalNavArea() const
{
	if (!m_pMap || m_pMap->m_eState != NavState::Active)
		return nullptr;
	auto pLocal = I::ClientEntityList->GetClientEntity(I::EngineClient->GetLocalPlayer());
	if (!pLocal) return nullptr;
	auto pPlayer = pLocal->As<C_TFPlayer>();
	if (!pPlayer) return nullptr;
	return m_pMap->FindClosestNavArea(pPlayer->m_vecOrigin());
}

bool CNavEngine::IsSurfaceWalkable(const Vec3& vNormal) const
{
	// Surface is walkable if it's not too steep (normal Z > 0.7, about 45 degrees)
	// This matches Amalgam's IsSurfaceWalkable check
	return vNormal.z > 0.7f;
}

bool CNavEngine::IsSetupTime()
{
	// Ported from Amalgam's IsSetupTime
	// During setup time on PL/CP maps, blue team can't leave spawn
	// The bot should stop pathing and wait for setup to end
	static float flLastCheckTime = 0.0f;
	static bool bSetupTime = false;

	float flNow = I::GlobalVars->curtime;
	if (flNow - flLastCheckTime < 0.5f)
		return bSetupTime;
	flLastCheckTime = flNow;

	auto pLocal = H::Entities->GetLocal();
	if (!pLocal || !pLocal->IsAlive())
		return bSetupTime;

	if (auto pGameRules = I::TFGameRules())
	{
		auto pBase = reinterpret_cast<std::uintptr_t>(pGameRules);

		// Read round state from CTeamplayRoundBasedRulesProxy netvar
		static int nRoundStateOffset = NetVars::GetNetVar("CTeamplayRoundBasedRulesProxy", "m_iRoundState");
		static int nInSetupOffset = NetVars::GetNetVar("CTeamplayRoundBasedRulesProxy", "m_bInSetup");
		static int nInWaitingOffset = NetVars::GetNetVar("CTeamplayRoundBasedRulesProxy", "m_bInWaitingForPlayers");

		if (nRoundStateOffset > 0)
		{
			int nRoundState = *reinterpret_cast<int*>(pBase + nRoundStateOffset);

			// GR_STATE_PREROUND = 3 - all players frozen
			if (nRoundState == 3)
				return bSetupTime = true;
		}

		// Blue team can't move during setup on PL/CP maps
		if (pLocal->m_iTeamNum() == TF_TEAM_BLUE)
		{
			bool bInSetup = nInSetupOffset > 0 ? *reinterpret_cast<bool*>(pBase + nInSetupOffset) : false;
			bool bInWaiting = nInWaitingOffset > 0 ? *reinterpret_cast<bool*>(pBase + nInWaitingOffset) : false;

			if (bInSetup)
				return bSetupTime = true;

			// On PL/CP maps, waiting for players also counts as setup for blue
			if (bInWaiting)
			{
				std::string sLevelName = I::EngineClient->GetLevelName();
				if (sLevelName.find("pl_") == 0 || sLevelName.find("cp_") == 0 || sLevelName.find("plr_") == 0)
					return bSetupTime = true;
			}
		}

		bSetupTime = false;
	}

	return bSetupTime;
}

bool CNavEngine::SmartJump(C_TFPlayer* pLocal, CUserCmd* pCmd, const Vec3& vTarget)
{
	if (!pLocal || !pLocal->IsAlive())
		return false;

	bool bOnGround = (pLocal->m_fFlags() & FL_ONGROUND) != 0;
	if (!bOnGround)
	{
		// Already in air - if we're pressing jump, that's fine
		if (pCmd->buttons & IN_JUMP)
			return true;
		return false;
	}

	// Calculate jump trajectory prediction (ported from Amalgam's SmartJump)
	Vec3 vVelocity = pLocal->m_vecVelocity();

	// Factor in current movement input
	Vec3 vMoveInput(pCmd->forwardmove, -pCmd->sidemove, 0.0f);
	if (vMoveInput.Length() > 0.0f)
	{
		Vec3 vViewAngles = pCmd->viewangles;
		Vec3 vForward, vRight;
		Math::AngleVectors(vViewAngles, &vForward, &vRight, nullptr);
		vForward.z = vRight.z = 0.0f;
		vForward.Normalize();
		vRight.Normalize();

		Vec3 vRotatedMoveDir = vForward * vMoveInput.x + vRight * vMoveInput.y;
		vVelocity = vRotatedMoveDir.Normalized() * std::max(10.0f, vVelocity.Length2D());
	}

	// TF2 jump physics constants
	constexpr float flJumpForce = 277.0f;
	constexpr float flGravity = 800.0f;
	float flTimeToPeak = flJumpForce / flGravity;
	float flDistTravelled = vVelocity.Length2D() * flTimeToPeak;

	Vec3 vJumpDirection = vVelocity.Normalized();

	// Check if jump direction aligns with path direction
	Vec3 vPathDir = (vTarget - pLocal->m_vecOrigin());
	vPathDir.z = 0.0f;
	float flPathLen = vPathDir.Length();
	if (flPathLen > 0.1f)
	{
		vPathDir /= flPathLen;
		// Don't jump if we're not moving toward the target
		if (vJumpDirection.Dot(vPathDir) < 0.5f)
			return false;

		// Don't jump if the path turns sharply right after current crumb
		if (m_nCurrentCrumbIndex + 1 < m_vCrumbs.size())
		{
			Vec3 vNextDir = m_vCrumbs[m_nCurrentCrumbIndex + 1].m_vPos - m_vCrumbs[m_nCurrentCrumbIndex].m_vPos;
			vNextDir.z = 0.0f;
			if (vNextDir.Normalize() > 0.1f && vPathDir.Dot(vNextDir) < 0.707f)
			{
				// Path turns sharply - only jump if we're far from current crumb
				Vec3 vDistToCrumb = m_vCrumbs[m_nCurrentCrumbIndex].m_vPos - pLocal->m_vecOrigin();
				vDistToCrumb.z = 0.0f;
				if (vDistToCrumb.Length() < 100.0f)
					return false;
			}
		}

		vJumpDirection = vPathDir;
	}

	// Trace forward to predict landing position
	Vec3 vJumpPeakPos = pLocal->m_vecOrigin() + vJumpDirection * flDistTravelled;

	const Vec3 vHullMinSjump(-16.0f, -16.0f, 0.0f);
	const Vec3 vHullMaxSjump(16.0f, 16.0f, 62.0f);
	const Vec3 vHullMin(-23.99f, -23.99f, 0.0f);
	const Vec3 vHullMax(23.99f, 23.99f, 62.0f);
	const Vec3 vStepHeight(0.0f, 0.0f, 18.0f);
	const Vec3 vMaxJumpHeight(0.0f, 0.0f, 72.0f);

	Vec3 vTraceStart = pLocal->m_vecOrigin() + vStepHeight;
	Vec3 vTraceEnd = vTraceStart + vJumpDirection * flDistTravelled;

	CGameTrace forwardTrace = {};
	CTraceFilterWorldAndPropsOnlyAmalgam filter = {};
	H::AimUtils->TraceHull(vTraceStart, vTraceEnd, vHullMinSjump, vHullMaxSjump, MASK_PLAYERSOLID_BRUSHONLY, &filter, &forwardTrace);

	if (forwardTrace.fraction < 1.0f && !IsSurfaceWalkable(forwardTrace.plane.normal))
	{
		// Hit a wall - trace down from hit point to find landing
		CGameTrace downwardTrace = {};
		H::AimUtils->TraceHull(forwardTrace.endpos, forwardTrace.endpos - vMaxJumpHeight, vHullMinSjump, vHullMaxSjump, MASK_PLAYERSOLID_BRUSHONLY, &filter, &downwardTrace);

		Vec3 vLandingPos = downwardTrace.endpos + vJumpDirection * 10.0f;
		CGameTrace landingTrace = {};
		H::AimUtils->TraceHull(vLandingPos + vMaxJumpHeight, vLandingPos, vHullMin, vHullMax, MASK_PLAYERSOLID_BRUSHONLY, &filter, &landingTrace);

		if (landingTrace.fraction > 0.0f && landingTrace.fraction < 0.75f)
		{
			if (IsSurfaceWalkable(landingTrace.plane.normal))
				return true;
		}
	}
	else if (forwardTrace.fraction >= 1.0f)
	{
		// No wall hit - trace down from end of trajectory to find ground
		CGameTrace downwardTrace = {};
		H::AimUtils->TraceHull(vTraceEnd, vTraceEnd - vMaxJumpHeight, vHullMinSjump, vHullMaxSjump, MASK_PLAYERSOLID_BRUSHONLY, &filter, &downwardTrace);

		if (downwardTrace.fraction > 0.0f && downwardTrace.fraction < 0.75f)
		{
			if (IsSurfaceWalkable(downwardTrace.plane.normal))
				return true;
		}
	}

	return false;
}

void CNavEngine::HandleSmartJump(C_TFPlayer* pLocal, CUserCmd* pCmd)
{
	if (!pLocal || !pLocal->IsAlive())
	{
		m_eJumpState = JUMP_AWAITING;
		return;
	}

	bool bOnGround = (pLocal->m_fFlags() & FL_ONGROUND) != 0;
	bool bDucking = (pLocal->m_fFlags() & FL_DUCKING) != 0;
	float flCurTime = I::GlobalVars->curtime;

	// Reset state if we're on ground and ducking (landed from a jump)
	if (bOnGround && bDucking)
		m_eJumpState = JUMP_AWAITING;

	// Not pathing? Reset jump state
	if (!IsPathing())
	{
		m_eJumpState = JUMP_AWAITING;
		return;
	}

	// Get current crumb for jump prediction
	Vec3 vTarget = m_vCrumbs[m_nCurrentCrumbIndex].m_vPos;

	switch (m_eJumpState)
	{
	case JUMP_AWAITING:
	{
		// Check if we should jump:
		// 1. We're stuck (inactivity timer exceeded) - like Amalgam's stuck detection
		// 2. SmartJump predicts we can land the jump
		// 3. The next waypoint is significantly higher (height-based trigger)

		bool bShouldJump = false;

		// Check if we're stuck (like Amalgam's inactivity timer check)
		float flStuckTime = flCurTime - m_flInactivityTime;
		bool bIsStuck = flStuckTime > 1.0f;  // Stuck after 1 second of no progress

		// Check if we're on a nav area that disallows jumping
		bool bPreventJump = false;
		if (m_pMap && m_pMap->m_eState == NavState::Active)
		{
			CNavArea* pLocalArea = m_pMap->FindClosestNavArea(pLocal->m_vecOrigin());
			if (pLocalArea && (pLocalArea->m_iAttributeFlags & (NAV_MESH_NO_JUMP | NAV_MESH_STAIRS)))
				bPreventJump = true;
		}

		// Don't jump if next crumb is significantly below (going down a drop)
		if (m_nCurrentCrumbIndex + 1 < m_vCrumbs.size())
		{
			float flHeightDiff = m_vCrumbs[m_nCurrentCrumbIndex].m_vPos.z - m_vCrumbs[m_nCurrentCrumbIndex + 1].m_vPos.z;
			if (flHeightDiff < 0.0f && flHeightDiff <= -72.0f)  // PLAYER_JUMP_HEIGHT
				bPreventJump = true;
		}

		if (!bPreventJump && bOnGround)
		{
			if (bIsStuck)
			{
				// We're stuck - try SmartJump prediction
				if (SmartJump(pLocal, pCmd, vTarget))
					bShouldJump = true;
				else if (flStuckTime > 2.0f)
				{
					// Stuck for too long - just force a jump even without prediction
					// (like Amalgam's fallback stuck jump)
					bShouldJump = true;
				}
			}
			else
			{
				// Not stuck but check if we need to jump for height
				float flHeightDiff = vTarget.z - pLocal->m_vecOrigin().z;
				if (flHeightDiff > 18.0f)
				{
					// Target is higher - use SmartJump to verify we can make it
					if (SmartJump(pLocal, pCmd, vTarget))
						bShouldJump = true;
				}
			}
		}

		if (bShouldJump && (flCurTime - m_flLastJumpTime) > 0.2f)
		{
			m_eJumpState = JUMP_JUMPING;
			m_flLastJumpTime = flCurTime;
		}
		break;
	}

	case JUMP_JUMPING:
		// Execute the jump - release duck, press jump
		pCmd->buttons &= ~IN_DUCK;
		pCmd->buttons |= IN_JUMP;
		m_eJumpState = JUMP_ASCENDING;
		break;

	case JUMP_ASCENDING:
		// Duck while ascending for height optimization (like Amalgam)
		pCmd->buttons |= IN_DUCK;
		if (pLocal->m_vecVelocity().z <= 0.0f)
			m_eJumpState = JUMP_DESCENDING;
		else if (bOnGround)
			m_eJumpState = JUMP_AWAITING;
		break;

	case JUMP_DESCENDING:
		// Unduck while descending to land faster
		pCmd->buttons &= ~IN_DUCK;
		if (bOnGround)
		{
			// Landed - duck to absorb impact, then reset
			pCmd->buttons |= IN_DUCK;
			m_eJumpState = JUMP_AWAITING;
		}
		else
		{
			// Still in air - check if we should chain another jump
			if (SmartJump(pLocal, pCmd, vTarget))
			{
				pCmd->buttons &= ~IN_DUCK;
				pCmd->buttons |= IN_JUMP;
				m_eJumpState = JUMP_JUMPING;
				m_flLastJumpTime = flCurTime;
			}
		}
		break;
	}
}

// Ported from Amalgam's UpdateRespawnRooms
// Scans for CFuncRespawnRoom entities and sets TF_NAV_SPAWN_ROOM flags on nav areas
void CNavEngine::UpdateRespawnRooms()
{
	if (!m_pMap || m_pMap->m_eState != NavState::Active)
		return;

	// Collect all CFuncRespawnRoom entities
	struct SpawnRoom_t
	{
		int m_iTeam;
		Vec3 m_vWorldMins;
		Vec3 m_vWorldMaxs;
	};

	std::vector<SpawnRoom_t> vSpawnRooms;

	for (int i = 0; i < I::ClientEntityList->GetMaxEntities(); i++)
	{
		auto pEntity = I::ClientEntityList->GetClientEntity(i);
		if (!pEntity)
			continue;

		if (pEntity->GetClassId() != ETFClassIds::CFuncRespawnRoom)
			continue;

		auto pBase = pEntity->As<C_BaseEntity>();
		if (!pBase)
			continue;

		int iTeam = pBase->m_iTeamNum();
		Vec3 vOrigin = pBase->m_vecOrigin();
		Vec3 vMins = pBase->m_vecMins();
		Vec3 vMaxs = pBase->m_vecMaxs();

		// Compute world-space AABB
		Vec3 vWorldMins(vOrigin.x + vMins.x, vOrigin.y + vMins.y, vOrigin.z + vMins.z);
		Vec3 vWorldMaxs(vOrigin.x + vMaxs.x, vOrigin.y + vMaxs.y, vOrigin.z + vMaxs.z);

		vSpawnRooms.push_back({ iTeam, vWorldMins, vWorldMaxs });
	}

	if (vSpawnRooms.empty())
	{
		m_bUpdatedRespawnRooms = true;
		return;
	}

	// Flag nav areas that are within spawn rooms
	std::unordered_set<CNavArea*> setSpawnRoomAreas;

	for (auto& tRoom : vSpawnRooms)
	{
		uint32_t uFlags = (tRoom.m_iTeam == TF_TEAM_BLUE) ? TF_NAV_SPAWN_ROOM_BLUE : TF_NAV_SPAWN_ROOM_RED;

		for (auto& tArea : m_pMap->m_navfile.m_vAreas)
		{
			if (setSpawnRoomAreas.contains(&tArea))
				continue;

			// Check if any corner of the nav area is within the spawn room AABB
			static Vec3 vStepHeight(0.0f, 0.0f, 18.0f);
			Vec3 vPoints[] = {
				tArea.m_vCenter + vStepHeight,
				tArea.m_vNwCorner + vStepHeight,
				tArea.GetNeCorner() + vStepHeight,
				tArea.GetSwCorner() + vStepHeight,
				tArea.m_vSeCorner + vStepHeight
			};

			for (auto& vPoint : vPoints)
			{
				if (vPoint.x >= tRoom.m_vWorldMins.x && vPoint.x <= tRoom.m_vWorldMaxs.x &&
					vPoint.y >= tRoom.m_vWorldMins.y && vPoint.y <= tRoom.m_vWorldMaxs.y &&
					vPoint.z >= tRoom.m_vWorldMins.z && vPoint.z <= tRoom.m_vWorldMaxs.z)
				{
					setSpawnRoomAreas.insert(&tArea);
					if (!(tArea.m_iTFAttributeFlags & uFlags))
						tArea.m_iTFAttributeFlags |= uFlags;
					break;
				}
			}
		}
	}

	// Set spawn room exit attributes on areas connected to spawn rooms
	for (auto pArea : setSpawnRoomAreas)
	{
		for (auto& tConnection : pArea->m_vConnections)
		{
			if (!(tConnection.m_pArea->m_iTFAttributeFlags & (TF_NAV_SPAWN_ROOM_RED | TF_NAV_SPAWN_ROOM_BLUE | TF_NAV_SPAWN_ROOM_EXIT)))
			{
				tConnection.m_pArea->m_iTFAttributeFlags |= TF_NAV_SPAWN_ROOM_EXIT;
				m_vRespawnRoomExitAreas.push_back(tConnection.m_pArea);
			}
		}
	}

	m_bUpdatedRespawnRooms = true;
}
