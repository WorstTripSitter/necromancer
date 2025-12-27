#pragma once
#include "../../../SDK/SDK.h"

class CFakeLag
{
private:
	bool IsAllowed(C_TFPlayer* pLocal, C_TFWeaponBase* pWeapon, CUserCmd* pCmd);
	bool IsSniperThreat(C_TFPlayer* pLocal, int& outMinTicks, int& outMaxTicks);
	void PreserveBlastJump(C_TFPlayer* pLocal);
	void Unduck(C_TFPlayer* pLocal, CUserCmd* pCmd);
	void Prediction(C_TFPlayer* pLocal, CUserCmd* pCmd);

	Vec3 m_vLastPosition = {};
	int m_iNewEntityUnchokeTicks = 0;
	int m_iCurrentChokeTicks = 0;
	int m_iTargetChokeTicks = 0;
	
	// Amalgam-style flags
	bool m_bPreservingBlast = false;
	bool m_bUnducking = false;

public:
	void Run(C_TFPlayer* pLocal, C_TFWeaponBase* pWeapon, CUserCmd* pCmd, bool* pSendPacket);
	void UpdateDrawChams(); // Call after Run() to update fake model visibility

	int m_iGoal = 0;
	bool m_bEnabled = false;
};

MAKE_SINGLETON_SCOPED(CFakeLag, FakeLag, F);
