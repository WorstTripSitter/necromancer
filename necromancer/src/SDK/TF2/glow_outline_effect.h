#pragma once

#include "c_baseentity.h"
#include "utlvector.h"

// TF2's native glow system structures
// Based on Source SDK 2013 glow_outline_effect.h

class CGlowObjectManager
{
public:
	struct GlowObjectDefinition_t
	{
		bool ShouldDraw(int nSlot) const
		{
			return m_hEntity.Get() &&
				(m_bRenderWhenOccluded || m_bRenderWhenUnoccluded) &&
				m_hEntity->ShouldDraw() &&
				!m_hEntity->IsDormant();
		}

		bool IsUnused() const { return m_nNextFreeSlot != ENTRY_IN_USE; }

		EHANDLE m_hEntity;
		Vec3 m_vGlowColor;
		float m_flGlowAlpha;

		bool m_bRenderWhenOccluded;
		bool m_bRenderWhenUnoccluded;
		int m_nSplitScreenSlot;

		// Linked list of free slots
		int m_nNextFreeSlot;

		// Special values for m_nNextFreeSlot
		static const int END_OF_FREE_LIST = -1;
		static const int ENTRY_IN_USE = -2;
	};

	CUtlVector<GlowObjectDefinition_t> m_GlowObjectDefinitions;
	int m_nFirstFreeSlot;

	// Helper methods
	int RegisterGlowObject(C_BaseEntity* pEntity, const Vec3& vGlowColor, float flGlowAlpha, bool bRenderWhenOccluded, bool bRenderWhenUnoccluded, int nSplitScreenSlot = -1)
	{
		int nIndex;
		if (m_nFirstFreeSlot == GlowObjectDefinition_t::END_OF_FREE_LIST)
		{
			nIndex = m_GlowObjectDefinitions.AddToTail();
		}
		else
		{
			nIndex = m_nFirstFreeSlot;
			m_nFirstFreeSlot = m_GlowObjectDefinitions[nIndex].m_nNextFreeSlot;
		}

		m_GlowObjectDefinitions[nIndex].m_hEntity = pEntity;
		m_GlowObjectDefinitions[nIndex].m_vGlowColor = vGlowColor;
		m_GlowObjectDefinitions[nIndex].m_flGlowAlpha = flGlowAlpha;
		m_GlowObjectDefinitions[nIndex].m_bRenderWhenOccluded = bRenderWhenOccluded;
		m_GlowObjectDefinitions[nIndex].m_bRenderWhenUnoccluded = bRenderWhenUnoccluded;
		m_GlowObjectDefinitions[nIndex].m_nSplitScreenSlot = nSplitScreenSlot;
		m_GlowObjectDefinitions[nIndex].m_nNextFreeSlot = GlowObjectDefinition_t::ENTRY_IN_USE;

		return nIndex;
	}

	void UnregisterGlowObject(int nGlowObjectHandle)
	{
		if (nGlowObjectHandle < 0 || nGlowObjectHandle >= m_GlowObjectDefinitions.Count())
			return;

		m_GlowObjectDefinitions[nGlowObjectHandle].m_nNextFreeSlot = m_nFirstFreeSlot;
		m_GlowObjectDefinitions[nGlowObjectHandle].m_hEntity = nullptr;
		m_nFirstFreeSlot = nGlowObjectHandle;
	}

	void SetEntity(int nGlowObjectHandle, C_BaseEntity* pEntity)
	{
		if (nGlowObjectHandle < 0 || nGlowObjectHandle >= m_GlowObjectDefinitions.Count())
			return;
		m_GlowObjectDefinitions[nGlowObjectHandle].m_hEntity = pEntity;
	}

	void SetColor(int nGlowObjectHandle, const Vec3& vGlowColor)
	{
		if (nGlowObjectHandle < 0 || nGlowObjectHandle >= m_GlowObjectDefinitions.Count())
			return;
		m_GlowObjectDefinitions[nGlowObjectHandle].m_vGlowColor = vGlowColor;
	}

	void SetAlpha(int nGlowObjectHandle, float flAlpha)
	{
		if (nGlowObjectHandle < 0 || nGlowObjectHandle >= m_GlowObjectDefinitions.Count())
			return;
		m_GlowObjectDefinitions[nGlowObjectHandle].m_flGlowAlpha = flAlpha;
	}

	void SetRenderFlags(int nGlowObjectHandle, bool bRenderWhenOccluded, bool bRenderWhenUnoccluded)
	{
		if (nGlowObjectHandle < 0 || nGlowObjectHandle >= m_GlowObjectDefinitions.Count())
			return;
		m_GlowObjectDefinitions[nGlowObjectHandle].m_bRenderWhenOccluded = bRenderWhenOccluded;
		m_GlowObjectDefinitions[nGlowObjectHandle].m_bRenderWhenUnoccluded = bRenderWhenUnoccluded;
	}

	// Note: RenderGlowEffects is called via signature, not through this class
};
