#include "../../SDK/SDK.h"

#include "../Features/CFG.h"
#include "../Features/SeedPred/SeedPred.h"
#include "../Features/ChatESP/ChatESP.h"

MAKE_SIGNATURE(CUserMessages_DispatchUserMessage, "client.dll", "40 56 48 83 EC ? 49 8B F0", 0x0);

// User message types
enum EUserMessageType
{
	UM_SayText2 = 4,
	UM_TextMsg = 5,
	UM_Shake = 10,
	UM_Fade = 11,
	UM_VGUIMenu = 12
};

MAKE_HOOK(CUserMessages_DispatchUserMessage, Signatures::CUserMessages_DispatchUserMessage.Get(), bool, __fastcall,
	void* ecx, int msg_type, bf_read& msg_data)
{
	if (msg_type == UM_Shake && CFG::Visuals_Remove_Screen_Shake)
	{
		return true;
	}

	if (msg_type == UM_Fade && CFG::Visuals_Remove_Screen_Fade)
	{
		return true;
	}

	// MOTD removal - VGUIMenu message with "info" panel name
	if (msg_type == UM_VGUIMenu && CFG::Visuals_Remove_MOTD)
	{
		// Read the panel name from the message data
		auto bufData = reinterpret_cast<const char*>(msg_data.m_pData);
		if (bufData && strcmp(bufData, "info") == 0)
		{
			I::EngineClient->ClientCmd_Unrestricted("closedwelcomemenu");
			return true;
		}
	}

	if (msg_type == UM_TextMsg && F::SeedPred->ParsePlayerPerf(msg_data))
	{
		return true;
	}

	// Chat ESP - capture SayText2 messages
	if (msg_type == UM_SayText2 && CFG::Visuals_ChatESP_Active)
	{
		int nStartBit = msg_data.GetNumBitsRead();

		int iPlayerIndex = msg_data.ReadByte();
		msg_data.ReadByte(); // bWantsToChat

		// Skip the localized message format string
		char szFormat[256];
		msg_data.ReadString(szFormat, sizeof(szFormat));

		// Read player name (skip it)
		char szPlayerName[256];
		msg_data.ReadString(szPlayerName, sizeof(szPlayerName));

		// Read the actual chat message
		char szMessage[256];
		msg_data.ReadString(szMessage, sizeof(szMessage));

		// Pass to ChatESP
		if (iPlayerIndex > 0 && szMessage[0] != '\0')
		{
			F::ChatESP->OnChatMessage(iPlayerIndex, szMessage);
		}

		// Reset read position so the original handler can process it
		msg_data.Seek(nStartBit);
	}

	return CALL_ORIGINAL(ecx, msg_type, msg_data);
}
