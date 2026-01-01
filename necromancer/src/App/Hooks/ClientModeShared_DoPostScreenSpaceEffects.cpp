#include "../../SDK/SDK.h"
#include "../../SDK/Helpers/Draw/Draw.h"

#include "../Features/CFG.h"
#include "../Features/MiscVisuals/MiscVisuals.h"
#include "../Features/amalgam_port/TrajectoryPreview/TrajectoryPreview.h"

MAKE_HOOK(ClientModeShared_DoPostScreenSpaceEffects, Memory::GetVFunc(I::ClientModeShared, 39), bool, __fastcall,
	CClientModeShared* ecx, const CViewSetup* pSetup)
{
	const auto original = CALL_ORIGINAL(ecx, pSetup);

	F::MiscVisuals->SniperLines();

	// Draw real-time trajectory preview (like Amalgam's ProjectileTrace with bQuick=true)
	if (auto pLocal = H::Entities->GetLocal())
	{
		if (auto pWeapon = H::Entities->GetWeapon())
		{
			F::TrajectoryPreview->ProjectileTrace(pLocal, pWeapon, true);
		}
	}

	// Draw Amalgam-style stored paths (player movement prediction, projectile paths, etc.)
	H::Draw->DrawStoredPaths();

	return original;
}
