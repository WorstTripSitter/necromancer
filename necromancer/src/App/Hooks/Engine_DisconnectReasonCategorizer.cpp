#include "../../SDK/SDK.h"
#include "../Features/CFG.h"
#include <string>
#include <algorithm>
#include <cctype>

// DisconnectReasonCategorizer - engine.dll function that receives the disconnect reason string
// from the server and categorizes it into a numeric code.
// Parameters: (__int64 a1, const char* a2) — a1 is CSteam3Client context, a2 is the reason string
// Fires when: server sends a kick/disconnect packet (SourceBans bans, admin kicks, VAC kicks)
MAKE_SIGNATURE(DisconnectReasonCategorizer, "engine.dll", "40 53 48 83 EC ? 83 B9 ? ? ? ? ? 48 8B DA 0F 8E", 0x0);

MAKE_HOOK(DisconnectReasonCategorizer, Signatures::DisconnectReasonCategorizer.Get(), void, __fastcall,
	__int64 a1, const char* a2)
{
	// Check if rejoin on kick is enabled and we have a reason string
	if (CFG::Misc_Auto_Rejoin_On_Kick && a2 && a2[0] != '\0')
	{
		std::string sReason(a2);
		std::string sReasonLower = sReason;
		std::transform(sReasonLower.begin(), sReasonLower.end(), sReasonLower.begin(), ::tolower);

		// Detect kick/ban reason strings from the server
		// Common reasons: "Kicked by Console", "Kicked by admin", "You have been banned",
		// "VAC banned from secure server", "Steam auth ticket has been canceled", etc.
		bool bKicked = sReasonLower.find("kicked") != std::string::npos
			|| sReasonLower.find("banned") != std::string::npos
			|| sReasonLower.find("vote off") != std::string::npos
			|| sReasonLower.find("voted off") != std::string::npos
			|| sReasonLower.find("steam") != std::string::npos;

		if (bKicked)
		{
			G::bRejoinOnKickPending = true;
		}
	}

	CALL_ORIGINAL(a1, a2);
}
