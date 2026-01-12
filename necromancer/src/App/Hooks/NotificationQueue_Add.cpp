#include "../../SDK/SDK.h"
#include "../../SDK/TF2/CEconNotification.h"
#include "../Features/CFG.h"

MAKE_SIGNATURE(NotificationQueue_Add, "client.dll", "48 89 5C 24 ? 57 48 83 EC ? 48 8B F9 48 8B 0D ? ? ? ? 48 8B 01 FF 90 ? ? ? ? 84 C0 75", 0x0);

namespace FNV1A_Items
{
	constexpr uint32_t Hash32Const(const char* str, uint32_t hash = 2166136261u)
	{
		return *str ? Hash32Const(str + 1, (hash ^ static_cast<uint32_t>(*str)) * 16777619u) : hash;
	}
	
	inline uint32_t Hash32(const char* str)
	{
		uint32_t hash = 2166136261u;
		while (*str)
		{
			hash ^= static_cast<uint32_t>(*str++);
			hash *= 16777619u;
		}
		return hash;
	}
}

MAKE_HOOK(NotificationQueue_Add, Signatures::NotificationQueue_Add.Get(), int, __fastcall,
	CEconNotification* pNotification)
{
	if (CFG::Misc_Auto_Accept_Items && pNotification && pNotification->m_pText)
	{
		if (FNV1A_Items::Hash32(pNotification->m_pText) == FNV1A_Items::Hash32Const("TF_HasNewItems"))
		{
			pNotification->Accept();
			pNotification->Trigger();
			pNotification->UpdateTick();
			pNotification->MarkForDeletion();
			return 0;
		}
	}

	return CALL_ORIGINAL(pNotification);
}
