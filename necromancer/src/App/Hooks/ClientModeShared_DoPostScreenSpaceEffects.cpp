#include "../../SDK/SDK.h"
#include "../../SDK/Helpers/Draw/Draw.h"

#include "../Features/CFG.h"
#include "../Features/MiscVisuals/MiscVisuals.h"
#include "../Features/TF2Glow/TF2Glow.h"

MAKE_HOOK(ClientModeShared_DoPostScreenSpaceEffects, Memory::GetVFunc(I::ClientModeShared, 39), bool, __fastcall,
	CClientModeShared* ecx, const CViewSetup* pSetup)
{
	// TF2 native glow rendering - must be called BEFORE the original to render properly
	if (CFG::Outlines_Active && CFG::Outlines_Style == 4 && !(CFG::Misc_Clean_Screenshot && I::EngineClient->IsTakingScreenshot()))
	{
		F::TF2Glow->Render(pSetup);
	}

	const auto original = CALL_ORIGINAL(ecx, pSetup);

	F::MiscVisuals->SniperLines();

	// Draw real-time trajectory preview (like Amalgam's ProjectileTrace with bQuick=true)
	if (auto pLocal = H::Entities->GetLocal())
	{
		if (auto pWeapon = H::Entities->GetWeapon())
		{

		}
	}

	// Draw Amalgam-style stored paths (player movement prediction, projectile paths, etc.)
	H::Draw->DrawStoredPaths();

	return original;
}
