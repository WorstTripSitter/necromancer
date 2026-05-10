#pragma once
#include "../../../SDK/SDK.h"
#include <vector>
#include <unordered_map>

class CNavArea;  // Forward declaration for danger blacklist escape target

// Undef Windows min/max macros that conflict with std::min/std::max
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// NavBot - Objective-based navigation with proper nav mesh pathfinding
// Captures objectives and wanders when idle using A* pathfinding
// NO aiming functionality - aimbot handles that separately
class CNavBot
{
private:
	bool m_bActive = false;
	C_BaseEntity* m_pTargetEntity = nullptr;
	Vec3 m_vTargetPos = Vec3(0, 0, 0);
	Vec3 m_vWanderTarget = Vec3(0, 0, 0);
	float m_flNextWanderTime = 0.0f;
	float m_flNextRepathTime = 0.0f;  // Throttle repathing (like Amalgam's Timer)

	// Per-tick position tracking for instant teleport detection
	Vec3 m_vLastTickPos = Vec3(0, 0, 0);	// Position from previous tick
	bool m_bHasLastTickPos = false;			// Whether m_vLastTickPos has been set

	// Stuck detection (windowed - compares position every N ticks to avoid false positives in water/slow movement)
	Vec3 m_vStuckCheckPos = Vec3(0, 0, 0);	// Position snapshot for windowed stuck check
	int m_nStuckCheckTicks = 0;			// Ticks since last snapshot
	int m_nStuckCount = 0;				// Number of consecutive stuck windows
	int m_nStuckJumpCooldown = 0;		// Cooldown between stuck jumps
	bool m_bStuckWiggle = false;		// Alternate strafe direction when stuck

	// Amalgam-style visited areas tracking (prevents revisiting same spots)
	std::vector<Vec3> m_vVisitedAreaCenters;
	float m_flNextVisitedClearTime = 0.0f;
	int m_nConsecutiveFails = 0;

	// === LookAtPath system (ported from Amalgam's BotUtils) ===
	Vec3 m_vLastAngles = Vec3(0, 0, 0);		// Last view angles for smooth interpolation
	float m_flLookSpeed = 6.0f;			// Smooth aim speed

	// === StayNear / Enemy tracking (ported from Amalgam's StayNear + BotUtils) ===
	int m_iStayNearTargetIdx = -1;			// Entity index of the player we're stalking
	float m_flNextStayNearCheck = 0.0f;	// Cooldown for StayNear repathing

	// Dormant position cache - stores last known positions for dormant players
	struct DormantPos_t {
		Vec3 m_vOrigin = Vec3(0, 0, 0);
		float m_flTime = 0.0f;
		bool m_bValid = false;
	};
	std::unordered_map<int, DormantPos_t> m_tDormantPositions;

	// Closest enemy tracking
	struct ClosestEnemy_t {
		int m_iEntIdx = -1;
		C_TFPlayer* m_pPlayer = nullptr;
		float m_flDist = FLT_MAX;
	};
	ClosestEnemy_t m_tClosestEnemy;

	// Find nearest capturable objective (control point, flag, payload cart)
	bool FindObjective(C_TFPlayer* pLocal, Vec3& vOut);

	// Find nearest health/ammo pack when low (ported from Amalgam's GetSupplies)
	bool FindSupply(C_TFPlayer* pLocal, Vec3& vOut, bool bHealth);

	// Check if we should search for health/ammo (ported from Amalgam's ShouldSearchHealth/ShouldSearchAmmo)
	bool ShouldSearchHealth(C_TFPlayer* pLocal);
	bool ShouldSearchAmmo(C_TFPlayer* pLocal);

	// Pick random nav area as wander target (uses nav mesh if available)
	// Ported from Amalgam's Roam.cpp - avoids spawn rooms, visited areas, nearby areas
	void PickRandomWanderTarget(C_TFPlayer* pLocal);

	// === LookAtPath - smooth view angle control while pathing (ported from Amalgam) ===
	void LookAtPath(C_TFPlayer* pLocal, CUserCmd* pCmd, const Vec3& vTarget);

	// === Sniper Spot - move to navmesh sniper sightline spots (ported from TF2 bot's CTFBotSniperLurk) ===
	bool SniperSpot(C_TFPlayer* pLocal);
	Vec3 m_vSniperSpotPos = Vec3(0, 0, 0);  // Current sniper spot we're pathing to
	float m_flSniperPatienceEnd = 0.0f;   // When our patience at this spot expires (pick new spot)
	float m_flNextSniperSearchTime = 0.0f; // Throttle how often we search for new spots

