#include "../../../SDK/SDK.h"
#include "../CFG.h"
#include <string>

// Process incoming chat message for vote cooldown lifted notifications
void ProcessChatForVoteLifted(const wchar_t* message)
{
	if (!CFG::Misc_Chat_VoteBanOnLifted && !CFG::Misc_Chat_VoteMuteOnLifted)
		return;

	if (!I::EngineClient->IsConnected() || !I::EngineClient->IsInGame())
		return;

	// Convert wchar_t to string for easier searching
	std::wstring wstr(message);

	// Check for "Votekick/Voteban cooldown has been lifted."
	if (CFG::Misc_Chat_VoteBanOnLifted)
	{
		if (wstr.find(L"Votekick/Voteban cooldown has been lifted.") != std::wstring::npos ||
			wstr.find(L"Voteban cooldown has been lifted.") != std::wstring::npos ||
			wstr.find(L"Votekick cooldown has been lifted.") != std::wstring::npos)
		{
			I::EngineClient->ClientCmd_Unrestricted("say \"!voteban\"");
			return;
		}
	}

	// Check for "Votemute cooldown has been lifted."
	if (CFG::Misc_Chat_VoteMuteOnLifted)
	{
		if (wstr.find(L"Votemute cooldown has been lifted.") != std::wstring::npos)
		{
			I::EngineClient->ClientCmd_Unrestricted("say \"!votemute\"");
			return;
		}
	}
}
