#pragma once
#include "../../../../../Utils/Math/Math.h"
#include "../../../../../SDK/TF2/tf_shareddefs.h"
#include <vector>
#include <cstdint>
#include <cassert>

// Undef Windows min/max macros that conflict with std::min/std::max
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// Nav mesh attribute flags
enum NavAttributeType : uint32_t
{
    NAV_MESH_INVALID = 0,
    NAV_MESH_CROUCH  = 0x0000001,      // must crouch to use this node/area
    NAV_MESH_JUMP    = 0x0000002,      // must jump to traverse this area (only used during generation)
    NAV_MESH_PRECISE   = 0x0000004,    // do not adjust for obstacles, just move along area
    NAV_MESH_NO_JUMP   = 0x0000008,    // inhibit discontinuity jumping
    NAV_MESH_STOP      = 0x0000010,    // must stop when entering this area
    NAV_MESH_RUN       = 0x0000020,    // must run to traverse this area
    NAV_MESH_WALK      = 0x0000040,    // must walk to traverse this area
    NAV_MESH_AVOID     = 0x0000080,    // avoid this area unless alternatives are too dangerous
    NAV_MESH_TRANSIENT = 0x0000100,    // area may become blocked, and should be periodically checked
    NAV_MESH_DONT_HIDE   = 0x0000200,  // area should not be considered for hiding spot generation
    NAV_MESH_STAND       = 0x0000400,  // bots hiding in this area should stand
    NAV_MESH_NO_HOSTAGES = 0x0000800,  // hostages shouldn't use this area
    NAV_MESH_STAIRS      = 0x0001000,  // this area represents stairs, do not attempt to climb or jump them - just walk up
    NAV_MESH_NO_MERGE     = 0x0002000, // don't merge this area with adjacent areas
    NAV_MESH_OBSTACLE_TOP = 0x0004000, // this nav area is the climb point on the tip of an obstacle
    NAV_MESH_CLIFF        = 0x0008000, // this nav area is adjacent to a drop of at least CliffHeight

    NAV_MESH_FIRST_CUSTOM = 0x00010000, // apps may define custom app-specific bits starting with this value
    NAV_MESH_LAST_CUSTOM = 0x04000000,  // apps must not define custom app-specific bits higher than with this value
};

// TF2-specific nav attributes
enum TFNavAttributeType : uint32_t
{
    TF_NAV_INVALID = 0x00000000,

    TF_NAV_BLOCKED         = 0x00000001, // blocked for some TF-specific reason
    TF_NAV_SPAWN_ROOM_RED  = 0x00000002,
    TF_NAV_SPAWN_ROOM_BLUE = 0x00000004,
    TF_NAV_SPAWN_ROOM_EXIT = 0x00000008,
    TF_NAV_HAS_AMMO        = 0x00000010,
    TF_NAV_HAS_HEALTH      = 0x00000020,
    TF_NAV_CONTROL_POINT   = 0x00000040,

    TF_NAV_BLUE_SENTRY_DANGER = 0x00000080, // sentry can potentially fire upon enemies in this area
    TF_NAV_RED_SENTRY_DANGER  = 0x00000100,

    TF_NAV_BLUE_SETUP_GATE             = 0x00000800, // this area is blocked until the setup period is over
    TF_NAV_RED_SETUP_GATE              = 0x00001000, // this area is blocked until the setup period is over
    TF_NAV_BLOCKED_AFTER_POINT_CAPTURE = 0x00002000, // this area becomes blocked after the first point is capped
    TF_NAV_BLOCKED_UNTIL_POINT_CAPTURE = 0x00004000, // this area is blocked until the first point is capped, then is unblocked
    TF_NAV_BLUE_ONE_WAY_DOOR           = 0x00008000,
    TF_NAV_RED_ONE_WAY_DOOR            = 0x00010000,

