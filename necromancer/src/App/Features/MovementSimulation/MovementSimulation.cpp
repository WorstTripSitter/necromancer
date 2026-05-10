#include "MovementSimulation.h"

#include "../CFG.h"
#include "../amalgam_port/AmalgamCompat.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr size_t MaxMoveRecords = 66;
	constexpr size_t MaxSimTimeRecords = 8;
	constexpr float PlayerOriginCompression = 0.125f;
	constexpr float TeleportDistanceSqr = 4096.f * 4096.f;
	constexpr int MaxChokedTicks = 22;

	float g_flOldFrametime = 0.f;
	bool g_bOldInPrediction = false;
	bool g_bOldFirstTimePredicted = false;
	CUserCmd g_MoveSimCmd = {};
	Vec3 g_vOriginalHullMin = {};
	Vec3 g_vOriginalHullMax = {};
	Vec3 g_vOriginalDuckHullMin = {};
	Vec3 g_vOriginalDuckHullMax = {};
	bool g_bBoundsChanged = false;

	float GetFrictionScale(C_TFPlayer* pPlayer)
	{
		static ConVar* sv_friction = U::ConVars.FindVar("sv_friction");
		if (!sv_friction || !pPlayer)
			return 1.f;

		const float flSpeed = pPlayer->m_vecVelocity().Length2D();
		if (flSpeed <= 0.f)
			return 1.f;

		const float flControl = std::max(flSpeed, pPlayer->TeamFortress_CalculateMaxSpeed());
		const float flDrop = flControl * sv_friction->GetFloat() * TICK_INTERVAL;
		return std::clamp((flSpeed - flDrop) / flSpeed, 0.f, 1.f);
	}

	MoveMode GetMoveMode(C_TFPlayer* pPlayer)
	{
		if (pPlayer && pPlayer->m_nWaterLevel() >= 2)
			return MoveMode::Swim;

		return IsOnGround(pPlayer) ? MoveMode::Ground : MoveMode::Air;
	}

	Vec3 DirectionFromVelocity(const Vec3& vVelocity)
	{
		// Ground movement must keep move-unit magnitude; a unit vector makes GameMovement under-drive the target.
		return vVelocity.To2D();
	}

	Vec3 NormalizedDirectionFromVelocity(const Vec3& vVelocity)
	{
		Vec3 vDirection = DirectionFromVelocity(vVelocity);
		if (!vDirection.IsZero())
			vDirection.Normalize2D();

		return vDirection;
	}

	float YawFromVector(const Vec3& vVector)
	{
		return Math::VectorAngles(vVector).y;
	}

	bool IsUsablePlayer(C_TFPlayer* pPlayer)
	{
		return pPlayer && !pPlayer->IsDormant() && pPlayer->IsAlive() && !IsAGhost(pPlayer);
	}

	void HandleMovementRecord(C_TFPlayer* pPlayer, std::deque<MoveRecord>& vRecords)
	{
		if (!pPlayer || vRecords.empty())
			return;

		const MoveMode iMode = vRecords.front().m_iMode;
		if (vRecords.size() > 1)
		{
			const auto& tPrevious = vRecords[1];
			const Vec3 vTraceStart = tPrevious.m_vOrigin;
			const Vec3 vTraceEnd = vTraceStart + tPrevious.m_vVelocity * TICK_INTERVAL;

			CTraceFilterWorldAndPropsOnlyAmalgam filter = {};
			filter.pSkip = pPlayer;

			trace_t trace = {};
			const Vec3 vCompression(PlayerOriginCompression, PlayerOriginCompression, 0.f);
			SDK::TraceHull(vTraceStart, vTraceEnd, pPlayer->m_vecMins() + vCompression, pPlayer->m_vecMaxs() - vCompression, SolidMask(pPlayer), &filter, &trace);

			if (trace.DidHit() && trace.plane.normal.z < 0.7f)
			{
				vRecords.clear();
				return;
			}
		}

		if (pPlayer->InCond(TF_COND_SHIELD_CHARGE))
		{
			vRecords.front().m_vDirection = NormalizedDirectionFromVelocity(pPlayer->m_vecVelocity()) * 450.f;
			return;
		}

		if (iMode == MoveMode::Air)
		{
			vRecords.front().m_vDirection = NormalizedDirectionFromVelocity(pPlayer->m_vecVelocity()) * pPlayer->TeamFortress_CalculateMaxSpeed();
			return;
		}

		if (iMode == MoveMode::Swim)
			vRecords.front().m_vDirection *= 2.f;
	}
}

