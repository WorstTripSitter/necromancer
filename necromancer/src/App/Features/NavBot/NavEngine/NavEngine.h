#pragma once
#include "../../../../SDK/SDK.h"
#include "Map.h"
#include <vector>
#include <memory>
#include <string>

// Undef Windows min/max macros that conflict with std::min/std::max
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// Priority enum for NavBot jobs (ported from Amalgam's PriorityListEnum)
// Higher value = higher priority. NavTo with higher priority cancels lower.
enum ENavPriority
{
	NAV_PRIO_NONE = 0,
	NAV_PRIO_PATROL = 5,
	NAV_PRIO_LOWPRIO_HEALTH,
	NAV_PRIO_STAYNEAR,
	NAV_PRIO_CAPTURE,
	NAV_PRIO_GETAMMO,
	NAV_PRIO_GETHEALTH,
	NAV_PRIO_ESCAPE_SPAWN,
	NAV_PRIO_ESCAPE_DANGER,
	NAV_PRIO_FOLLOW_TEAMMATE,
	NAV_PRIO_FOLLOW_TAGGED,
	NAV_PRIO_FOLLOW_SUPPLY  // Supply pickup while following (higher than follow so it doesn't get overridden)
};

// Jump state machine (ported from Amalgam's EJumpState)
enum EJumpState
{
	JUMP_AWAITING,
	JUMP_CTAP,
	JUMP_JUMPING,
	JUMP_ASCENDING,
	JUMP_DESCENDING
};

// Crumb structure (ported from Amalgam's Crumb_t)
// Stores a waypoint with metadata for approach direction and dropdown handling.
struct Crumb_t
{
	Vec3 m_vPos = {};              // Waypoint position
	Vec3 m_vApproachDir = {};      // Direction to walk when reaching this crumb
	float m_flApproachDistance = 0.f; // How far to push past this crumb (for drops)
	bool m_bRequiresDrop = false;  // Whether this crumb requires a drop/dropdown
	float m_flDropHeight = 0.f;    // Height of the drop at this crumb
	CNavArea* m_pNavArea = nullptr; // Nav area this crumb belongs to
};

// NavEngine - handles high-level pathfinding and path following
// Ported from Amalgam-v2 NavEngine with simplified wandering support
class CNavEngine
{
public:
	CNavEngine() = default;
	~CNavEngine() = default;

	// Public map instance for external access
	std::unique_ptr<CMap> m_pMap;

	// Current priority (public so NavBot can check it)
	ENavPriority m_eCurrentPriority = NAV_PRIO_NONE;

	// Initialize nav mesh for current map
	bool Init(const char* szMapName);

	// Start navigating to a position with priority
	bool NavTo(const Vec3& vDestination, ENavPriority ePriority = NAV_PRIO_PATROL, bool bShouldRepath = true);

	// Follow current path (call every frame)
	void FollowPath(C_TFPlayer* pLocal, CUserCmd* pCmd);

	// Cancel current path completely
	void CancelPath();

	// Abandon current path and auto-repath to same destination (like Amalgam)
	void AbandonPath(const char* sReason);

	// Check if currently pathing (including approaching destination after crumbs exhausted)
	bool IsPathing() const { return !m_vCrumbs.empty() || m_bApproachingDestination; }

	// Get current destination
	Vec3 GetDestination() const { return m_vDestination; }

	// Get last destination (for auto-repath after AbandonPath)
	Vec3 GetLastDestination() const { return m_vLastDestination; }

	// Get current crumbs (for visualization)
	const std::vector<Crumb_t>& GetCrumbs() const { return m_vCrumbs; }

	// Get current waypoint the bot is walking toward (for LookAtPath)
	Vec3 GetCurrentWaypoint() const
	{
		if (m_nCurrentCrumbIndex < m_vCrumbs.size())
			return m_vCrumbs[m_nCurrentCrumbIndex].m_vPos;
		return m_vDestination;
	}

	// Get current path direction (ported from Amalgam's GetCurrentPathDir)
	Vec3 GetCurrentPathDir() const { return m_vCurrentPathDir; }

	// Vischeck the current path - cancel if blocked (ported from Amalgam)
	void VischeckPath(C_TFPlayer* pLocal);

	// Update stuck time tracking and blacklist stuck connections (ported from Amalgam)
	void UpdateStuckTime(C_TFPlayer* pLocal);

	// Reset nav engine - reloads nav mesh for current map (call on map change)
	void Reset(bool bForced = false);

	// Check if nav engine is ready (nav mesh loaded and active)
	bool IsReady() const;

	// Check if nav mesh is loaded
	bool IsNavMeshLoaded() const { return m_pMap && m_pMap->m_eState == NavState::Active; }

	// Get nav file path
	std::string GetNavFilePath() const { return m_pMap ? m_pMap->m_sMapName : ""; }

