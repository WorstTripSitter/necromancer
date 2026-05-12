#pragma once
#include "../../../SDK/SDK.h"

#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

enum class MoveMode
{
	Ground,
	Air,
	Swim
};

using RunTickCallback = std::function<void(CMoveData&)>;

struct MoveRecord
{
	Vec3 m_vDirection = {};
	float m_flSimTime = 0.f;
	MoveMode m_iMode = MoveMode::Ground;
	Vec3 m_vVelocity = {};
	Vec3 m_vOrigin = {};
	bool m_bHitWall = false;
};

struct MoveStorage
{
	C_TFPlayer* m_pPlayer = nullptr;
	CMoveData m_MoveData = {};
	byte* m_pData = nullptr;

	float m_flAverageYaw = 0.f;
	bool m_bBunnyHop = false;
	float m_flBhopCadence = 0.f;        // Predicted time between jumps (seconds)
	int m_iBhopSimTicksInAir = 0;       // Ticks airborne in the sim (for jump timing)

	float m_flSimTime = 0.f;
	float m_flPredictedDelta = 0.f;
	float m_flPredictedSimTime = 0.f;
	bool m_bDirectMove = false;
	bool m_bPredictNetworked = false;

	Vec3 m_vPredictedOrigin = {};
	std::vector<Vec3> m_vPath = {};

	bool m_bFailed = false;
	bool m_bInitFailed = false;
	bool m_bStationary = false;
};

class CMovementSimulation
{
public:
	void Store();

	bool Initialize(C_TFPlayer* pPlayer);
	bool Initialize(C_TFPlayer* pPlayer, MoveStorage& tMoveStorage, bool bHitchance = false, bool bStrafe = true);

	void RunTick();
	void RunTick(MoveStorage& tMoveStorage, bool bPath = true, const RunTickCallback* pCallback = nullptr);

	void Restore();
	void Restore(MoveStorage& tMoveStorage);

	inline const Vec3& GetOrigin() const { return m_CurrentStorage.m_MoveData.m_vecAbsOrigin; }
	inline const Vec3& GetVelocity() const { return m_CurrentStorage.m_MoveData.m_vecVelocity; }
	inline bool HasFailed() const { return m_CurrentStorage.m_bFailed || m_CurrentStorage.m_bInitFailed; }
	inline bool IsStationary() const { return m_CurrentStorage.m_bStationary; }

	void ClearRecords();
	float GetPredictedDelta(C_TFPlayer* pPlayer);

private:
	bool StoreState(MoveStorage& tMoveStorage);
	void ResetState(MoveStorage& tMoveStorage);

	void SetupMoveData(MoveStorage& tMoveStorage, bool bStrafe);
	float GetAverageYaw(C_TFPlayer* pPlayer, int iSamples);
	void StrafePrediction(MoveStorage& tMoveStorage, bool bHitchance);

	bool IsBunnyHopping(C_TFPlayer* pPlayer, float* pCadenceOut = nullptr);
	int GetChokedTicks(C_TFPlayer* pPlayer) const;

	void SetBounds(C_TFPlayer* pPlayer);
	void RestoreBounds(C_TFPlayer* pPlayer);

private:
	MoveStorage m_CurrentStorage = {};
	std::unordered_map<int, std::deque<MoveRecord>> m_mRecords = {};
	std::unordered_map<int, std::deque<float>> m_mSimTimes = {};
};

MAKE_SINGLETON_SCOPED(CMovementSimulation, MovementSimulation, F);