bool CMovementSimulation::StoreState(MoveStorage& tMoveStorage)
{
	if (!tMoveStorage.m_pPlayer || !I::MemAlloc)
		return false;

	auto pMap = GetPredDescMap(tMoveStorage.m_pPlayer);
	if (!pMap)
		return false;

	const size_t iSize = GetIntermediateDataSize(tMoveStorage.m_pPlayer);
	if (!iSize)
		return false;

	tMoveStorage.m_pData = reinterpret_cast<byte*>(I::MemAlloc->Alloc(iSize));
	if (!tMoveStorage.m_pData)
		return false;

	CPredictionCopy copy(PC_EVERYTHING, tMoveStorage.m_pData, PC_DATA_PACKED, tMoveStorage.m_pPlayer, PC_DATA_NORMAL);
	copy.TransferData("CMovementSimulation::StoreState", tMoveStorage.m_pPlayer->entindex(), pMap);
	return true;
}

void CMovementSimulation::ResetState(MoveStorage& tMoveStorage)
{
	if (!tMoveStorage.m_pData || !I::MemAlloc)
		return;

	if (tMoveStorage.m_pPlayer)
	{
		auto pMap = GetPredDescMap(tMoveStorage.m_pPlayer);
		if (pMap)
		{
			CPredictionCopy copy(PC_EVERYTHING, tMoveStorage.m_pPlayer, PC_DATA_NORMAL, tMoveStorage.m_pData, PC_DATA_PACKED);
			copy.TransferData("CMovementSimulation::ResetState", tMoveStorage.m_pPlayer->entindex(), pMap);
		}
	}

	I::MemAlloc->Free(tMoveStorage.m_pData);
	tMoveStorage.m_pData = nullptr;
}

void CMovementSimulation::Store()
{
	if (!I::ClientEntityList || !I::EngineClient || !I::EngineClient->IsInGame() || !I::EngineClient->IsConnected())
	{
		m_mRecords.clear();
		m_mSimTimes.clear();
		return;
	}

	C_TFPlayer* pLocal = H::Entities ? H::Entities->GetLocal() : nullptr;
	const int nMaxClients = std::min(I::ClientEntityList->GetHighestEntityIndex(), I::GlobalVars ? I::GlobalVars->maxClients : 64);

	for (int n = 1; n <= nMaxClients; n++)
	{
		auto pEntity = I::ClientEntityList->GetClientEntity(n);
		C_TFPlayer* pPlayer = pEntity ? pEntity->As<C_TFPlayer>() : nullptr;
		auto& vRecords = m_mRecords[n];

		if (!IsUsablePlayer(pPlayer) || pPlayer == pLocal || pPlayer->m_vecVelocity().To2D().IsZero())
		{
			vRecords.clear();
			m_mSimTimes[n].clear();
			continue;
		}

		const float flSimTime = pPlayer->m_flSimulationTime();
		if (!vRecords.empty() && std::fabs(vRecords.front().m_flSimTime - flSimTime) <= 0.0001f)
			continue;

		if (!vRecords.empty() && (pPlayer->m_vecOrigin() - vRecords.front().m_vOrigin).LengthSqr() > TeleportDistanceSqr)
		{
			vRecords.clear();
			m_mSimTimes[n].clear();
		}

		if (!vRecords.empty())
		{
			const float flDelta = flSimTime - vRecords.front().m_flSimTime;
			if (flDelta <= 0.f)
			{
				vRecords.clear();
				m_mSimTimes[n].clear();
				continue;
			}

			auto& vSimTimes = m_mSimTimes[n];
			vSimTimes.push_front(flDelta);
			while (vSimTimes.size() > MaxSimTimeRecords)
				vSimTimes.pop_back();
		}

		const Vec3 vVelocity = pPlayer->m_vecVelocity();
		MoveRecord tRecord = {};
		tRecord.m_vDirection = DirectionFromVelocity(vVelocity);
		tRecord.m_flSimTime = flSimTime;
		tRecord.m_iMode = GetMoveMode(pPlayer);
		tRecord.m_vVelocity = vVelocity;
		tRecord.m_vOrigin = pPlayer->m_vecOrigin();

		vRecords.push_front(tRecord);
		while (vRecords.size() > MaxMoveRecords)
			vRecords.pop_back();

		HandleMovementRecord(pPlayer, vRecords);
	}
}

