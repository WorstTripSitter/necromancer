#pragma once

#include "../../../SDK/SDK.h"
#include <map>

// RAII wrapper for glow objects - automatically unregisters on destruction
class CGlowObjectWrapper
{
private:
	int m_nHandle = -1;

public:
	CGlowObjectWrapper() = default;
	
	CGlowObjectWrapper(C_BaseEntity* pEntity, const Vec3& vColor, float flAlpha, bool bOccluded, bool bUnoccluded)
	{
		auto pGlowManager = SDKUtils::GetGlowObjectManager();
		if (pGlowManager)
		{
			m_nHandle = pGlowManager->RegisterGlowObject(pEntity, vColor, flAlpha, bOccluded, bUnoccluded);
		}
	}

	~CGlowObjectWrapper()
	{
		if (m_nHandle >= 0)
		{
			auto pGlowManager = SDKUtils::GetGlowObjectManager();
			if (pGlowManager && m_nHandle < pGlowManager->m_GlowObjectDefinitions.Count())
			{
				pGlowManager->UnregisterGlowObject(m_nHandle);
			}
		}
	}

	// Delete copy constructor and assignment
	CGlowObjectWrapper(const CGlowObjectWrapper&) = delete;
	CGlowObjectWrapper& operator=(const CGlowObjectWrapper&) = delete;

	// Allow move
	CGlowObjectWrapper(CGlowObjectWrapper&& other) noexcept
	{
		m_nHandle = other.m_nHandle;
		other.m_nHandle = -1;
	}

	CGlowObjectWrapper& operator=(CGlowObjectWrapper&& other) noexcept
	{
		if (this != &other)
		{
			// Clean up our current handle
			if (m_nHandle >= 0)
			{
				auto pGlowManager = SDKUtils::GetGlowObjectManager();
				if (pGlowManager && m_nHandle < pGlowManager->m_GlowObjectDefinitions.Count())
				{
					pGlowManager->UnregisterGlowObject(m_nHandle);
				}
			}
			
			m_nHandle = other.m_nHandle;
			other.m_nHandle = -1;
		}
		return *this;
	}

	void SetColor(const Vec3& vColor)
	{
		if (m_nHandle >= 0)
		{
			auto pGlowManager = SDKUtils::GetGlowObjectManager();
			if (pGlowManager)
				pGlowManager->SetColor(m_nHandle, vColor);
		}
	}

	void SetAlpha(float flAlpha)
	{
		if (m_nHandle >= 0)
		{
			auto pGlowManager = SDKUtils::GetGlowObjectManager();
			if (pGlowManager)
				pGlowManager->SetAlpha(m_nHandle, flAlpha);
		}
	}

	bool IsValid() const { return m_nHandle >= 0; }
};

class CTF2Glow
{
private:
	std::map<C_BaseEntity*, CGlowObjectWrapper> m_mapGlowObjects = {};

public:
	void Run();
	void Render(const CViewSetup* pViewSetup);
	void CleanUp();
};

MAKE_SINGLETON_SCOPED(CTF2Glow, TF2Glow, F);
