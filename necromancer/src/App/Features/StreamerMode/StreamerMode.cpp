#include "StreamerMode.h"
#include "../../Features/CFG.h"

// Name constants
#define LOCAL_NAME "Local"
#define FRIEND_NAME "Friend"
#define PARTY_NAME "Party"
#define ENEMY_NAME "Enemy"
#define TEAMMATE_NAME "Teammate"
#define PLAYER_NAME "Player"

int StreamerMode::GetNameType(int playerIndex)
{
	if (CFG::Misc_Streamer_Mode <= 0)
		return NameType_None;

	auto pResource = GetTFPlayerResource();
	if (!pResource || !pResource->IsValid(playerIndex))
		return NameType_None;

	// Local player check
	if (playerIndex == I::EngineClient->GetLocalPlayer())
	{
		if (CFG::Misc_Streamer_Mode >= 1)
			return NameType_Local;
		return NameType_None;
	}

	// Friend check
	C_TFPlayer* pPlayer = I::ClientEntityList->GetClientEntity(playerIndex)->As<C_TFPlayer>();
	if (pPlayer && pPlayer->IsPlayerOnSteamFriendsList())
	{
		if (CFG::Misc_Streamer_Mode >= 2)
			return NameType_Friend;
		return NameType_None;
	}

	// Party check
	uint32_t theirAccountId = pResource->GetAccountID(playerIndex);
	if (I::TFGCClientSystem)
	{
		if (auto pParty = I::TFGCClientSystem->GetParty())
		{
			int64_t nMembers = pParty->GetNumMembers();
			for (int64_t i = 0; i < nMembers; i++)
			{
				CSteamID memberSteamID;
				pParty->GetMember(&memberSteamID, i);
				if (memberSteamID.GetAccountID() == theirAccountId)
				{
					if (CFG::Misc_Streamer_Mode >= 3)
						return NameType_Party;
					return NameType_None;
				}
			}
		}
	}

	// All other players
	if (CFG::Misc_Streamer_Mode >= 4)
		return NameType_Player;

	return NameType_None;
}

int StreamerMode::GetNameType(uint32_t accountId)
{
	if (CFG::Misc_Streamer_Mode <= 0)
		return NameType_None;

	auto pResource = GetTFPlayerResource();
	if (!pResource)
		return NameType_None;

	// Find player index by account ID
	for (int n = 1; n <= I::EngineClient->GetMaxClients(); n++)
	{
		if (pResource->IsValid(n) && pResource->GetAccountID(n) == accountId)
			return GetNameType(n);
	}

	return NameType_None;
}

const char* StreamerMode::GetPlayerName(int playerIndex)
{
	int iType = GetNameType(playerIndex);

	switch (iType)
	{
	case NameType_Local:
		return LOCAL_NAME;
	case NameType_Friend:
		return FRIEND_NAME;
	case NameType_Party:
		return PARTY_NAME;
	case NameType_Player:
		{
			auto pResource = GetTFPlayerResource();
			if (pResource)
			{
				int localTeam = pResource->GetTeam(I::EngineClient->GetLocalPlayer());
				int theirTeam = pResource->GetTeam(playerIndex);
				return (localTeam != theirTeam) ? ENEMY_NAME : TEAMMATE_NAME;
			}
			return PLAYER_NAME;
		}
	default:
		return nullptr;
	}
}

const char* StreamerMode::GetPlayerName(uint32_t accountId)
{
	auto pResource = GetTFPlayerResource();
	if (!pResource)
		return nullptr;

	// Find player index by account ID
	for (int n = 1; n <= I::EngineClient->GetMaxClients(); n++)
	{
		if (pResource->IsValid(n) && pResource->GetAccountID(n) == accountId)
			return GetPlayerName(n);
	}

	return nullptr;
}
