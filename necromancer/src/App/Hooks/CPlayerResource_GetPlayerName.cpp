#include "../../SDK/SDK.h"
#include "../Features/CFG.h"

#include "../Features/StreamerMode/StreamerMode.h"

// Exact copy from Amalgam
MAKE_SIGNATURE(CPlayerResource_GetPlayerName, "client.dll", "48 89 5C 24 ? 56 48 83 EC ? 48 63 F2", 0x0);

// Manual init
namespace Hooks
{
	namespace CPlayerResource_GetPlayerName
	{
		void Init();
		inline CHook Hook(Init);
		using fn = const char*(__fastcall*)(void*, int);
		const char* __fastcall Func(void* rcx, int iIndex);
	}
}

void Hooks::CPlayerResource_GetPlayerName::Init()
{
	auto addr = Signatures::CPlayerResource_GetPlayerName.Get();
	if (addr)
	{
		Hook.Create(reinterpret_cast<void*>(addr), Func);
		MH_EnableHook(reinterpret_cast<void*>(addr));
	}
}

const char* __fastcall Hooks::CPlayerResource_GetPlayerName::Func(void* rcx, int iIndex)
{
	if (CFG::Misc_Streamer_Mode > 0)
	{
		if (const char* pName = StreamerMode::GetPlayerName(iIndex))
			return pName;
	}

	return Hook.Original<fn>()(rcx, iIndex);
}
