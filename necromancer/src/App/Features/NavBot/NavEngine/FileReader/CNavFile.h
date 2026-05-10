#pragma once
#include "nav.h"
#include <fstream>
#include <string>
#include <algorithm>

// CNavFile - Parses TF2 .nav files and builds the nav mesh
class CNavFile
{
public:
	CNavFile() {}

	// Load nav file from level name
	// Intended to use with engine->GetLevelName() or mapname from server_spawn GameEvent
	explicit CNavFile(const char* szLevelname)
	{
		if (!szLevelname)
			return;

		m_sMapName.append(szLevelname);
		std::ifstream file(m_sMapName, std::ios::binary);
		if (!file.is_open())
		{
			// .nav file does not exist
			return;
		}

		uint32_t uMagic;
		file.read((char*)&uMagic, sizeof(uint32_t));
		if (uMagic != 0xFEEDFACEu)
		{
			// Wrong magic number
			return;
		}

		uint32_t uVersion;
		file.read((char*)&uVersion, sizeof(uint32_t));
		if (uVersion < 16) // 16 is latest for TF2
		{
			// Version is too old
			return;
		}

		uint32_t uSubVersion;
		file.read((char*)&uSubVersion, sizeof(uint32_t));
		if (uSubVersion != 2) // 2 for TF2
		{
			// Not TF2 nav file
			return;
		}

		// We do not really need to check the size
		file.read((char*)&m_uBspSize, sizeof(uint32_t));
		file.read((char*)&m_bAnalyzed, sizeof(unsigned char));

		// TF2 does not use places, but in case they exist
		unsigned short uPlacesCount;
		file.read((char*)&uPlacesCount, sizeof(uint16_t));
		for (int i = 0; i < uPlacesCount; ++i)
		{
			NavPlace_t tPlace;
			file.read((char*)&tPlace.m_uLen, sizeof(uint16_t));
			file.read((char*)&tPlace.m_sName, tPlace.m_uLen);

			m_vPlaces.push_back(tPlace);
		}

		file.read((char*)&m_bHasUnnamedAreas, sizeof(unsigned char));

		unsigned int uAreaCount;
		file.read((char*)&uAreaCount, sizeof(uint32_t));
		for (size_t i = 0; i < uAreaCount; ++i)
		{
			CNavArea tArea;
			file.read((char*)&tArea.m_uId, sizeof(uint32_t));
			file.read((char*)&tArea.m_iAttributeFlags, sizeof(uint32_t));
			file.read((char*)&tArea.m_vNwCorner, sizeof(Vec3));
			file.read((char*)&tArea.m_vSeCorner, sizeof(Vec3));
			file.read((char*)&tArea.m_flNeZ, sizeof(float));
			file.read((char*)&tArea.m_flSwZ, sizeof(float));

			// Calculate center point
			tArea.m_vCenter.x = (tArea.m_vNwCorner.x + tArea.m_vSeCorner.x) / 2.0f;
			tArea.m_vCenter.y = (tArea.m_vNwCorner.y + tArea.m_vSeCorner.y) / 2.0f;
			tArea.m_vCenter.z = (tArea.m_vNwCorner.z + tArea.m_vSeCorner.z) / 2.0f;

			// Calculate inverse corner deltas for interpolation
			if ((tArea.m_vSeCorner.x - tArea.m_vNwCorner.x) > 0.0f &&
				(tArea.m_vSeCorner.y - tArea.m_vNwCorner.y) > 0.0f)
			{
				tArea.m_flInvDxCorners = 1.0f / (tArea.m_vSeCorner.x - tArea.m_vNwCorner.x);
				tArea.m_flInvDyCorners = 1.0f / (tArea.m_vSeCorner.y - tArea.m_vNwCorner.y);
			}
			else
				tArea.m_flInvDxCorners = tArea.m_flInvDyCorners = 0.0f;

			// Calculate min/max Z with tolerance (18 units is player height tolerance)
			tArea.m_flMinZ = (std::min)(tArea.m_vSeCorner.z, tArea.m_vNwCorner.z) - 18.f;
			tArea.m_flMaxZ = (std::max)(tArea.m_vSeCorner.z, tArea.m_vNwCorner.z) + 18.f;

			// Read connections in 4 directions (NORTH, EAST, SOUTH, WEST)
			for (int iDir = 0; iDir < 4; iDir++)
			{
				file.read((char*)&tArea.m_uConnectionCount, sizeof(uint32_t));
				for (size_t j = 0; j < tArea.m_uConnectionCount; j++)
				{
					NavConnect_t tConnect;
					file.read((char*)&tConnect.m_uId, sizeof(uint32_t));

					// Skip self-connections
					if (tConnect.m_uId == tArea.m_uId)
					{
						tArea.m_uConnectionCount--;
						continue;
					}

					// Store connection (both directional and non-directional)
					tArea.m_vConnections.push_back(tConnect);
					tArea.m_vConnectionsDir[iDir].push_back(tConnect);
				}
			}

			// Read hiding spots
			file.read((char*)&tArea.m_uHidingSpotCount, sizeof(uint8_t));
			for (size_t j = 0; j < tArea.m_uHidingSpotCount; j++)
			{
				CHidingSpot tSpot;
				file.read((char*)&tSpot.m_uId, sizeof(uint32_t));
				file.read((char*)&tSpot.m_vPos, sizeof(Vec3));
				file.read((char*)&tSpot.m_fFlags, sizeof(unsigned char));

				tArea.m_vHidingSpots.push_back(tSpot);
			}

			// Read encounter spots
			file.read((char*)&tArea.m_uEncounterSpotCount, sizeof(uint32_t));
			for (size_t j = 0; j < tArea.m_uEncounterSpotCount; j++)
			{
				SpotEncounter_t tSpot;
				file.read((char*)&tSpot.m_tFrom.m_uId, sizeof(uint32_t));
				file.read((char*)&tSpot.m_iFromDir, sizeof(unsigned char));
				file.read((char*)&tSpot.m_tTo.m_uId, sizeof(uint32_t));
				file.read((char*)&tSpot.m_iToDir, sizeof(unsigned char));
				file.read((char*)&tSpot.m_uSpotCount, sizeof(unsigned char));

				for (int s = 0; s < tSpot.m_uSpotCount; ++s)
				{
					SpotOrder_t tOrder;
					file.read((char*)&tOrder.m_uId, sizeof(uint32_t));
					file.read((char*)&tOrder.flT, sizeof(unsigned char));
					tSpot.m_vSpots.push_back(tOrder);
				}

				tArea.m_vSpotEncounters.push_back(tSpot);
			}

			file.read((char*)&tArea.m_uIndexType, sizeof(uint16_t));

			// Read ladder connections (TF2 doesn't really use these)
			for (int iDir = 0; iDir < 2; iDir++)
			{
				file.read((char*)&tArea.m_uLadderCount, sizeof(uint32_t));
				for (size_t j = 0; j < tArea.m_uLadderCount; j++)
				{
					int iTemp;
					file.read((char*)&iTemp, sizeof(uint32_t));
					tArea.m_vLadders[iDir].push_back(iTemp);
				}
			}

			// Read earliest occupy times
			for (float& j : tArea.m_flEarliestOccupyTime)
				file.read((char*)&j, sizeof(float));

			// Read light intensity at corners
			for (float& j : tArea.m_flLightIntensity)
				file.read((char*)&j, sizeof(float));

			// Read potentially visible areas
			file.read((char*)&tArea.m_uVisibleAreaCount, sizeof(uint32_t));
			for (size_t j = 0; j < tArea.m_uVisibleAreaCount; ++j)
			{
				AreaBindInfo_t tInfo;
				file.read((char*)&tInfo.m_uId, sizeof(uint32_t));
				file.read((char*)&tInfo.m_uAttributes, sizeof(unsigned char));

				tArea.m_vPotentiallyVisibleAreas.push_back(tInfo);
			}

			file.read((char*)&tArea.m_uInheritVisibilityFrom, sizeof(uint32_t));

			// TF2 specific area flags
			file.read((char*)&tArea.m_iTFAttributeFlags, sizeof(uint32_t));

			m_vAreas.push_back(tArea);
		}

		file.close();

		// Fill connection pointers for every area
		// This converts area IDs to actual pointers for faster pathfinding
		for (auto& tArea : m_vAreas)
		{
			// Fill connection area pointers
			for (auto& connection : tArea.m_vConnections)
				for (auto& connected_area : m_vAreas)
					if (connection.m_uId == connected_area.m_uId)
						connection.m_pArea = &connected_area;

			// Fill directional connection area pointers
			// These are needed by ComputeAdjacentConnectionHeightChange to determine
			// which edge two areas share, so it can compute accurate edge heights
			for (int iDir = 0; iDir < 4; iDir++)
				for (auto& connection : tArea.m_vConnectionsDir[iDir])
					for (auto& connected_area : m_vAreas)
						if (connection.m_uId == connected_area.m_uId)
							connection.m_pArea = &connected_area;

			// Fill potentially visible area pointers
			for (auto& bindinfo : tArea.m_vPotentiallyVisibleAreas)
				for (auto& boundarea : m_vAreas)
					if (bindinfo.m_uId == boundarea.m_uId)
						bindinfo.m_pArea = &boundarea;
		}

		m_bOK = true;
	}

	// Public members
	std::vector<NavPlace_t> m_vPlaces;
	std::vector<CNavArea> m_vAreas;
	std::string m_sMapName;

	unsigned int m_uBspSize;
	bool m_bHasUnnamedAreas{};
	bool m_bAnalyzed{};
	bool m_bOK = false;
};