bool CMovementSimulation::Initialize(C_TFPlayer* pPlayer)
{
	if (m_CurrentStorage.m_pData)
		Restore(m_CurrentStorage);

	m_CurrentStorage = {};
	return Initialize(pPlayer, m_CurrentStorage, false, true);
}

bool CMovementSimulation::Initialize(C_TFPlayer* pPlayer, MoveStorage& tMoveStorage, bool bHitchance, bool bStrafe)
{
	tMoveStorage = {};
	tMoveStorage.m_pPlayer = pPlayer;

	if (!IsUsablePlayer(pPlayer) || !I::GlobalVars || !I::Prediction || !I::MoveHelper || !I::GameMovement)
	{
		tMoveStorage.m_bFailed = true;
		tMoveStorage.m_bInitFailed = true;
		return false;
	}

	g_flOldFrametime = I::GlobalVars->frametime;
	g_bOldInPrediction = I::Prediction->m_bInPrediction;
	g_bOldFirstTimePredicted = I::Prediction->m_bFirstTimePredicted;

	tMoveStorage.m_bPredictNetworked = !bHitchance;
	I::GlobalVars->frametime = I::Prediction->m_bEnginePaused ? 0.f : TICK_INTERVAL;
	I::Prediction->m_bInPrediction = true;
	I::Prediction->m_bFirstTimePredicted = false;

	if (!StoreState(tMoveStorage))
	{
		tMoveStorage.m_bFailed = true;
		tMoveStorage.m_bInitFailed = true;
		I::GlobalVars->frametime = g_flOldFrametime;
		I::Prediction->m_bInPrediction = g_bOldInPrediction;
		I::Prediction->m_bFirstTimePredicted = g_bOldFirstTimePredicted;
		return false;
	}

	I::MoveHelper->SetHost(pPlayer);
	g_MoveSimCmd = {};
	pPlayer->SetCurrentCommand(&g_MoveSimCmd);

	if (C_TFPlayer* pLocal = H::Entities ? H::Entities->GetLocal() : nullptr; pPlayer != pLocal)
	{
		if (Vec3* pAverage = g_AmalgamEntitiesExt.GetAvgVelocity(pPlayer->entindex()); pAverage && !pAverage->IsZero())
			pPlayer->m_vecVelocity() = *pAverage;

		if (IsDucking(pPlayer))
		{
			pPlayer->m_fFlags() &= ~FL_DUCKING;
			pPlayer->m_bDucked() = true;
			pPlayer->m_bDucking() = false;
			pPlayer->m_bInDuckJump() = false;
			pPlayer->m_flDucktime() = 0.f;
			pPlayer->m_flDuckJumpTime() = 0.f;
		}

		pPlayer->m_vecBaseVelocity() = {};
		if (IsOnGround(pPlayer))
			pPlayer->m_vecVelocity().z = std::min(pPlayer->m_vecVelocity().z, 0.f);
		else
			pPlayer->m_hGroundEntity() = nullptr;
	}

	tMoveStorage.m_bBunnyHop = IsBunnyHopping(pPlayer);
	SetupMoveData(tMoveStorage, bStrafe);
	if (bStrafe)
		StrafePrediction(tMoveStorage, bHitchance);

	tMoveStorage.m_vPath.push_back(tMoveStorage.m_MoveData.m_vecAbsOrigin);

	const int nChokedTicks = GetChokedTicks(pPlayer);
	for (int i = 0; i < nChokedTicks && !tMoveStorage.m_bFailed; i++)
		RunTick(tMoveStorage, false, nullptr);

	tMoveStorage.m_vPath.clear();
	tMoveStorage.m_vPath.push_back(tMoveStorage.m_MoveData.m_vecAbsOrigin);
	return !tMoveStorage.m_bFailed && !tMoveStorage.m_bInitFailed;
}