	// === StayNear - stalk enemy players (ported from Amalgam's StayNear) ===
	bool StayNear(C_TFPlayer* pLocal);

	// === Dormant tracking - get last known position of dormant players ===
	bool GetDormantOrigin(int iEntIndex, Vec3& vOut);

	// === Update closest enemy tracker ===
	void UpdateCloseEnemies(C_TFPlayer* pLocal);

	// === EscapeDanger - flee from enemies/sentries when taking fire (ported from Amalgam) ===
	bool EscapeDanger(C_TFPlayer* pLocal);
	bool EscapeProjectiles(C_TFPlayer* pLocal);
	bool EscapeSpawn(C_TFPlayer* pLocal);
	void UpdateDangerBlacklist(C_TFPlayer* pLocal);
	CNavArea* m_pEscapeTargetArea = nullptr;  // Area we're escaping to
	Vec3 m_vPreEscapeDestination = Vec3(0, 0, 0);  // Where we were going before escaping (stay near objective)
	float m_flEscapeCooldownEnd = 0.0f;  // Prevent Capture from re-pathing through danger immediately after escape

	// === Weapon switching (ported from Amalgam's UpdateBestSlot/SetSlot) ===
	void UpdateBestSlot(C_TFPlayer* pLocal);
	void SwitchWeapon(C_TFPlayer* pLocal);
	int m_iBestSlot = -1;
	int m_iCurrentSlot = -1;

	// === Follow Teammates (ported from Amalgam's FollowBot) ===
	bool FollowTeammates(C_TFPlayer* pLocal);
	int m_iFollowTargetIdx = -1;  // Ent index of teammate we're following
	bool m_bIsFollowingTeammate = false;  // Are we currently following a teammate?

	// === Follow Tagged Players (follow players with "Follow Player" tag) ===
	bool FollowTaggedPlayer(C_TFPlayer* pLocal);
	int m_iFollowTaggedTargetIdx = -1;  // Ent index of tagged player we're following
	bool m_bIsFollowingTaggedPlayer = false;  // Are we currently following a tagged player?
	bool m_bIsGettingSupply = false;  // Pausing follow to grab health/ammo nearby
	C_BaseEntity* m_pSupplyEntity = nullptr;  // The supply entity we're pathing to (for cancellation checks)
	int m_iSupplyEntityIdx = -1;  // Cached entindex for safe validation without dereferencing dangling pointer
	bool m_bSupplyIsDispenser = false;  // Is the supply entity a dispenser? (needs proximity wait behavior)
	bool m_bWaitingAtDispenser = false;  // Standing near dispenser waiting to be healed/refilled (don't restart path)

	// === Death pause (stop moving after respawn for a configurable duration) ===
	float m_flDeathPauseEndTime = 0.0f;  // Time at which death pause expires
	bool m_bWasDeadLastTick = false;     // Track death state to detect respawn

	// === Auto Join Class (ported from Amalgam's AutoJoin) ===
	void AutoJoinClass(C_TFPlayer* pLocal);
	void AutoJoinTeam(C_TFPlayer* pLocal);
	float m_flNextAutoJoinTeamTime = 0.0f;
	float m_flNextAutoJoinClassTime = 0.0f;

	// === Auto Scope (ported from Amalgam's BotUtils::AutoScope) ===
	void AutoScope(C_TFPlayer* pLocal, C_TFWeaponBase* pWeapon, CUserCmd* pCmd);
	bool m_bAutoScopeKeep = false;  // Keep scope active
	float m_flAutoScopeTimer = 0.0f;  // Timer for cancel timeout
	std::unordered_map<int, bool> m_mAutoScopeCache;  // Cached visibility results
	float m_flAutoScopeLastShotTime = 0.0f;  // Time of last scoped shot (for wait-after-shot)
	bool m_bAutoScopeWasScopedLastTick = false;  // Were we scoped last tick?

public:
	// Main update function - call every tick when Active is enabled
	void Run(C_TFPlayer* pLocal, CUserCmd* pCmd);

	// Stop navigation
	void Stop();

	// Check if currently navigating
	bool IsActive() const { return m_bActive; }

	// Debug rendering
	void Draw();
};

inline CNavBot g_NavBot;
