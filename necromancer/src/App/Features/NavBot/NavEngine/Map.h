#pragma once
#include "FileReader/CNavFile.h"
#include "MicroPather/micropather.h"
#include <unordered_map>
#include <limits>
#include <string>

// Undef Windows min/max macros that conflict with std::min/std::max
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// Player dimensions for pathfinding
#define PLAYER_WIDTH 49.0f
#define HALF_PLAYER_WIDTH (PLAYER_WIDTH / 2.0f)
#define PLAYER_HEIGHT 83.0f
#define PLAYER_JUMP_HEIGHT 50.0f
#define PLAYER_CROUCHED_JUMP_HEIGHT 62.0f

// Navigation points structure for pathfinding
struct NavPoints_t
{
	Vec3 m_vCurrent;      // Current area center
	Vec3 m_vCenter;       // Connection point (adjusted for safe pathing)
	Vec3 m_vCenterNext;   // Next area connection point
	Vec3 m_vNext;         // Next area center

	NavPoints_t() = default;
	NavPoints_t(const Vec3& vCurrent, const Vec3& vCenter, const Vec3& vCenterNext, const Vec3& vNext)
		: m_vCurrent(vCurrent), m_vCenter(vCenter), m_vCenterNext(vCenterNext), m_vNext(vNext) {}
};

// Dropdown hint structure for vertical navigation
struct DropdownHint_t
{
	bool m_bRequiresDrop = false;
	float m_flDropHeight = 0.f;
	float m_flApproachDistance = 0.f;
	Vec3 m_vApproachDir;
	Vec3 m_vAdjustedPos;
};

// Map state
enum class NavState
{
	Unavailable,
	Active
};

// Stuck connection tracking (ported from Amalgam's ConnectionStuckTime_t)
struct StuckConnection_t
{
	float m_flExpireTime = 0.0f;	// When this entry expires (game time)
	int m_iStuckTicks = 0;			// How many ticks we've been stuck on this connection
};

// Stuck blacklist entry (ported from Amalgam's BlacklistReason_t)
struct BlacklistEntry_t
{
	float m_flExpireTime = 0.0f;	// When this blacklist entry expires
	float m_flPenalty = 2500.0f;	// Cost penalty for pathing through this area
};

// Danger blacklist reason enum (ported from Amalgam's BlacklistReasonEnum)
enum EDangerReason
{
	DANGER_NONE = 0,
	DANGER_SENTRY,			// Sentry gun in range - very high danger
	DANGER_SENTRY_MEDIUM,	// Sentry at medium range
	DANGER_SENTRY_LOW,		// Sentry at long range
	DANGER_ENEMY_NORMAL,	// Visible enemy nearby
	DANGER_ENEMY_DORMANT,	// Dormant enemy last-known position
	DANGER_STICKY,			// Enemy sticky trap
	DANGER_ENEMY_INVULN,	// Invulnerable enemy (uber)
	DANGER_BAD_BUILD_SPOT	// Engineer building spot (not real danger)
};

// Danger blacklist entry (ported from Amalgam's FreeBlacklist)
struct DangerBlacklistEntry_t
{
	EDangerReason m_eReason = DANGER_NONE;
	float m_flExpireTime = 0.0f;	// When this entry expires (0 = no expiry)
	int m_iExpireTick = 0;		// Tick-based expiry for stickies

	DangerBlacklistEntry_t() = default;
	DangerBlacklistEntry_t(EDangerReason eReason, float flExpireTime = 0.0f, int iExpireTick = 0)
		: m_eReason(eReason), m_flExpireTime(flExpireTime), m_iExpireTick(iExpireTick) {}
};

// Get penalty for a danger reason (ported from Amalgam's GetBlacklistPenalty)
inline float GetDangerPenalty(EDangerReason eReason)
{
	switch (eReason)
	{
	case DANGER_SENTRY:			return 8000.f;	// Extremely dangerous - must avoid
	case DANGER_ENEMY_INVULN:	return 5000.f;
	case DANGER_STICKY:			return 4000.f;
	case DANGER_SENTRY_MEDIUM:	return 2500.f;
	case DANGER_ENEMY_NORMAL:	return 1500.f;
	case DANGER_SENTRY_LOW:		return 800.f;
	case DANGER_ENEMY_DORMANT:	return 400.f;
	case DANGER_BAD_BUILD_SPOT:	return 100.f;
	default:					return 0.f;
	}
}