void CMovementSimulation::SetupMoveData(MoveStorage& tMoveStorage, bool bStrafe)
{
	C_TFPlayer* pPlayer = tMoveStorage.m_pPlayer;
	C_TFPlayer* pLocal = H::Entities ? H::Entities->GetLocal() : nullptr;
	auto& tMoveData = tMoveStorage.m_MoveData;
	auto& vRecords = m_mRecords[pPlayer->entindex()];

	tMoveData = {};
	tMoveData.m_bFirstRunOfFunctions = false;
	tMoveData.m_bGameCodeMovedPlayer = false;
	tMoveData.m_nPlayerHandle = pPlayer->GetRefEHandle();
	tMoveData.m_vecVelocity = pPlayer->m_vecVelocity();
	tMoveData.m_vecAbsOrigin = pPlayer->m_vecOrigin();
	tMoveData.m_flMaxSpeed = SDK::MaxSpeed(pPlayer);
	tMoveData.m_flClientMaxSpeed = tMoveData.m_flMaxSpeed;
	tMoveData.m_nButtons = 0;
	tMoveData.m_nOldButtons = 0;

	CUserCmd* pCmd = G::CurrentUserCmd ? G::CurrentUserCmd : G::LastUserCmd;
	if (pPlayer == pLocal && pCmd)
	{
		tMoveData.m_vecViewAngles = pCmd->viewangles;
		tMoveData.m_flForwardMove = pCmd->forwardmove;
		tMoveData.m_flSideMove = pCmd->sidemove;
		tMoveData.m_flUpMove = pCmd->upmove;
		tMoveData.m_nButtons = pCmd->buttons;
	}
	else if (!vRecords.empty())
	{
		if (pPlayer->InCond(TF_COND_SHIELD_CHARGE))
			tMoveData.m_vecViewAngles = pPlayer->GetEyeAngles();
		else if (!pPlayer->m_vecVelocity().To2D().IsZero())
			tMoveData.m_vecViewAngles = Math::VectorAngles(pPlayer->m_vecVelocity().To2D());

		g_MoveSimCmd = {};
		g_MoveSimCmd.forwardmove = vRecords.front().m_vDirection.x;
		g_MoveSimCmd.sidemove = -vRecords.front().m_vDirection.y;
		g_MoveSimCmd.upmove = vRecords.front().m_vDirection.z;
		g_MoveSimCmd.viewangles = {};

		SDK::FixMovement(&g_MoveSimCmd, {}, tMoveData.m_vecViewAngles);

		tMoveData.m_flForwardMove = g_MoveSimCmd.forwardmove;
		tMoveData.m_flSideMove = g_MoveSimCmd.sidemove;
		tMoveData.m_flUpMove = g_MoveSimCmd.upmove;
	}
	else if (!pPlayer->m_vecVelocity().To2D().IsZero())
	{
		tMoveData.m_vecViewAngles = Math::VectorAngles(pPlayer->m_vecVelocity().To2D());
		tMoveData.m_flForwardMove = tMoveData.m_flMaxSpeed;
	}

	tMoveData.m_vecAngles = tMoveData.m_vecViewAngles;
	tMoveData.m_outStepHeight = 0.f;
	tMoveData.m_vecConstraintCenter = pPlayer->m_vecConstraintCenter();
	tMoveData.m_flConstraintRadius = pPlayer->m_flConstraintRadius();
	tMoveData.m_flConstraintWidth = pPlayer->m_flConstraintWidth();
	tMoveData.m_flConstraintSpeedFactor = pPlayer->m_flConstraintSpeedFactor();

	tMoveStorage.m_flSimTime = pPlayer->m_flSimulationTime();
	tMoveStorage.m_flPredictedDelta = GetPredictedDelta(pPlayer);
	tMoveStorage.m_flPredictedSimTime = tMoveStorage.m_flSimTime + tMoveStorage.m_flPredictedDelta;
	tMoveStorage.m_vPredictedOrigin = tMoveData.m_vecAbsOrigin;
	tMoveStorage.m_bDirectMove = IsOnGround(pPlayer) || GetMoveMode(pPlayer) == MoveMode::Swim || !bStrafe;
}

