#pragma once

#include "../../AmalgamCompat.h"
#include <functional>
#include <deque>

// ============================================
// Move Storage Structure
// ============================================

struct MoveStorage
{
    C_TFPlayer* m_pPlayer = nullptr;
    CMoveData m_MoveData = {};
    byte* m_pData = nullptr;

    float m_flAverageYaw = 0.f;
    bool m_bBunnyHop = false;

    float m_flSimTime = 0.f;
    float m_flPredictedDelta = 0.f;
    float m_flPredictedSimTime = 0.f;
    bool m_bDirectMove = true;

    bool m_bPredictNetworked = true;
    Vec3 m_vPredictedOrigin = {};

    std::vector<Vec3> m_vPath = {};

    bool m_bFailed = false;
    bool m_bInitFailed = false;
};

// ============================================
// Move Data Record Structure
// ============================================

struct MoveData_Record
{
    Vec3 m_vDirection = {};
    float m_flSimTime = 0.f;
    int m_iMode = 0;
    Vec3 m_vVelocity = {};
    Vec3 m_vOrigin = {};
};

// ============================================
// Movement Simulation Class
// ============================================

class CAmalgamMovementSimulation
{
private:
    void Store(MoveStorage& tStorage);
    void Reset(MoveStorage& tStorage);

    bool SetupMoveData(MoveStorage& tStorage);
    void GetAverageYaw(MoveStorage& tStorage, int iSamples);
    bool StrafePrediction(MoveStorage& tStorage, int iSamples);

    void SetBounds(C_TFPlayer* pPlayer);
    void RestoreBounds(C_TFPlayer* pPlayer);

    bool m_bOldInPrediction = false;
    bool m_bOldFirstTimePredicted = false;
    float m_flOldFrametime = 0.f;

    std::unordered_map<int, std::deque<MoveData_Record>> m_mRecords = {};
    std::unordered_map<int, std::deque<float>> m_mSimTimes = {};

public:
    void Store();
    void Clear(); // Clear all tracking data (call on map change/disconnect)

    bool Initialize(C_BaseEntity* pEntity, MoveStorage& tStorage, bool bHitchance = true, bool bStrafe = true);
    bool SetDuck(MoveStorage& tStorage, bool bDuck);
    void RunTick(MoveStorage& tStorage, bool bPath = true, std::function<void(CMoveData&)>* pCallback = nullptr);
    void RunTick(MoveStorage& tStorage, bool bPath, std::function<void(CMoveData&)> fCallback);
    void Restore(MoveStorage& tStorage);

    float GetPredictedDelta(C_BaseEntity* pEntity);
};

MAKE_SINGLETON_SCOPED(CAmalgamMovementSimulation, AmalgamMoveSim, F);

// Alias for compatibility with Amalgam code
namespace F {
    inline CAmalgamMovementSimulation& MoveSim = *F::AmalgamMoveSim;
}