    TF_NAV_WITH_SECOND_POINT = 0x00020000, // modifier for BLOCKED_*_POINT_CAPTURE
    TF_NAV_WITH_THIRD_POINT  = 0x00040000, // modifier for BLOCKED_*_POINT_CAPTURE
    TF_NAV_WITH_FOURTH_POINT = 0x00080000, // modifier for BLOCKED_*_POINT_CAPTURE
    TF_NAV_WITH_FIFTH_POINT  = 0x00100000, // modifier for BLOCKED_*_POINT_CAPTURE

    TF_NAV_SNIPER_SPOT = 0x00200000, // this is a good place for a sniper to lurk
    TF_NAV_SENTRY_SPOT = 0x00400000, // this is a good place to build a sentry

    TF_NAV_ESCAPE_ROUTE         = 0x00800000, // for Raid mode
    TF_NAV_ESCAPE_ROUTE_VISIBLE = 0x01000000, // all areas that have visibility to the escape route

    TF_NAV_NO_SPAWNING = 0x02000000, // don't spawn bots in this area

    TF_NAV_RESCUE_CLOSET = 0x04000000, // for respawning friends in Raid mode

    TF_NAV_BOMB_CAN_DROP_HERE = 0x08000000, // the bomb can be dropped here and reached by the invaders in MvM

    TF_NAV_DOOR_NEVER_BLOCKS  = 0x10000000,
    TF_NAV_DOOR_ALWAYS_BLOCKS = 0x20000000,
};

class CNavArea;

// Nav place name structure
struct NavPlace_t
{
public:
	char m_sName[256];
	unsigned short m_uLen;
};

// A HidingSpot is a good place for a bot to crouch and wait for enemies
class CHidingSpot
{
public:
	enum
	{
		IN_COVER          = 0x01, // in a corner with good hard cover nearby
		GOOD_SNIPER_SPOT  = 0x02, // had at least one decent sniping corridor
		IDEAL_SNIPER_SPOT = 0x04, // can see either very far, or a large area, or both
		EXPOSED           = 0x08  // spot in the open, usually on a ledge or cliff
	};

	bool HasGoodCover() const
	{
		return (m_fFlags & IN_COVER) != 0;
	}
	bool IsGoodSniperSpot() const
	{
		return (m_fFlags & GOOD_SNIPER_SPOT) != 0;
	}
	bool IsIdealSniperSpot() const
	{
		return (m_fFlags & IDEAL_SNIPER_SPOT) != 0;
	}
	bool IsExposed() const
	{
		return (m_fFlags & EXPOSED) != 0;
	}

	Vec3 m_vPos;            // world coordinates of the spot
	unsigned int m_uId;     // this spot's unique ID
	unsigned char m_fFlags; // bit flags
};

// Area visibility binding info
struct AreaBindInfo_t
{
	unsigned int m_uId = 0;
	CNavArea* m_pArea = nullptr;

	unsigned char m_uAttributes{}; // VisibilityType
};

// Nav area connection
struct NavConnect_t
{
	unsigned int m_uId = 0;
	float m_flLength = -1;
	CNavArea* m_pArea = nullptr;

	bool operator==(const NavConnect_t& tOther) const { return m_pArea == tOther.m_pArea; }
};

// Spot order for encounter paths
struct SpotOrder_t
{
	CHidingSpot* spot;  // the spot to look at
	float flT;          // parametric distance along ray where this spot first has LOS to our path
	unsigned int m_uId; // spot ID for save/load
};

// This struct stores possible path segments thru a CNavArea, and the dangerous
// spots to look at as we traverse that path segment.
struct SpotEncounter_t
{
	NavConnect_t m_tFrom;
	NavConnect_t m_tTo;
	int m_iFromDir;
	int m_iToDir;

	unsigned char m_uSpotCount;
	std::vector<SpotOrder_t> m_vSpots; // list of spots to look at, in order of occurrence
};