float CMovementSimulation::GetAverageYaw(C_TFPlayer* pPlayer, int iSamples)
{
	if (!pPlayer || iSamples <= 0)
		return 0.f;

	auto& vRecords = m_mRecords[pPlayer->entindex()];
	if (vRecords.size() < 3)
		return 0.f;

	const MoveMode iMode = vRecords.front().m_iMode;
	const int iMaxSamples = std::min<int>(iSamples, static_cast<int>(vRecords.size()) - 1);
	float flYawTotal = 0.f;
	int iTickTotal = 0;
	int iRealSamples = 0;
	int iDirectionChanges = 0;
	int iLastSign = 0;

	const float flStraightFuzzy = iMode == MoveMode::Ground ? 25.f : 15.f;
	const int iMaxDirectionChanges = iMode == MoveMode::Ground ? 2 : 3;

	for (int i = 0; i < iMaxSamples; i++)
	{
		const auto& tCurrent = vRecords[i];
		const auto& tPrevious = vRecords[i + 1];
		if (tCurrent.m_iMode != iMode || tPrevious.m_iMode != iMode)
			break;

		if (tCurrent.m_vDirection.IsZero() || tPrevious.m_vDirection.IsZero())
			continue;

		const float flTimeDelta = tCurrent.m_flSimTime - tPrevious.m_flSimTime;
		const int iTicks = std::max(TIME_TO_TICKS(flTimeDelta), 1);

		float flYaw = Math::NormalizeAngle(YawFromVector(tCurrent.m_vDirection) - YawFromVector(tPrevious.m_vDirection));
		if (std::fabs(flYaw) > 45.f)
			break;

		const float flSpeed = std::max(tCurrent.m_vVelocity.Length2D(), 1.f);
		const bool bTooStraight = std::fabs(flYaw) * flSpeed * iTicks < flStraightFuzzy;
		const int iYawSign = sign(flYaw);

		if ((iLastSign && iYawSign && iYawSign != iLastSign) || bTooStraight)
		{
			iDirectionChanges++;
			if (iDirectionChanges > iMaxDirectionChanges)
				break;
		}

		if (iYawSign)
			iLastSign = iYawSign;

		flYawTotal += flYaw;
		iTickTotal += iTicks;
		iRealSamples++;
	}

	const int iMinSamples = iMode == MoveMode::Ground ? 4 : 5;
	if (iRealSamples < iMinSamples || iTickTotal <= 0)
		return 0.f;

	const float flAverage = flYawTotal / static_cast<float>(iTickTotal);
	if (std::fabs(flAverage) < (iMode == MoveMode::Ground ? 0.08f : 0.12f))
		return 0.f;

	return std::clamp(flAverage, -6.f, 6.f);
}

