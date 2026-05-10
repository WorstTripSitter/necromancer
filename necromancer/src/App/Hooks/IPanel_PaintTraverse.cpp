#include "../../SDK/SDK.h"
#include "../../Utils/HookManager/HookManager.h"
#include "../Features/CFG.h"
#include "../Features/StreamerMode/StreamerMode.h"

// Manual init
namespace Hooks
{
	namespace IPanel_PaintTraverse
	{
		void Init();
		inline CHook Hook(Init);
		using fn = void(__fastcall*)(void*, VPANEL, bool, bool);
		void __fastcall Func(void* rcx, VPANEL vguiPanel, bool forceRepaint, bool allowForce);
	}
}

void Hooks::IPanel_PaintTraverse::Init()
{
	if (I::VGuiPanel)
	{
		auto addr = Memory::GetVFunc(I::VGuiPanel, 41);
		if (addr)
			Hook.Create(addr, Func);
	}
}

void __fastcall Hooks::IPanel_PaintTraverse::Func(void* rcx, VPANEL vguiPanel, bool forceRepaint, bool allowForce)
{
	if (CFG::Misc_Streamer_Mode <= 0)
		return Hook.Original<fn>()(rcx, vguiPanel, forceRepaint, allowForce);

	const char* pName = I::VGuiPanel->GetName(vguiPanel);
	if (pName)
	{
		if (!strcmp(pName, "SteamFriendsList") ||
			!strcmp(pName, "avatar") ||
			!strcmp(pName, "RankPanel") ||
			!strcmp(pName, "ModelContainer") ||
			!strcmp(pName, "ServerLabelNew"))
		{
			return;
		}
	}

	Hook.Original<fn>()(rcx, vguiPanel, forceRepaint, allowForce);
}