// Main nav area class - represents a walkable area in the nav mesh
class CNavArea
{
public:
	uint32_t m_uId;
	int32_t m_iAttributeFlags;
	int32_t m_iTFAttributeFlags;
	Vec3 m_vNwCorner;
	Vec3 m_vSeCorner;
	Vec3 m_vCenter;
	float m_flInvDxCorners;
	float m_flInvDyCorners;
	float m_flNeZ;
	float m_flSwZ;
	float m_flMinZ;
	float m_flMaxZ;
	std::vector<NavConnect_t> m_vConnections;
	std::vector<NavConnect_t> m_vConnectionsDir[4];
	std::vector<AreaBindInfo_t> m_vPotentiallyVisibleAreas;
	std::vector<SpotEncounter_t> m_vSpotEncounters;
	std::vector<CHidingSpot> m_vHidingSpots;
	std::vector<uint32_t> m_vLadders[2];

	uint32_t m_uConnectionCount;
	uint32_t m_uVisibleAreaCount;
	uint32_t m_uInheritVisibilityFrom;
	uint32_t m_uEncounterSpotCount;
	unsigned char m_uHidingSpotCount;
	uint32_t m_uLadderCount;

	uint16_t m_uIndexType;

	float m_flEarliestOccupyTime[2]; // MAX_NAV_TEAMS
	float m_flLightIntensity[4];     // NUM_CORNERS

	// Check if area is blocked for a given team
	bool IsBlocked(int iTeam) const
	{
		if (m_iTFAttributeFlags & TF_NAV_BLOCKED)
			return true;

		const bool bRedSpawn = m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_RED;
		const bool bBlueSpawn = m_iTFAttributeFlags & TF_NAV_SPAWN_ROOM_BLUE;

		if (iTeam == TF_TEAM_RED && bBlueSpawn && !bRedSpawn)
			return true;
		if (iTeam == TF_TEAM_BLUE && bRedSpawn && !bBlueSpawn)
			return true;

		return false;
	}

	// Check if the given point is overlapping the area (2D check)
	// @return True if 'pos' is within 2D extents of area.
	bool IsOverlapping(const Vec3& vPos, float flTolerance = 0.0f) const
	{
		if (vPos.x + flTolerance < this->m_vNwCorner.x)
			return false;

		if (vPos.x - flTolerance > this->m_vSeCorner.x)
			return false;

		if (vPos.y + flTolerance < this->m_vNwCorner.y)
			return false;

		if (vPos.y - flTolerance > this->m_vSeCorner.y)
			return false;

		return true;
	}

	// Check if the point is within the 3D bounds of this area
	bool Contains(Vec3& vPoint) const
	{
		if (!IsOverlapping(vPoint))
			return false;

		if (vPoint.z > m_flMaxZ)
			return false;

		if (vPoint.z < m_flMinZ)
			return false;

		return true;
	}

	// Get southwest corner
	inline Vec3 GetSwCorner() const
	{
		return Vec3(m_vNwCorner.x, m_vSeCorner.y, m_flSwZ);
	}

	// Get northeast corner
	inline Vec3 GetNeCorner() const
	{
		return Vec3(m_vSeCorner.x, m_vNwCorner.y, m_flNeZ);
	}

	// Float selection helper (branchless)
	static float FloatSel(float flComparand, float flValGreaterEqual, float flLessThan)
	{
		return flComparand >= 0 ? flValGreaterEqual : flLessThan;
	}

	// Get Z coordinate at given X,Y position (interpolated from corners)
	float GetZ(float x, float y) const
	{
		if (m_flInvDxCorners == 0.0f || m_flInvDyCorners == 0.0f)
			return m_flNeZ;

		float u = (x - m_vNwCorner.x) * m_flInvDxCorners;
		float v = (y - m_vNwCorner.y) * m_flInvDyCorners;

		u = FloatSel(u, u, 0);           // u >= 0 ? u : 0
		u = FloatSel(u - 1.0f, 1.0f, u); // u >= 1 ? 1 : u

		v = FloatSel(v, v, 0);           // v >= 0 ? v : 0
		v = FloatSel(v - 1.0f, 1.0f, v); // v >= 1 ? 1 : v

		float northZ = m_vNwCorner.z + u * (m_flNeZ - m_vNwCorner.z);
		float southZ = m_flSwZ + u * (m_vSeCorner.z - m_flSwZ);

		return northZ + v * (southZ - northZ);
	}

