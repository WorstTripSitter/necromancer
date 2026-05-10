#include "../../SDK/SDK.h"

#include "../Features/CFG.h"

MAKE_SIGNATURE(S_StartSound, "engine.dll", "40 53 48 83 EC ? 48 83 79 ? ? 48 8B D9 75 ? 33 C0", 0x0);

// Type definitions for engine sound system (ported from Amalgam)
class CAudioSource;
class CSfxTable
{
public:
	virtual const char	*getname();
	const char			*GetFileName();
	void*				GetFileNameHandle();
	void				SetNamePoolIndex(int index);
	bool				IsPrecachedSound();
	void				OnNameChanged(const char *pName);
	int					m_namePoolIndex;
	CAudioSource		*pSource;
	bool				m_bUseErrorFilename : 1;
	bool				m_bIsUISound : 1;
	bool				m_bIsLateLoad : 1;
	bool				m_bMixGroupsCached : 1;
	byte				m_mixGroupCount;
	byte				m_mixGroupList[8];
};

struct StartSoundParams_t
{
	bool			staticsound;
	int				userdata;
	int				soundsource;
	int				entchannel;
	CSfxTable*		pSfx;
	Vec3			origin;
	Vec3			direction;
	bool			bUpdatePositions;
	float			fvol;
	soundlevel_t	soundlevel;
	int				flags;
	int				pitch;
	int				specialdsp;
	bool			fromserver;
	float			delay;
	int				speakerentity;
	bool			suppressrecording;
	int				initialStreamPosition;
};

// ShouldBlockSound is defined in CSoundEmitterSystem_EmitSound.cpp
extern bool ShouldBlockSound(const char* pSound);

// Manual init
namespace Hooks
{
	namespace S_StartSound
	{
		void Init();
		inline CHook Hook(Init);
		using fn = int(__fastcall*)(StartSoundParams_t&);
		int __fastcall Func(StartSoundParams_t& params);
	}
}

void Hooks::S_StartSound::Init()
{
	auto addr = Signatures::S_StartSound.Get();
	if (addr)
	{
		Hook.Create(reinterpret_cast<void*>(addr), Func);
		MH_EnableHook(reinterpret_cast<void*>(addr));
	}
}

int __fastcall Hooks::S_StartSound::Func(StartSoundParams_t& params)
{
	if (params.pSfx && ShouldBlockSound(params.pSfx->getname()))
		return 0;

	return Hook.Original<fn>()(params);
}