	// Danger blacklist access (ported from Amalgam's FreeBlacklist)
	std::unordered_map<CNavArea*, DangerBlacklistEntry_t>* GetDangerBlacklist() { return m_pMap ? &m_pMap->m_mDangerBlacklist : nullptr; }
	void ClearDangerBlacklist() { if (m_pMap) m_pMap->m_mDangerBlacklist.clear(); }
	void ClearDangerBlacklist(EDangerReason eReason) {
		if (!m_pMap) return;
		std::erase_if(m_pMap->m_mDangerBlacklist, [eReason](const auto& entry) { return entry.second.m_eReason == eReason; });
	}

	// Update respawn room nav area flags (ported from Amalgam's UpdateRespawnRooms)
	void UpdateRespawnRooms();

	// Get respawn room exit areas (for EscapeSpawn)
	std::vector<CNavArea*>* GetRespawnRoomExitAreas() { return &m_vRespawnRoomExitAreas; }

	// Get local player's nav area
	CNavArea* GetLocalNavArea() const;

	// Check if we're in setup time (blue team on PL/CP maps during setup)
	// Ported from Amalgam's IsSetupTime
	bool IsSetupTime();

	// Get inactivity time (for stuck wiggle logic in NavBot)
	float GetInactivityTime() const { return m_flInactivityTime; }

private:
	std::vector<Crumb_t> m_vCrumbs;  // Current path as crumbs (ported from Amalgam)
	bool m_bApproachingDestination = false;  // Walking toward destination after crumbs exhausted
	Vec3 m_vDestination;
	Vec3 m_vLastDestination;  // Last destination for auto-repath
	size_t m_nCurrentCrumbIndex = 0;
	Vec3 m_vCurrentPathDir = {};  // Current movement direction (like Amalgam's m_vCurrentPathDir)
	std::string m_sLastMapName;  // Track last map for auto-reset
	bool m_bRepathOnFail = true;  // Auto-repath on AbandonPath
	float m_flNextVischeckTime = 0.0f;  // Throttle vischecks
	int m_nVischeckFailCount = 0;       // How many times vischeck blocked the current destination
	float m_flVischeckCooldownUntil = 0.0f;  // Don't vischeck until this time (after repeated failures)

	// Smart jump state machine (ported from Amalgam)
	EJumpState m_eJumpState = JUMP_AWAITING;
	float m_flInactivityTime = 0.0f;  // How long we've been stuck on current waypoint
	float m_flLastJumpTime = 0.0f;    // Throttle jumps (0.2s between jumps)

	// Stuck detection tracking (ported from Amalgam's UpdateStuckTime)
	CNavArea* m_pLastNavArea = nullptr;  // Previous nav area for connection tracking

	// Dropdown edge nudge - when bot reaches a drop crumb but hasn't actually fallen
	// because the navmesh doesn't cover the very edge of the structure
	bool m_bDropNudgeActive = false;       // Currently nudging forward on a drop
	Vec3 m_vDropNudgeDir = {};             // Direction to nudge (approach direction)
	float m_flDropNudgeStartTime = 0.0f;   // When the current nudge attempt started
	int m_nDropNudgeAttempts = 0;          // How many forward nudges we've tried
	float m_flDropNudgeExtraForward = 0.0f; // Extra forward distance for current nudge

	// Route variety tracking (ported from TF2 bot's CTFBotPathCost)
	// When the time modulus changes (every ~10s), path costs change and cached paths become stale
	int m_iLastRouteVarietyTimeMod = -1;

	// AbandonPath cooldown — prevents rapid route switching oscillation
	// Without this, AbandonPath→NavTo→AbandonPath→NavTo can cycle between 2 routes every tick
	float m_flLastAbandonTime = 0.0f;
	float m_flAbandonCooldown = 0.0f;  // Dynamic cooldown, increases with repeated abandons
	int m_nAbandonCount = 0;           // How many consecutive abandons for same destination

	// Minimum repath interval — prevents NavTo from being called too rapidly
	float m_flLastPathTime = 0.0f;

	// Cached local nav area — prevents FindClosestNavArea oscillation in FollowPath
	CNavArea* m_pCachedLocalArea = nullptr;
	float m_flCachedLocalAreaTime = 0.0f;

	// Respawn room tracking (ported from Amalgam's UpdateRespawnRooms)
	bool m_bUpdatedRespawnRooms = false;
	std::vector<CNavArea*> m_vRespawnRoomExitAreas;

	// Convert nav area path to world position path
	void BuildWorldPath(const std::vector<void*>& vAreaPath);

	// Calculate movement commands to reach target
	void MoveTowards(C_TFPlayer* pLocal, CUserCmd* pCmd, const Vec3& vTarget);

	// Smart jump prediction - traces forward to check if jump will land on walkable surface
	bool SmartJump(C_TFPlayer* pLocal, CUserCmd* pCmd, const Vec3& vTarget);

	// Handle smart jump state machine (c-tap, duck timing, etc.)
	void HandleSmartJump(C_TFPlayer* pLocal, CUserCmd* pCmd);

	// Force a jump (called by stuck detection)
	void ForceJump() { if (m_eJumpState == JUMP_AWAITING) m_eJumpState = JUMP_JUMPING; }

	// Check if a surface normal is walkable (not too steep)
	bool IsSurfaceWalkable(const Vec3& vNormal) const;
};

// Global nav engine instance
inline CNavEngine G_NavEngine;