	// Get nearest point on area to given 2D point
	Vec3 GetNearestPoint(const Vec2 vPoint) const
	{
		float x, y, z;

		assert(vPoint.x >= 0 && vPoint.y >= 0);
		assert(m_vNwCorner.x >= 0 && m_vNwCorner.y >= 0);
		assert(m_vSeCorner.x >= 0 && m_vSeCorner.y >= 0);

		x = FloatSel(vPoint.x - m_vNwCorner.x, vPoint.x, m_vNwCorner.x);
		x = FloatSel(x - m_vSeCorner.x, m_vSeCorner.x, x);

		y = FloatSel(vPoint.y - m_vNwCorner.y, vPoint.y, m_vNwCorner.y);
		y = FloatSel(y - m_vSeCorner.y, m_vSeCorner.y, y);

		z = GetZ(x, y);

		return Vec3(x, y, z);
	}

	// Compute height change at the shared edge between this area and an adjacent area.
	// Ported from TF2's CNavArea::ComputeAdjacentConnectionHeightChange.
	// This returns the height difference at the boundary (where you actually step),
	// NOT the center-to-center difference. For stairs, the edge height is much smaller
	// than the center height, which is why center-to-center checks incorrectly block stairs.
	float ComputeAdjacentConnectionHeightChange(const CNavArea* pDestination) const
	{
		if (!pDestination)
			return 0.0f;

		// Find which direction the destination is connected on
		// NORTH=0, EAST=1, SOUTH=2, WEST=3
		int dir = -1;
		for (int d = 0; d < 4; ++d)
		{
			for (const auto& connect : m_vConnectionsDir[d])
			{
				if (connect.m_pArea == pDestination)
				{
					dir = d;
					break;
				}
			}
			if (dir >= 0) break;
		}

		if (dir < 0)
		{
			// Not directly connected — fall back to center-to-center
			return pDestination->m_vCenter.z - m_vCenter.z;
		}

		// Compute portal center on our edge (where we step FROM)
		float myEdgeX, myEdgeY;
		ComputePortalCenter(pDestination, dir, &myEdgeX, &myEdgeY);
		float myEdgeZ = GetZ(myEdgeX, myEdgeY);

		// Compute portal center on destination's edge (where we step TO)
		// Opposite direction: NORTH<->SOUTH, EAST<->WEST
		int oppDir = (dir + 2) % 4;
		float destEdgeX, destEdgeY;
		pDestination->ComputePortalCenter(this, oppDir, &destEdgeX, &destEdgeY);
		float destEdgeZ = pDestination->GetZ(destEdgeX, destEdgeY);

		return destEdgeZ - myEdgeZ;
	}

	// Compute the center of the portal (shared edge) between this area and another area
	// for a given direction. Returns the X,Y of the portal center.
	// Ported from TF2's CNavArea::ComputePortal.
	void ComputePortalCenter(const CNavArea* pOther, int dir, float* outX, float* outY) const
	{
		if (dir == 0 || dir == 2) // NORTH or SOUTH
		{
			*outY = (dir == 0) ? m_vNwCorner.y : m_vSeCorner.y;

			float left = (std::max)(m_vNwCorner.x, pOther->m_vNwCorner.x);
			float right = (std::min)(m_vSeCorner.x, pOther->m_vSeCorner.x);

			left = (std::clamp)(left, m_vNwCorner.x, m_vSeCorner.x);
			right = (std::clamp)(right, m_vNwCorner.x, m_vSeCorner.x);

			*outX = (left + right) * 0.5f;
		}
		else // EAST or WEST
		{
			*outX = (dir == 3) ? m_vNwCorner.x : m_vSeCorner.x;  // WEST=3, EAST=1

			float top = (std::max)(m_vNwCorner.y, pOther->m_vNwCorner.y);
			float bottom = (std::min)(m_vSeCorner.y, pOther->m_vSeCorner.y);

			top = (std::clamp)(top, m_vNwCorner.y, m_vSeCorner.y);
			bottom = (std::clamp)(bottom, m_vNwCorner.y, m_vSeCorner.y);

			*outY = (top + bottom) * 0.5f;
		}
	}
};
