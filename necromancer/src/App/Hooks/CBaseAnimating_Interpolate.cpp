#include "../../SDK/SDK.h"

#include "../Features/CFG.h"
#include "../Features/TickbaseManip/TickbaseManip.h"

MAKE_SIGNATURE(CBaseAnimating_Interpolate, "client.dll", "48 8B C4 48 89 70 ? F3 0F 11 48", 0x0);

MAKE_HOOK(CBaseAnimating_Interpolate, Signatures::CBaseAnimating_Interpolate.Get(), bool, __fastcall,
	void* ecx, float currentTime)
{
	// Safety check
	if (!ecx || !I::EngineClient->IsInGame())
		return CALL_ORIGINAL(ecx, currentTime);

	const auto pLocal = H::Entities->GetLocal();

	// Skip interpolation for local player during recharge — prevents visual "stretching"
	// when we're not sending commands to the server (same as Amalgam)
	if (pLocal && ecx == pLocal && Shifting::bRecharging)
		return true;

	// Skip interpolation for other players if Disable Interp is on
	if (CFG::Visuals_Disable_Interp && CFG::Misc_Accuracy_Improvements)
	{
		if (pLocal && ecx != pLocal)
		{
			auto pEntity = static_cast<C_BaseEntity*>(ecx);
			if (pEntity)
			{
				auto pNetworkable = pEntity->GetClientNetworkable();
				if (pNetworkable)
				{
					auto pClientClass = pNetworkable->GetClientClass();
					if (pClientClass && pClientClass->m_ClassID == static_cast<int>(ETFClassIds::CTFPlayer))
						return true;
				}
			}
		}
	}

	return CALL_ORIGINAL(ecx, currentTime);
}