void CMovementSimulation::StrafePrediction(MoveStorage& tMoveStorage, bool bHitchance)
{
	C_TFPlayer* pPlayer = tMoveStorage.m_pPlayer;
	if (!pPlayer)
		return;

	auto& vRecords = m_mRecords[pPlayer->entindex()];
	if (vRecords.empty())
	{
		tMoveStorage.m_flAverageYaw = 0.f;
		return;
	}

	const MoveMode iMode = vRecords.front().m_iMode;
	const int iSamples = iMode == MoveMode::Ground ? 12 : 20;
	tMoveStorage.m_flAverageYaw = GetAverageYaw(pPlayer, iSamples);

	if (!bHitchance)
		return;

	const Vec3 vStart = tMoveStorage.m_MoveData.m_vecAbsOrigin;
	const Vec3 vVelocity = tMoveStorage.m_MoveData.m_vecVelocity;
	CTraceFilterWorldAndPropsOnlyAmalgam filter = {};
	filter.pSkip = pPlayer;

	trace_t trace = {};
	SDK::TraceHull(vStart, vStart + vVelocity * TICK_INTERVAL, pPlayer->m_vecMins(), pPlayer->m_vecMaxs(), SolidMask(pPlayer), &filter, &trace);
	if (trace.DidHit() && trace.plane.normal.z < 0.7f)
		tMoveStorage.m_flAverageYaw = 0.f;
}

bool CMovementSimulation::IsBunnyHopping(C_TFPlayer* pPlayer)
{
	if (!pPlayer)
		return false;

	auto& vRecords = m_mRecords[pPlayer->entindex()];
	if (vRecords.size() < 3)
		return false;

	const float flSpeed = vRecords.front().m_vVelocity.Length2D();
	if (flSpeed < 150.f)
		return false;

	int iTakeoffs = 0;
	int iLandings = 0;
	int iGroundSamples = 0;
	int iAirSamples = 0;
	int iGroundStreak = 0;
	int iMaxGroundStreak = 0;

	const int iMaxRecords = std::min<int>(static_cast<int>(vRecords.size()) - 1, 33);
	for (int i = 0; i < iMaxRecords; i++)
	{
		const bool bNewGround = vRecords[i].m_iMode == MoveMode::Ground;
		const bool bOldGround = vRecords[i + 1].m_iMode == MoveMode::Ground;

		if (bNewGround)
		{
			iGroundSamples++;
			iGroundStreak++;
			iMaxGroundStreak = std::max(iMaxGroundStreak, iGroundStreak);
		}
		else
		{
			iAirSamples++;
			iGroundStreak = 0;
		}

		if (!bNewGround && bOldGround)
			iTakeoffs++;
		else if (bNewGround && !bOldGround)
			iLandings++;
	}

	if (vRecords.front().m_iMode == MoveMode::Ground && vRecords[1].m_iMode == MoveMode::Air)
		return true;

	if (iTakeoffs >= 2)
		return true;

	if (iTakeoffs >= 1 && iLandings >= 1 && iAirSamples > iGroundSamples && iMaxGroundStreak <= 2)
		return true;

	return false;
}

int CMovementSimulation::GetChokedTicks(C_TFPlayer* pPlayer) const
{
	if (!pPlayer)
		return 0;

	const int nIndex = pPlayer->entindex();
	auto it = m_mSimTimes.find(nIndex);
	if (it != m_mSimTimes.end() && !it->second.empty())
		return std::clamp(TIME_TO_TICKS(it->second.front()) - 1, 0, MaxChokedTicks);

	auto itRecords = m_mRecords.find(nIndex);
	if (itRecords == m_mRecords.end() || itRecords->second.size() < 2)
		return 0;

	const float flDelta = itRecords->second[0].m_flSimTime - itRecords->second[1].m_flSimTime;
	return std::clamp(TIME_TO_TICKS(flDelta) - 1, 0, MaxChokedTicks);
}

void CMovementSimulation::RunTick()
{
	RunTick(m_CurrentStorage, true, nullptr);
}

