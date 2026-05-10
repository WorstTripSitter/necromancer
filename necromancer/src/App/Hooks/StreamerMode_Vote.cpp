#include "../../SDK/SDK.h"
#include "../../SDK/TF2/bitbuf.h"

#include "../Features/StreamerMode/StreamerMode.h"

// Exact signatures from Amalgam
MAKE_SIGNATURE(bf_read_ReadString, "client.dll", "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 40 32 FF", 0x0);
MAKE_SIGNATURE(CHudVote_MsgFunc_VoteStart_ReadString_Call, "client.dll", "8B 4B ? 8B C1 44 8B 5B ? 41 2B C3 83 F8 ? 7D ? 89 4B ? 48 8B CB E8 ? ? ? ? 41 8B C6", 0x0);

// Exact copy from Amalgam - bf_read::ReadString
MAKE_HOOK(bf_read_ReadString, Signatures::bf_read_ReadString.Get(), bool, __fastcall,
	void* rcx, char* pStr, int maxLen, bool bLine, int* pOutNumChars)
{
	const auto dwRetAddr = uintptr_t(_ReturnAddress());
	const auto dwDesired = Signatures::CHudVote_MsgFunc_VoteStart_ReadString_Call.Get();

	bool bReturn = CALL_ORIGINAL(rcx, pStr, maxLen, bLine, pOutNumChars);

	if (dwRetAddr == dwDesired)
	{
		auto pMsg = reinterpret_cast<bf_read*>(rcx);
		const int iOriginalBit = pMsg->m_iCurBit;
		const int iTarget = pMsg->ReadByte() >> 1;
		pMsg->Seek(iOriginalBit);

		if (!iTarget)
			return bReturn;

		// Amalgam: int iType; const char* sName = F::PlayerUtils.GetPlayerName(iTarget, nullptr, &iType);
		// Amalgam: if (iType == NameTypeEnum::None) return bReturn;
		if (const char* sName = StreamerMode::GetPlayerName(iTarget))
		{
			int iChar = 0;
			while (1)
			{
				char val = sName[iChar];
				if (val == 0)
					break;
				else if (bLine && val == '\n')
					break;

				if (iChar < (maxLen - 1))
				{
					pStr[iChar] = val;
					++iChar;
				}
			}
			pStr[iChar] = 0;
			if (pOutNumChars)
				*pOutNumChars = iChar;
		}
	}

	return bReturn;
}
