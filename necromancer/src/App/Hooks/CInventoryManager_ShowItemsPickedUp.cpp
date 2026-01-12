#include "../../SDK/SDK.h"
#include "../Features/CFG.h"

MAKE_SIGNATURE(CInventoryManager_ShowItemsPickedUp, "client.dll", "44 88 4C 24 ? 44 88 44 24 ? 53 41 56", 0x0);

MAKE_HOOK(CInventoryManager_ShowItemsPickedUp, Signatures::CInventoryManager_ShowItemsPickedUp.Get(), bool, __fastcall,
	void* rcx, bool bForce, bool bReturnToGame, bool bNoPanel)
{
	if (CFG::Misc_Auto_Accept_Items)
	{
		CALL_ORIGINAL(rcx, true, true, true);
		return false;
	}

	return CALL_ORIGINAL(rcx, bForce, bReturnToGame, bNoPanel);
}