// Simplified Map class - handles pathfinding with MicroPather
class CMap : public micropather::Graph
{
public:
	CNavFile m_navfile;
	std::string m_sMapName;
	NavState m_eState;

	// Constructor - loads nav file
	CMap(const char* sMapName)
	{
		m_navfile = CNavFile(sMapName);
		m_sMapName = sMapName;
		m_eState = m_navfile.m_bOK ? NavState::Active : NavState::Unavailable;
	}

	// MicroPather instance (3000 nodes, 6 typical adjacent, caching enabled)
	micropather::MicroPather m_pather{ this, 3000, 6, true };

	// Stuck connection tracking (ported from Amalgam)
	// Key: pair of (from area, to area) pointers - tracks how long we've been stuck on each connection
	std::unordered_map<CNavArea*, std::unordered_map<CNavArea*, StuckConnection_t>> m_mConnectionStuckTime;

	// Stuck blacklist (ported from Amalgam)
	// Key: area pointer - areas that are temporarily blacklisted due to being stuck
	std::unordered_map<CNavArea*, BlacklistEntry_t> m_mStuckBlacklist;

	// Danger blacklist (ported from Amalgam's FreeBlacklist)
	// Key: area pointer - areas marked as dangerous with reason and penalty
	std::unordered_map<CNavArea*, DangerBlacklistEntry_t> m_mDangerBlacklist;

	// MicroPather Graph interface - heuristic cost estimate (straight-line distance)
	float LeastCostEstimate(void* pStartArea, void* pEndArea) override
	{
		CNavArea* pStart = reinterpret_cast<CNavArea*>(pStartArea);
		CNavArea* pEnd = reinterpret_cast<CNavArea*>(pEndArea);
		return pStart->m_vCenter.DistTo(pEnd->m_vCenter);
	}

	// MicroPather Graph interface - get adjacent areas and their costs
	void AdjacentCost(void* pArea, std::vector<micropather::StateCost>* pAdjacent) override;

	// Find closest nav area to a position (returns nullptr if no area within flMaxDist)
	CNavArea* FindClosestNavArea(const Vec3& vPos, float flMaxDist = 500.0f);

	// Find path between two areas
	std::vector<void*> FindPath(CNavArea* pLocalArea, CNavArea* pDestArea, int* pOutResult = nullptr)
	{
		if (m_eState != NavState::Active)
			return {};

		float flCost;
		std::vector<void*> vPath;
		int iResult = m_pather.Solve(reinterpret_cast<void*>(pLocalArea), reinterpret_cast<void*>(pDestArea), &vPath, &flCost);
		if (pOutResult)
			*pOutResult = iResult;

		if (iResult == micropather::MicroPather::START_END_SAME)
			return { reinterpret_cast<void*>(pLocalArea) };

		return vPath;
	}

	// Reset pathfinding state
	void Reset()
	{
		m_pather.Reset();
	}

	// Required by MicroPather but unused
	void PrintStateInfo(void*) {}

	// Determine navigation points between two areas
	NavPoints_t DeterminePoints(CNavArea* pCurrentArea, CNavArea* pNextArea, bool bIsOneWay);

	// Handle dropdown/vertical navigation
	DropdownHint_t HandleDropdown(const Vec3& vCurrentPos, const Vec3& vNextPos, bool bIsOneWay);

	// Check if connection is one-way
	bool IsOneWay(CNavArea* pFrom, CNavArea* pTo) const;

	// Find nearest cover/hiding spot (IN_COVER flag) within max distance from position
	// TF2 bots use these for taking cover from sentries, healing behind walls, etc.
	Vec3 FindCoverSpot(const Vec3& vFrom, float flMaxDist = 1500.0f, int iTeam = 0);

	// Find nearest good sniper spot within max distance
	Vec3 FindSniperSpot(const Vec3& vFrom, float flMaxDist = 2000.0f, int iTeam = 0);

	// Find nearest hiding spot with specific flags within max distance
	Vec3 FindHidingSpot(const Vec3& vFrom, float flMaxDist, unsigned char uRequiredFlags, int iTeam);

private:
	// Calculate cost between two connected areas (simple version)
	float EvaluateConnectionCost(CNavArea* pCurrentArea, CNavArea* pNextArea) const;

	// Calculate cost between two connected areas (advanced version with NavPoints and Dropdown)
	float EvaluateConnectionCost(CNavArea* pCurrentArea, CNavArea* pNextArea, const NavPoints_t& tPoints, const DropdownHint_t& tDropdown, int iTeam) const;
};
