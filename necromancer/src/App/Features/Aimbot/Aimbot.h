#pragma once

#include "../../../SDK/SDK.h"

class CAimbot
{
	void RunMain(CUserCmd* pCmd);

	// Cached from RunMain to avoid re-fetching in Run()
	C_TFPlayer* m_pCachedLocal = nullptr;
	C_TFWeaponBase* m_pCachedWeapon = nullptr;

public:
	void Run(CUserCmd* pCmd);
};

MAKE_SINGLETON_SCOPED(CAimbot, Aimbot, F);
