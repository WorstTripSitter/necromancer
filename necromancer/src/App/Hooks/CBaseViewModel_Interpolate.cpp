#include "../../SDK/SDK.h"

#include "../Features/TickbaseManip/TickbaseManip.h"

MAKE_SIGNATURE(CBaseViewModel_Interpolate, "client.dll", "48 8B C4 48 89 58 ? 48 89 68 ? 48 89 70 ? 48 89 78 ? 41 56 48 83 EC ? 0F 29 70 ? 33 FF", 0x0);

MAKE_HOOK(CBaseViewModel_Interpolate, Signatures::CBaseViewModel_Interpolate.Get(), bool, __fastcall,
	void* ecx, float& currentTime)
{
	// Skip viewmodel interpolation during recharge — prevents viewmodel
	// visual artifacts when we're not sending commands to the server
	if (Shifting::bRecharging)
		return true;

	return CALL_ORIGINAL(ecx, currentTime);
}