void CMovementSimulation::RunTick(MoveStorage& tMoveStorage, bool bPath, const RunTickCallback* pCallback)
{
	C_TFPlayer* pPlayer = tMoveStorage.m_pPlayer;
	if (!pPlayer || tMoveStorage.m_bFailed || tMoveStorage.m_bInitFailed || !I::GameMovement || !I::GlobalVars || !I::Prediction)
	{
		tMoveStorage.m_bFailed = true;
		return;
	}

	I::GlobalVars->frametime = I::Prediction->m_bEnginePaused ? 0.f : TICK_INTERVAL;
	I::Prediction->m_bInPrediction = true;
	I::Prediction->m_bFirstTimePredicted = false;

	SetBounds(pPlayer);

	float flCorrection = 0.f;
	const float flOldClientMaxSpeed = tMoveStorage.m_MoveData.m_flClientMaxSpeed;

	if (tMoveStorage.m_flAverageYaw)
	{
		const bool bAir = !IsOnGround(pPlayer) && GetMoveMode(pPlayer) != MoveMode::Swim;
		flCorrection = bAir && !pPlayer->InCond(TF_COND_SHIELD_CHARGE) ? 90.f * sign(tMoveStorage.m_flAverageYaw) : 0.f;
		const float flFriction = bAir ? 1.f : GetFrictionScale(pPlayer);
		tMoveStorage.m_MoveData.m_vecViewAngles.y += tMoveStorage.m_flAverageYaw * flFriction + flCorrection;
	}
	else if (!IsOnGround(pPlayer) && GetMoveMode(pPlayer) != MoveMode::Swim)
	{
		tMoveStorage.m_MoveData.m_flForwardMove = 0.f;
		tMoveStorage.m_MoveData.m_flSideMove = 0.f;
	}

	if (IsDucking(pPlayer) && IsOnGround(pPlayer) && GetMoveMode(pPlayer) != MoveMode::Swim)
		tMoveStorage.m_MoveData.m_flClientMaxSpeed /= 3.f;

	if (tMoveStorage.m_bBunnyHop && IsOnGround(pPlayer) && !pPlayer->m_bDucked())
	{
		tMoveStorage.m_MoveData.m_nOldButtons &= ~IN_JUMP;
		tMoveStorage.m_MoveData.m_nButtons |= IN_JUMP;
	}

	I::GameMovement->ProcessMovement(pPlayer, &tMoveStorage.m_MoveData);

	if (pCallback)
		(*pCallback)(tMoveStorage.m_MoveData);

	tMoveStorage.m_flSimTime += TICK_INTERVAL;
	tMoveStorage.m_bPredictNetworked = tMoveStorage.m_flSimTime >= tMoveStorage.m_flPredictedSimTime;
	if (tMoveStorage.m_bPredictNetworked)
	{
		tMoveStorage.m_vPredictedOrigin = tMoveStorage.m_MoveData.m_vecAbsOrigin;
		tMoveStorage.m_flPredictedSimTime += tMoveStorage.m_flPredictedDelta;
	}

	const bool bLastDirectMove = tMoveStorage.m_bDirectMove;
	tMoveStorage.m_bDirectMove = IsOnGround(pPlayer) || GetMoveMode(pPlayer) == MoveMode::Swim;
	tMoveStorage.m_MoveData.m_flClientMaxSpeed = flOldClientMaxSpeed;

	if (tMoveStorage.m_flAverageYaw)
		tMoveStorage.m_MoveData.m_vecViewAngles.y -= flCorrection;
	else if (tMoveStorage.m_bDirectMove && !bLastDirectMove
		&& !tMoveStorage.m_MoveData.m_flForwardMove && !tMoveStorage.m_MoveData.m_flSideMove
		&& tMoveStorage.m_MoveData.m_vecVelocity.Length2D() > tMoveStorage.m_MoveData.m_flMaxSpeed * 0.015f)
	{
		Vec3 vDirection = tMoveStorage.m_MoveData.m_vecVelocity.Normalized2D() * 450.f;
		g_MoveSimCmd = {};
		g_MoveSimCmd.forwardmove = vDirection.x;
		g_MoveSimCmd.sidemove = -vDirection.y;
		SDK::FixMovement(&g_MoveSimCmd, {}, tMoveStorage.m_MoveData.m_vecViewAngles);
		tMoveStorage.m_MoveData.m_flForwardMove = g_MoveSimCmd.forwardmove;
		tMoveStorage.m_MoveData.m_flSideMove = g_MoveSimCmd.sidemove;
	}

	RestoreBounds(pPlayer);

	if (bPath)
		tMoveStorage.m_vPath.push_back(tMoveStorage.m_MoveData.m_vecAbsOrigin);
}

