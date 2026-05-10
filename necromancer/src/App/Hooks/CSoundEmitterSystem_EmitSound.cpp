#include "../../SDK/SDK.h"

#include "../Features/CFG.h"

MAKE_SIGNATURE(CSoundEmitterSystem_EmitSound, "client.dll", "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 41 56 48 81 EC ? ? ? ? 49 8B D9", 0x0);

// Sound name lists for blocking (ported from Amalgam)
static const std::vector<const char*> s_vFootsteps = {
	"footstep", "flesh_impact_hard", "body_medium_impact_soft",
	"glass_sheet_step", "rubber_tire_impact_soft", "plastic_box_impact_soft",
	"plastic_barrel_impact_soft", "cardboard_box_impact_soft", "ceiling_tile_step"
};
static const std::vector<const char*> s_vNoisemaker = {
	"items\\halloween", "items\\football_manager", "items\\japan_fundraiser",
	"items\\samurai\\tf_samurai_noisemaker", "items\\summer",
	"misc\\happy_birthday_tf", "misc\\jingle_bells"
};
static const std::vector<const char*> s_vFryingPan = { "pan_" };
static const std::vector<const char*> s_vWater = {
	"ambient_mp3\\water\\water_splash", "slosh", "wade"
};

bool ShouldBlockSound(const char* pSound)
{
	if (!pSound)
		return false;

	bool bBlockFootsteps = CFG::Misc_Sound_Block_Footsteps;
	bool bBlockNoisemaker = CFG::Misc_Sound_Block_Noisemaker;
	bool bBlockFryingPan = CFG::Misc_Sound_Block_FryingPan;
	bool bBlockWater = CFG::Misc_Sound_Block_Water;

	if (!bBlockFootsteps && !bBlockNoisemaker && !bBlockFryingPan && !bBlockWater)
		return false;

	std::string sSound = pSound;
	std::transform(sSound.begin(), sSound.end(), sSound.begin(), ::tolower);

	auto CheckSound = [&](bool bEnabled, const std::vector<const char*>& vSounds) -> bool {
		if (!bEnabled)
			return false;
		for (auto& sNoise : vSounds)
		{
			if (sSound.find(sNoise) != std::string::npos)
				return true;
		}
		return false;
	};

	if (CheckSound(bBlockFootsteps, s_vFootsteps))
		return true;
	if (CheckSound(bBlockNoisemaker, s_vNoisemaker))
		return true;
	if (CheckSound(bBlockFryingPan, s_vFryingPan))
		return true;
	if (CheckSound(bBlockWater, s_vWater))
		return true;

	return false;
}

struct CSoundParameters
{
	CSoundParameters()
	{
		channel = 0;
		volume = 1.0f;
		pitch = 100;
		pitchlow = 100;
		pitchhigh = 100;
		soundlevel = SNDLVL_NORM;
		soundname[0] = 0;
		play_to_owner_only = false;
		count = 0;
		delay_msec = 0;
	}

	int				channel;
	float			volume;
	int				pitch;
	int				pitchlow, pitchhigh;
	soundlevel_t	soundlevel;
	bool			play_to_owner_only;
	int				count;
	char			soundname[128];
	int				delay_msec;
};

struct EmitSound_t
{
	EmitSound_t() :
		m_nChannel(0),
		m_pSoundName(0),
		m_flVolume(1.0f),
		m_SoundLevel(0),
		m_nFlags(0),
		m_nPitch(100),
		m_nSpecialDSP(0),
		m_pOrigin(0),
		m_flSoundTime(0.0f),
		m_pflSoundDuration(0),
		m_bEmitCloseCaption(true),
		m_bWarnOnMissingCloseCaption(false),
		m_bWarnOnDirectWaveReference(false),
		m_nSpeakerEntity(-1),
		m_UtlVecSoundOrigin(),
		m_hSoundScriptHandle(-1)
	{
	}

	int							m_nChannel;
	char const* m_pSoundName;
	float						m_flVolume;
	int				m_SoundLevel;
	int							m_nFlags;
	int							m_nPitch;
	int							m_nSpecialDSP;
	const Vector* m_pOrigin;
	float						m_flSoundTime;
	float* m_pflSoundDuration;
	bool						m_bEmitCloseCaption;
	bool						m_bWarnOnMissingCloseCaption;
	bool						m_bWarnOnDirectWaveReference;
	int							m_nSpeakerEntity;
	mutable CUtlVector< Vector >	m_UtlVecSoundOrigin;
	mutable short		m_hSoundScriptHandle;
};

class IRecipientFilter
{
public:
	virtual			~IRecipientFilter() {}
	virtual bool	IsReliable(void) const = 0;
	virtual bool	IsInitMessage(void) const = 0;
	virtual int		GetRecipientCount(void) const = 0;
	virtual int		GetRecipientIndex(int slot) const = 0;
};

// Manual init
namespace Hooks
{
	namespace CSoundEmitterSystem_EmitSound
	{
		void Init();
		inline CHook Hook(Init);
		using fn = void(__fastcall*)(void*, IRecipientFilter&, int, const EmitSound_t&);
		void __fastcall Func(void* rcx, IRecipientFilter& filter, int entindex, const EmitSound_t& ep);
	}
}

void Hooks::CSoundEmitterSystem_EmitSound::Init()
{
	auto addr = Signatures::CSoundEmitterSystem_EmitSound.Get();
	if (addr)
	{
		Hook.Create(reinterpret_cast<void*>(addr), Func);
		MH_EnableHook(reinterpret_cast<void*>(addr));
	}
}

void __fastcall Hooks::CSoundEmitterSystem_EmitSound::Func(void* rcx, IRecipientFilter& filter, int entindex, const EmitSound_t& ep)
{
	if (ShouldBlockSound(ep.m_pSoundName))
		return;

	return Hook.Original<fn>()(rcx, filter, entindex, ep);
}
