#pragma once

#include "../../../SDK/SDK.h"

// Name type flags (bitmask for privacy checks)
enum ENameType
{
	NameType_None = 0,
	NameType_Local = 1 << 0,
	NameType_Friend = 1 << 1,
	NameType_Party = 1 << 2,
	NameType_Player = 1 << 3,
	NameType_Privacy = NameType_Local | NameType_Friend | NameType_Party | NameType_Player
};

namespace StreamerMode
{
	// Get the name type for a player (returns bitmask)
	int GetNameType(int playerIndex);
	int GetNameType(uint32_t accountId);

	// Get the replacement name for a player (returns nullptr if no replacement)
	const char* GetPlayerName(int playerIndex);
	const char* GetPlayerName(uint32_t accountId);

	// Check if player should have privacy protection
	inline bool ShouldHide(int playerIndex) { return (GetNameType(playerIndex) & NameType_Privacy) != 0; }
	inline bool ShouldHide(uint32_t accountId) { return (GetNameType(accountId) & NameType_Privacy) != 0; }
}
