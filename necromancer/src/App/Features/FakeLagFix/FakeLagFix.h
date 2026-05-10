#pragma once
#include "../../../SDK/SDK.h"

// FakeLag Fix - handles shooting fakelagging enemies
// Strategy: Instead of just waiting for unchoke (which gives enemies free movement time),
// we predict where they actually are using their velocity and choke duration,
// then set tick_count to target the server's known position.
//
// How fakelag works on the server:
// - Enemy chokes N ticks → server stores all N commands
// - When enemy unchokes, server processes all stored commands at once
// - Server's lag compensation knows the enemy's REAL position at any tick
// - Our client only sees the last update (stale position during choke)
//
// How we counter it:
// - Detect choke by checking simtime gaps (simtime - oldSimtime > 1 tick)
// - Predict where the enemy actually is using their last known velocity
// - Set tick_count to the enemy's real simulation time (which the server knows)
// - This works because the server backtracks to whatever tick_count we send

constexpr int FAKELAG_MAX_TICKS = 24;
constexpr int FAKELAG_CHOKE_THRESHOLD = 2;  // Consider choking if no update for 2+ ticks
constexpr int FAKELAG_MAX_SLOTS = 65;        // Max entity slots

class CFakeLagFix
{
public:
	// Returns true if we should shoot (target just unchoked or isn't choking)
	bool ShouldShoot(C_TFPlayer* pTarget);
	
	// Get how many ticks the player choked on their last update
	int GetChokedTicks(C_TFPlayer* pPlayer);

	// Is this player currently choking (no recent simtime update)?
	bool IsChoking(C_TFPlayer* pPlayer);

	// Get the number of ticks since the last simtime update for this player
	int GetTicksSinceUpdate(C_TFPlayer* pPlayer);

	// Get the predicted position of a fakelagging player
	// Uses their last known velocity to extrapolate where they actually are
	// This is what the server knows but our client doesn't
	Vec3 GetPredictedPosition(C_TFPlayer* pPlayer);

	// Get the predicted simulation time for a fakelagging player
	// The server has their real simtime, we need to estimate it
	float GetPredictedSimTime(C_TFPlayer* pPlayer);

	// Update tracking data - call once per frame for all players
	void Update();

private:
	// Per-entity tracking data
	struct ChokeData_t
	{
		float flLastSimTime = -1.0f;
		float flLastOldSimTime = -1.0f;
		int nTicksSinceUpdate = 0;
		int nLastChokedTicks = 0;     // How many ticks they choked on their last update
		bool bActive = false;         // Is this slot in use?
	};

	ChokeData_t m_ChokeData[FAKELAG_MAX_SLOTS] = {};
};

MAKE_SINGLETON_SCOPED(CFakeLagFix, FakeLagFix, F);