void CMovementSimulation::Restore()
{
	Restore(m_CurrentStorage);
	m_CurrentStorage = {};
}

void CMovementSimulation::Restore(MoveStorage& tMoveStorage)
{
	if (tMoveStorage.m_pPlayer)
	{
		RestoreBounds(tMoveStorage.m_pPlayer);
		tMoveStorage.m_pPlayer->SetCurrentCommand(nullptr);
	}

	if (I::MoveHelper)
		I::MoveHelper->SetHost(nullptr);

	ResetState(tMoveStorage);

	if (I::Prediction)
	{
		I::Prediction->m_bInPrediction = g_bOldInPrediction;
		I::Prediction->m_bFirstTimePredicted = g_bOldFirstTimePredicted;
	}

	if (I::GlobalVars)
		I::GlobalVars->frametime = g_flOldFrametime;
}

void CMovementSimulation::SetBounds(C_TFPlayer* pPlayer)
{
	if (!pPlayer || g_bBoundsChanged || !I::TFGameRules())
		return;

	CTFGameRules* pRules = I::TFGameRules();
	if (!pRules || !pRules->GetViewVectors())
		return;

	auto pViewVectors = pRules->GetViewVectors();
	g_vOriginalHullMin = pViewVectors->m_vHullMin;
	g_vOriginalHullMax = pViewVectors->m_vHullMax;
	g_vOriginalDuckHullMin = pViewVectors->m_vDuckHullMin;
	g_vOriginalDuckHullMax = pViewVectors->m_vDuckHullMax;

	const Vec3 vCompression(PlayerOriginCompression, PlayerOriginCompression, 0.f);
	pViewVectors->m_vHullMin = pPlayer->m_vecMins() + vCompression;
	pViewVectors->m_vHullMax = pPlayer->m_vecMaxs() - vCompression;
	pViewVectors->m_vDuckHullMin = pPlayer->m_vecMins() + vCompression;
	pViewVectors->m_vDuckHullMax = pPlayer->m_vecMaxs() - vCompression;
	g_bBoundsChanged = true;
}

void CMovementSimulation::RestoreBounds(C_TFPlayer* pPlayer)
{
	if (!pPlayer || !g_bBoundsChanged || !I::TFGameRules())
		return;

	CTFGameRules* pRules = I::TFGameRules();
	if (!pRules || !pRules->GetViewVectors())
		return;

	auto pViewVectors = pRules->GetViewVectors();
	pViewVectors->m_vHullMin = g_vOriginalHullMin;
	pViewVectors->m_vHullMax = g_vOriginalHullMax;
	pViewVectors->m_vDuckHullMin = g_vOriginalDuckHullMin;
	pViewVectors->m_vDuckHullMax = g_vOriginalDuckHullMax;
	g_bBoundsChanged = false;
}

float CMovementSimulation::GetPredictedDelta(C_TFPlayer* pPlayer)
{
	if (!pPlayer)
		return TICK_INTERVAL;

	auto& vSimTimes = m_mSimTimes[pPlayer->entindex()];
	if (vSimTimes.empty())
		return TICK_INTERVAL;

	float flTotal = 0.f;
	for (const float flDelta : vSimTimes)
		flTotal += flDelta;

	return std::clamp(flTotal / static_cast<float>(vSimTimes.size()), TICK_INTERVAL, TICKS_TO_TIME(MaxChokedTicks + 1));
}

void CMovementSimulation::ClearRecords()
{
	if (m_CurrentStorage.m_pData)
		Restore(m_CurrentStorage);

	m_CurrentStorage = {};
	m_mRecords.clear();
	m_mSimTimes.clear();
}
