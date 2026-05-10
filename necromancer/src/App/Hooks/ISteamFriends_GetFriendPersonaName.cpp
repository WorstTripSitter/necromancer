#include "../../SDK/SDK.h"
#include "../../Utils/HookManager/HookManager.h"
#include "../Features/CFG.h"

#include "../Features/StreamerMode/StreamerMode.h"

// Exact copy from Amalgam
MAKE_SIGNATURE(GetPlayerNameForSteamID_GetFriendPersonaName_Call, "client.dll", "41 B9 ? ? ? ? 44 8B C3 48 8B C8", 0x0);

// Manual init
namespace Hooks
{
	namespace ISteamFriends_GetFriendPersonaName
	{
		void Init();
		inline CHook Hook(Init);
		using fn = const char*(__fastcall*)(void*, CSteamID);
		const char* __fastcall Func(void* rcx, CSteamID steamIDFriend);
	}
}

void Hooks::ISteamFriends_GetFriendPersonaName::Init()
{
	if (I::SteamFriends)
	{
		auto addr = Memory::GetVFunc(I::SteamFriends, 7);
		if (addr)
			Hook.Create(addr, Func);
	}
}

const char* __fastcall Hooks::ISteamFriends_GetFriendPersonaName::Func(void* rcx, CSteamID steamIDFriend)
{
	const auto dwRetAddr = uintptr_t(_ReturnAddress());
	const auto dwDesired = Signatures::GetPlayerNameForSteamID_GetFriendPersonaName_Call.Get();

	if (dwRetAddr == dwDesired && CFG::Misc_Streamer_Mode > 0)
	{
		if (const char* pName = StreamerMode::GetPlayerName(steamIDFriend.GetAccountID()))
			return pName;
	}

	return Hook.Original<fn>()(rcx, steamIDFriend);
}
