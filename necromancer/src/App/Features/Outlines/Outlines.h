#pragma once

#include "../../../SDK/SDK.h"

class COutlines
{
	IMaterial *m_pMatGlowColor = nullptr, *m_pMatHaloAddToScreen = nullptr;
	ITexture *m_pRtFullFrame = nullptr, *m_pRenderBuffer0 = nullptr, *m_pRenderBuffer1 = nullptr;
	IMaterial *m_pMatBlurX = nullptr, *m_pMatBlurY = nullptr;
	IMaterialVar* m_pBloomAmount = nullptr;

	void Initialize();

	std::unordered_map<C_BaseEntity*, bool> m_mapDrawnEntities = {};
	bool m_bRendering = false;
	bool m_bRenderingOutlines = false;
	bool m_bCleaningUp = false;

	void DrawEntity(C_BaseEntity* pEntity, bool bModel);

	struct OutlineEntity_t
	{
		C_BaseEntity* m_pEntity = nullptr;
		Color_t m_Color = {};
		float m_flAlpha = 0.0f;
	};

	// Lag record outline entry — uses DrawModelExecute directly with stored bones
	struct OutlineLagRecord_t
	{
		C_TFPlayer* m_pPlayer = nullptr;
		matrix3x4_t* m_pBoneMatrix = nullptr;	// pointer to record's BoneMatrix (valid during same frame)
		Color_t m_Color = {};
		float m_flAlpha = 0.0f;
	};

	std::vector<OutlineEntity_t> m_vecOutlineEntities = {};
	std::vector<OutlineLagRecord_t> m_vecOutlineLagRecords = {};

	// Per-player captured render state for lag record outline rendering
	// Different player classes have different studio headers, so we need per-player state
	std::unordered_map<C_TFPlayer*, DrawModelState_t> m_mapCapturedStates = {};
	std::unordered_map<C_TFPlayer*, ModelRenderInfo_t> m_mapCapturedInfos = {};

public:
	void RunModels();
	void Run();
	void CleanUp();
	void SetModelStencil(IMatRenderContext* pRenderContext);

	// Called by the DrawModelExecute hook to provide the original function pointer
	void SetDrawModelExecuteFn(IVModelRender::DrawModelExecuteFn fn) { m_fnDrawModelExecute = fn; }

	// Original DrawModelExecute function pointer — set by the hook, used by lag record outline rendering
	IVModelRender::DrawModelExecuteFn m_fnDrawModelExecute = nullptr;

	// Check if lag record outlines should be rendered for this player
	bool ShouldRenderLagRecords(C_TFPlayer* pPlayer);

	// Capture render state from the real model draw for later lag record outline rendering
	void CaptureRenderState(const DrawModelState_t& state, ModelRenderInfo_t& pInfo);

	// Render lag record outlines by calling DrawModelExecute directly with each record's BoneMatrix
	void RenderLagRecords(C_TFPlayer* pPlayer);

	bool HasDrawn(C_BaseEntity* pEntity)
	{
		return m_mapDrawnEntities.contains(pEntity);
	}

	bool IsRendering()
	{
		return m_bRendering;
	}

	bool IsRenderingOutlines()
	{
		return m_bRenderingOutlines;
	}

	bool IsUsedMaterial(const IMaterial* pMaterial)
	{
		return pMaterial == m_pMatGlowColor
			|| pMaterial == m_pMatBlurX
			|| pMaterial == m_pMatBlurY
			|| pMaterial == m_pMatHaloAddToScreen;
	}

	bool IsCleaningUp() { return m_bCleaningUp; }
};

MAKE_SINGLETON_SCOPED(COutlines, Outlines, F);
