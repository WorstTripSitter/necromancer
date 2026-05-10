#pragma once

#include "../Singleton/Singleton.h"
#include <Windows.h>
#include <string>

// Lightweight crash context — updated each frame so we know what was running when it blew up
struct CrashContext_t
{
	const char* m_pszLastFeature = "none";       // Which feature was last running
	const char* m_pszLastHook = "none";          // Which hook was active
	int m_nTargetIndex = -1;                     // Aimbot target
	int m_nCommandNumber = 0;                    // Current cmd number
	bool m_bInGame = false;                      // Engine says we're in game
	bool m_bLevelTransition = false;             // Level transition in progress
	char m_szMapName[64] = {};                   // Current map name
};

class CCrashHandler
{
private:
    static PVOID s_pVectoredHandle;
    static bool s_Initialized;

public:
    void Initialize();
    void Shutdown();

    static CrashContext_t s_Context;  // Updated by hooks/features each frame
};

MAKE_SINGLETON_SCOPED(CCrashHandler, CrashHandler, F);
