#include "LagRecords.h"

#include "../CFG.h"
#include "../Misc/AntiCheatCompat/AntiCheatCompat.h"

bool CLagRecords::IsSimulationTimeValid(float flCurSimTime, float flCmprSimTime)
{
	// Base limit (ping-based) + fake latency extension
	// NOTE: Don't clamp fake latency here - the fake latency system works by manipulating
	// sequence numbers, which is separate from the interp system. Amalgam doesn't clamp
	// fake latency, only fake interp (which is sent to the server via cl_interp).
	const float flMaxTime = std::min(GetServerAcceptedAgeRange() + m_flFakeLatency, m_flMaxUnlag);
	return (flCurSimTime - flCmprSimTime) < flMaxTime;
}

float CLagRecords::GetServerAcceptedAge() const
{
	// Cache per frame — this is called per-record in every aimbot, avoid repeated NetChannel queries
	const int nFrame = I::GlobalVars ? I::GlobalVars->framecount : 0;
	if (m_nCacheFrame == nFrame && m_flCachedServerAcceptedAge >= 0.0f)
		return m_flCachedServerAcceptedAge;

	// Server's StartLagCompensation (Source SDK 2013):
	//   correct = nci->GetLatency(FLOW_OUTGOING) + lerpTime
	//   targettick = cmd->tick_count - TIME_TO_TICKS(lerpTime)
	//   deltaTime = correct - TICKS_TO_TIME(gpGlobals->tickcount - targettick)
	//   if |deltaTime| > 0.2s: force targettick
	//
	// Server's nci->GetLatency(FLOW_OUTGOING) = latency from client TO server
	//   = our outgoing latency (NOT our incoming — FLOW_OUTGOING on the server
	//   measures how long it takes for data to flow FROM the client TO the server)
	//
	// With fake latency, we rewind m_nInSequenceNr, making the server measure
	// our outgoing latency as (real_out + fake_latency).
	//
	// We send: tick_count = TIME_TO_TICKS(simTime + lerp)
	// Server subtracts lerp: targettick ≈ TIME_TO_TICKS(simTime)
	//
	// Server computes actual = serverCurTime - simTime
	// From client: serverCurTime ≈ curtime + real_outgoing
	// So actual = (curtime - simTime) + real_outgoing = clientAge + real_out
	//
	// correct = (real_out + fake_latency) + lerp
	//
	// Validation: |correct - actual| = |(real_out + fake_latency + lerp) - (clientAge + real_out)|
	//           = |fake_latency + lerp - clientAge|
	// Therefore: ideal clientAge = fake_latency + lerp
	//
	// Without fake latency: ideal clientAge = lerp (records ~15ms old = current model)
	// With fake latency: ideal clientAge = fake_latency + lerp (backtrack records)
	const float flLerp = SDKUtils::GetLerp();
	const float flFakeLatency = m_flFakeLatency;
	m_flCachedServerAcceptedAge = flFakeLatency + flLerp;
	m_nCacheFrame = nFrame;
	return m_flCachedServerAcceptedAge;
}

float CLagRecords::GetOutgoingLatency() const
{
	if (const auto pNetChan = I::EngineClient->GetNetChannelInfo())
		return pNetChan->GetLatency(FLOW_OUTGOING);
	return 0.0f;
}

float CLagRecords::GetServerAcceptedAgeRange() const
{
	// Cache per frame — same pattern as GetServerAcceptedAge
	const int nFrame = I::GlobalVars ? I::GlobalVars->framecount : 0;
	if (m_nCacheFrame == nFrame && m_flCachedAgeRange >= 0.0f)
		return m_flCachedAgeRange;

	// Dynamic window based on REAL outgoing latency (not fake latency).
	// Lower ping → wider window (server is more lenient with low-ping players).
	// Higher ping → tighter window (server's lag compensation is less forgiving).
	const float flRealPing = GetOutgoingLatency() * 1000.0f;  // Convert to ms

	float flRange;
	if (flRealPing <= 89.0f)
		flRange = 0.185f;     // 1-89ms ping → 185ms window
	else if (flRealPing <= 150.0f)
		flRange = 0.180f;     // 90-150ms ping → 180ms window
	else if (flRealPing <= 200.0f)
		flRange = 0.175f;     // 151-200ms ping → 175ms window
	else if (flRealPing <= 220.0f)
		flRange = 0.160f;     // 201-220ms ping → 160ms window
	else
		flRange = 0.130f;     // 221ms+ ping → 130ms window

	m_flCachedAgeRange = flRange;
	return m_flCachedAgeRange;
}

bool CLagRecords::IsRecordAgeValidForServer(float flRecordAge) const
{
	// Server validation: |fake_latency + lerp - clientAge| <= ageRange (see GetServerAcceptedAge for derivation)
	// ageRange is dynamic based on real ping (see GetServerAcceptedAgeRange for tiers)
	// Records too old OR too new will have their tick_count overridden by the server.
	const float flIdealAge = GetServerAcceptedAge();
	const float flRange = GetServerAcceptedAgeRange();

	// Also clamp by sv_maxunlag (1.0s max)
	if (flRecordAge > m_flMaxUnlag || flRecordAge < 0.0f)
		return false;

	// Record must be within the server's acceptance window (both directions)
	// Small buffer (0.05s) for timing imprecision
	return fabsf(flRecordAge - flIdealAge) <= (flRange + 0.05f);
}

void CLagRecords::UpdateDatagram()
{
	auto pNetChan = reinterpret_cast<CNetChannel*>(I::EngineClient->GetNetChannelInfo());
	if (!pNetChan)
	{
		// Reset state when no net channel (disconnected/joining)
		m_dSequences.clear();
		m_iLastInSequence = 0;
		m_nLastInSequenceNr = 0;
		m_flFakeLatency = 0.0f;
		return;
	}

	const auto pLocal = H::Entities->GetLocal();
	if (pLocal)
		m_nOldTickBase = pLocal->m_nTickBase();

	// Detect sequence number reset (happens when joining new game)
	// If current sequence is much lower than last, we've joined a new game
	if (pNetChan->m_nInSequenceNr < m_iLastInSequence - 100)
	{
		// Reset fake latency state for new game
		m_dSequences.clear();
		m_iLastInSequence = 0;
		m_nLastInSequenceNr = 0;
		m_flFakeLatency = 0.0f;
	}

	// Track incoming sequences for fake latency
	if (pNetChan->m_nInSequenceNr > m_iLastInSequence)
	{
		m_iLastInSequence = pNetChan->m_nInSequenceNr;
		
		Sequence_t seq;
		seq.nInReliableState = pNetChan->m_nInReliableState;
		seq.nSequenceNr = pNetChan->m_nInSequenceNr;
		seq.flTime = I::GlobalVars->realtime;
		
		m_dSequences.emplace_front(seq);
	}

	// Keep only last 67 sequences (Amalgam's limit)
	if (m_dSequences.size() > 67)
		m_dSequences.pop_back();
}

void CLagRecords::AddRecord(C_TFPlayer* pPlayer)
{
	// Validate player before doing anything
	if (!pPlayer || !pPlayer->GetClientNetworkable() || !pPlayer->GetClientNetworkable()->GetClientClass())
		return;

	// Check if player has a valid model - if not, skip bone setup
	if (!pPlayer->GetModel())
		return;

	// Early death detection — skip SetupBones on dying/dead players.
	// deadflag() can lag a frame behind the actual death, but m_lifeState and
	// m_iHealth update sooner. SetupBones on a model transitioning to death pose
	// is extremely expensive (ragdoll IK solving, invalid bone configurations)
	// and causes lag spikes on kill even with ragdolls disabled.
	if (pPlayer->m_lifeState() != LIFE_ALIVE || pPlayer->m_iHealth() <= 0)
		return;

	LagRecord_t newRecord = {};

	m_bSettingUpBones = true;

	const bool bSetupBonesOpt = CFG::Misc_SetupBones_Optimization;

	if (bSetupBonesOpt)
		pPlayer->InvalidateBoneCache();

	// Use simulation time, NOT curtime — the record stores server-side position data
	// (SimulationTime, AbsOrigin) that corresponds to the simtime, not the interpolated curtime position.
	// For high-ping players, curtime bones are at the interpolated position which differs from
	// the actual server position, causing lag records to jitter/tear.
	const float flBoneTime = pPlayer->m_flSimulationTime();
	const bool bResult = pPlayer->SetupBones(newRecord.BoneMatrix, 128, BONE_USED_BY_ANYTHING, flBoneTime);

	// Invalidate bone cache after recording — the cache now has simtime bones,
	// but the main model render needs curtime bones for smooth interpolation
	pPlayer->InvalidateBoneCache();

	if (bSetupBonesOpt)
	{
		int nIterations = 0;
		for (auto attach = pPlayer->FirstMoveChild(); attach; attach = attach->NextMovePeer())
		{
			// Safety: limit iterations to prevent infinite loops on corrupted entity chains
			if (++nIterations > 32)
				break;

			// Validate the attachment is a valid entity with a model
			if (!attach->GetClientNetworkable() || !attach->GetClientNetworkable()->GetClientClass())
				continue;

			if (!attach->GetModel())
				continue;

			if (attach->ShouldDraw())
			{
				attach->InvalidateBoneCache();
				attach->SetupBones(nullptr, -1, BONE_USED_BY_ANYTHING, I::GlobalVars->curtime);
			}
		}
	}

	m_bSettingUpBones = false;

	if (!bResult)
		return;

	newRecord.Player = pPlayer;
	newRecord.PlayerIndex = pPlayer->entindex();
	newRecord.SimulationTime = pPlayer->m_flSimulationTime();
	newRecord.AbsOrigin = pPlayer->GetAbsOrigin();
	newRecord.VecOrigin = pPlayer->m_vecOrigin();
	newRecord.AbsAngles = pPlayer->GetAbsAngles();
	newRecord.EyeAngles = pPlayer->GetEyeAngles();
	newRecord.Velocity = pPlayer->m_vecVelocity();
	newRecord.Center = pPlayer->GetCenter();
	newRecord.Flags = pPlayer->m_fFlags();
	newRecord.VecMinsPreScaled = pPlayer->m_vecMinsPreScaled();  // Server stores OBBMinsPreScaled
	newRecord.VecMaxsPreScaled = pPlayer->m_vecMaxsPreScaled();  // Server stores OBBMaxsPreScaled
	newRecord.bIsAlive = !pPlayer->deadflag();           // Server checks LC_ALIVE

	if (const auto pAnimState = pPlayer->GetAnimState())
		newRecord.FeetYaw = pAnimState->m_flCurrentFeetYaw;

	const int nIdx = pPlayer->entindex();
	if (nIdx < 0 || nIdx >= MAX_LAG_RECORDS_SLOTS)
		return;

	auto& records = m_LagRecords[nIdx];

	// Teleport dist check (from Amalgam) — detect when player teleported
	// Server uses sv_lagcompensation_teleport_dist to invalidate old positions
	bool bLagComp = false;
	if (!records.empty())
	{
		const LagRecord_t& lastRecord = records.front(); // newest existing record
		const Vec3 vDelta = newRecord.VecOrigin - lastRecord.VecOrigin;

		static auto sv_lagcompensation_teleport_dist = I::CVar->FindVar("sv_lagcompensation_teleport_dist");
		const float flTeleportDist = sv_lagcompensation_teleport_dist ? powf(sv_lagcompensation_teleport_dist->GetFloat(), 2.f) : powf(64.f, 2.f);

		if (vDelta.Length2DSqr() > flTeleportDist)
		{
			bLagComp = true;
			// If this is the first teleport detection, resize to keep only the last record
			if (!m_bLagCompensation[nIdx])
				records.resize(1);
			// Mark all older records as invalid
			for (auto& record : records)
				record.bInvalid = true;
		}

		// Fix invalid records — copy current record's data to them (Amalgam approach)
		for (auto& record : records)
		{
			if (!record.bInvalid)
				continue;

			record.VecOrigin = newRecord.VecOrigin;
			record.AbsOrigin = newRecord.AbsOrigin;
			record.VecMinsPreScaled = newRecord.VecMinsPreScaled;
			record.VecMaxsPreScaled = newRecord.VecMaxsPreScaled;
			memcpy(record.BoneMatrix, newRecord.BoneMatrix, sizeof(record.BoneMatrix));
		}
	}

	m_bLagCompensation[nIdx] = bLagComp;

	records.emplace_front(newRecord);
}

const LagRecord_t* CLagRecords::GetRecord(C_TFPlayer* pPlayer, int nRecord, bool bSafe)
{
	// Always validate the player pointer - it could be dangling after class change
	if (!pPlayer || !H::Entities->IsEntityValid(pPlayer))
		return nullptr;

	const int nIdx = pPlayer->entindex();
	if (nIdx < 0 || nIdx >= MAX_LAG_RECORDS_SLOTS)
		return nullptr;

	auto& records = m_LagRecords[nIdx];
	if (nRecord < 0 || nRecord >= static_cast<int>(records.size()))
		return nullptr;

	return &records[nRecord];
}

bool CLagRecords::HasRecords(C_TFPlayer* pPlayer, int* pTotalRecords)
{
	if (!pPlayer)
		return false;

	const int nIdx = pPlayer->entindex();
	if (nIdx < 0 || nIdx >= MAX_LAG_RECORDS_SLOTS)
		return false;

	const size_t nSize = m_LagRecords[nIdx].size();
	if (nSize == 0)
		return false;

	if (pTotalRecords)
		*pTotalRecords = static_cast<int>(nSize - 1);

	return true;
}

void CLagRecords::UpdateRecords()
{
	const auto pLocal = H::Entities->GetLocal();

	if (!pLocal || pLocal->deadflag() || pLocal->InCond(TF_COND_HALLOWEEN_GHOST_MODE) || pLocal->InCond(TF_COND_HALLOWEEN_KART))
	{
		// Clear all records when dead/disconnected
		for (int i = 0; i < MAX_LAG_RECORDS_SLOTS; i++)
			m_LagRecords[i].clear();
		return;
	}

	// Iterate flat array by entity index - much faster than unordered_map iteration
	for (int i = 0; i < MAX_LAG_RECORDS_SLOTS; i++)
	{
		auto& records = m_LagRecords[i];
		if (records.empty())
			continue;

		// Validate the player at this entity index is still alive and matches our records
		const auto pEntity = I::ClientEntityList->GetClientEntity(i);
		if (!pEntity || pEntity->GetClassId() != ETFClassIds::CTFPlayer)
		{
			records.clear();
			continue;
		}

		const auto pPlayer = pEntity->As<C_TFPlayer>();
		if (pPlayer->deadflag())
		{
			records.clear();
			continue;
		}

		// Remove individual invalid records
		// No need to validate Player pointer per-record since we validated the entity index above
		const float flCurSimTime = pPlayer->m_flSimulationTime();
		std::erase_if(records, [flCurSimTime, this](const LagRecord_t& curRecord) {
			return !F::LagRecords->IsSimulationTimeValid(flCurSimTime, curRecord.SimulationTime);
		});
	}
}

bool CLagRecords::DiffersFromCurrent(const LagRecord_t* pRecord)
{
	if (!pRecord)
		return false;

	const auto pPlayer = pRecord->Player;

	// Validate player pointer using stored index — avoids virtual call on dangling pointer
	if (!pPlayer)
		return false;
	if (pRecord->PlayerIndex > 0)
	{
		const auto pCheck = I::ClientEntityList->GetClientEntity(pRecord->PlayerIndex);
		if (pCheck != pPlayer)
			return false;
	}
	else
		return false;

	if ((pPlayer->m_vecOrigin() - pRecord->AbsOrigin).Length2DSqr() > 0.1f)
		return true;

	if ((pPlayer->GetEyeAngles() - pRecord->EyeAngles).Length() > 0.1f)
		return true;

	if (pPlayer->m_fFlags() != pRecord->Flags)
		return true;

	if (const auto pAnimState = pPlayer->GetAnimState())
	{
		if (fabsf(pAnimState->m_flCurrentFeetYaw - pRecord->FeetYaw) > 0.0f)
			return true;
	}

	return false;
}

LagRecord_t CLagRecords::GetInterpolatedRecord(C_TFPlayer* pPlayer, float flTargetSimTime, bool* pOutIsInterpolated)
{
	if (pOutIsInterpolated)
		*pOutIsInterpolated = false;

	int numRecords = 0;
	if (!HasRecords(pPlayer, &numRecords))
	{
		// No records — return current position as a record
		// But don't SetupBones on dead/dying players (expensive + causes lag spikes)
		LagRecord_t current = {};
		current.Player = pPlayer;
		current.PlayerIndex = pPlayer->entindex();
		current.SimulationTime = pPlayer->m_flSimulationTime();
		current.AbsOrigin = pPlayer->GetAbsOrigin();
		current.VecMinsPreScaled = pPlayer->m_vecMinsPreScaled();
		current.VecMaxsPreScaled = pPlayer->m_vecMaxsPreScaled();
		current.EyeAngles = pPlayer->GetEyeAngles();
		current.AbsAngles = pPlayer->GetAbsAngles();
		current.bIsAlive = !pPlayer->deadflag();
		current.VecOrigin = pPlayer->m_vecOrigin();
		current.Velocity = pPlayer->m_vecVelocity();
		current.Flags = pPlayer->m_fFlags();
		if (pPlayer->m_lifeState() == LIFE_ALIVE && pPlayer->m_iHealth() > 0)
			pPlayer->SetupBones(current.BoneMatrix, 128, BONE_USED_BY_ANYTHING, I::GlobalVars->curtime);
		return current;
	}

	// Our deque: index 0 = newest (highest simtime), index N = oldest (lowest simtime)
	const LagRecord_t* pOlder = nullptr;
	const LagRecord_t* pNewer = nullptr;

	for (int n = 1; n <= numRecords; n++)
	{
		const auto record = GetRecord(pPlayer, n, true);
		if (!record || !record->bIsAlive)
			continue;

		if (record->SimulationTime <= flTargetSimTime)
		{
			pOlder = record;
			if (n > 1)
			{
				const auto prevRecord = GetRecord(pPlayer, n - 1, true);
				if (prevRecord && prevRecord->bIsAlive && prevRecord->SimulationTime > flTargetSimTime)
					pNewer = prevRecord;
			}
			break;
		}
		pNewer = record;
	}

	if (!pOlder)
	{
		const auto oldest = GetRecord(pPlayer, numRecords, true);
		if (oldest) pOlder = oldest;
	}
	if (!pNewer)
	{
		const auto newest = GetRecord(pPlayer, 1, true);
		if (newest) pNewer = newest;
	}

	if (!pOlder || !pNewer || pOlder == pNewer)
	{
		if (pOlder) return *pOlder;
		if (pNewer) return *pNewer;
		LagRecord_t current = {};
		current.Player = pPlayer;
		current.PlayerIndex = pPlayer->entindex();
		current.SimulationTime = pPlayer->m_flSimulationTime();
		current.AbsOrigin = pPlayer->GetAbsOrigin();
		current.VecMinsPreScaled = pPlayer->m_vecMinsPreScaled();
		current.VecMaxsPreScaled = pPlayer->m_vecMaxsPreScaled();
		current.EyeAngles = pPlayer->GetEyeAngles();
		current.AbsAngles = pPlayer->GetAbsAngles();
		current.bIsAlive = !pPlayer->deadflag();
		current.VecOrigin = pPlayer->m_vecOrigin();
		current.Velocity = pPlayer->m_vecVelocity();
		current.Flags = pPlayer->m_fFlags();
		pPlayer->SetupBones(current.BoneMatrix, 128, BONE_USED_BY_ANYTHING, I::GlobalVars->curtime);
		return current;
	}

	const float flTimeSpan = pNewer->SimulationTime - pOlder->SimulationTime;
	if (flTimeSpan <= 0.001f)
		return *pOlder;

	float flFrac = (flTargetSimTime - pOlder->SimulationTime) / flTimeSpan;
	flFrac = std::clamp(flFrac, 0.0f, 1.0f);

	if (pOutIsInterpolated)
		*pOutIsInterpolated = (flFrac > 0.001f && flFrac < 0.999f);

	return InterpolateRecords(pOlder, pNewer, flFrac);
}

void CLagRecordMatrixHelper::Set(const LagRecord_t* pRecord)
{
	if (!pRecord)
		return;

	const auto pPlayer = pRecord->Player;

	// Validate the player pointer using stored index — avoids virtual call on dangling pointer
	if (!pPlayer)
		return;
	if (pRecord->PlayerIndex > 0)
	{
		const auto pCheck = I::ClientEntityList->GetClientEntity(pRecord->PlayerIndex);
		if (pCheck != pPlayer)
			return;
	}
	else
		return;

	if (pPlayer->deadflag())
		return;

	// Server checks LC_ALIVE - dead records are never valid for lag compensation
	if (!pRecord->bIsAlive)
		return;

	const auto pCachedBoneData = pPlayer->GetCachedBoneData();

	if (!pCachedBoneData || pCachedBoneData->Count() <= 0 || !pCachedBoneData->Base())
		return;

	m_pPlayer = pPlayer;
	m_vAbsOrigin = pPlayer->GetAbsOrigin();
	m_vAbsAngles = pPlayer->GetAbsAngles();
	
	const int nBoneCount = std::min(pCachedBoneData->Count(), 128);
	memcpy(m_BoneMatrix, pCachedBoneData->Base(), sizeof(matrix3x4_t) * nBoneCount);
	memcpy(pCachedBoneData->Base(), pRecord->BoneMatrix, sizeof(matrix3x4_t) * nBoneCount);

	pPlayer->SetAbsOrigin(pRecord->AbsOrigin);
	pPlayer->SetAbsAngles(pRecord->AbsAngles);

	// Server restores collision bounds (LC_SIZE_CHANGED) during lag compensation
	// Server calls SetSize(minsPreScaled, maxsPreScaled) which sets both pre-scaled and actual bounds
	// The trace system uses m_vecMins/m_vecMaxs for collision detection
	m_vRestoreMinsPreScaled = pPlayer->m_vecMinsPreScaled();
	m_vRestoreMaxsPreScaled = pPlayer->m_vecMaxsPreScaled();
	m_vRestoreMins = pPlayer->m_vecMins();
	m_vRestoreMaxs = pPlayer->m_vecMaxs();
	pPlayer->m_vecMinsPreScaled() = pRecord->VecMinsPreScaled;
	pPlayer->m_vecMaxsPreScaled() = pRecord->VecMaxsPreScaled;
	pPlayer->m_vecMins() = pRecord->VecMinsPreScaled;
	pPlayer->m_vecMaxs() = pRecord->VecMaxsPreScaled;

	m_bSuccessfullyStored = true;
}

void CLagRecordMatrixHelper::Restore()
{
	if (!m_bSuccessfullyStored || !m_pPlayer)
		return;

	const auto pCachedBoneData = m_pPlayer->GetCachedBoneData();

	if (!pCachedBoneData || pCachedBoneData->Count() <= 0 || !pCachedBoneData->Base())
	{
		// Reset state even if we can't restore bones
		m_pPlayer = nullptr;
		m_vAbsOrigin = {};
		m_vAbsAngles = {};
		m_vRestoreMinsPreScaled = {};
		m_vRestoreMaxsPreScaled = {};
		m_vRestoreMins = {};
		m_vRestoreMaxs = {};
		m_bSuccessfullyStored = false;
		return;
	}

	m_pPlayer->SetAbsOrigin(m_vAbsOrigin);
	m_pPlayer->SetAbsAngles(m_vAbsAngles);
	
	// Restore collision bounds (matching server's FinishLagCompensation LC_SIZE_CHANGED)
	m_pPlayer->m_vecMinsPreScaled() = m_vRestoreMinsPreScaled;
	m_pPlayer->m_vecMaxsPreScaled() = m_vRestoreMaxsPreScaled;
	m_pPlayer->m_vecMins() = m_vRestoreMins;
	m_pPlayer->m_vecMaxs() = m_vRestoreMaxs;

	const int nBoneCount = std::min(pCachedBoneData->Count(), 128);
	memcpy(pCachedBoneData->Base(), m_BoneMatrix, sizeof(matrix3x4_t) * nBoneCount);

	m_pPlayer = nullptr;
	m_vAbsOrigin = {};
	m_vAbsAngles = {};
	m_vRestoreMinsPreScaled = {};
	m_vRestoreMaxsPreScaled = {};
	m_vRestoreMins = {};
	m_vRestoreMaxs = {};
	m_bSuccessfullyStored = false;
}


void CLagRecords::AdjustPing(INetChannel* pNetChanInterface)
{
	auto pNetChan = reinterpret_cast<CNetChannel*>(pNetChanInterface);
	if (!pNetChan)
		return;

	// Store original values
	m_nOldInSequenceNr = pNetChan->m_nInSequenceNr;
	m_nOldInReliableState = pNetChan->m_nInReliableState;

	const auto pLocal = H::Entities->GetLocal();
	if (!pLocal || !pLocal->m_iClass())
		return;

	// Get desired fake latency from config (convert ms to seconds)
	// Always apply fake latency regardless of weapon type
	m_flWishFakeLatency = CFG::Aimbot_Hitscan_Fake_Latency / 1000.0f;
	
	// Update max unlag from server cvar
	static auto sv_maxunlag = I::CVar->FindVar("sv_maxunlag");
	if (sv_maxunlag)
		m_flMaxUnlag = sv_maxunlag->GetFloat();

	if (m_flWishFakeLatency <= 0.0f)
	{
		// No fake latency desired, smooth back to 0
		if (m_flFakeLatency > 0.0f)
		{
			m_flFakeLatency = std::max(0.0f, m_flFakeLatency - I::GlobalVars->interval_per_tick);
		}
		return;
	}

	// Calculate real latency from tickbase
	float flReal = TICKS_TO_TIME(pLocal->m_nTickBase() - m_nOldTickBase);
	static float flStaticReal = 0.0f;
	flStaticReal += (flReal + 5 * I::GlobalVars->interval_per_tick - flStaticReal) * 0.1f;

	// Find sequence that gives us the desired fake latency
	int nInReliableState = pNetChan->m_nInReliableState;
	int nInSequenceNr = pNetChan->m_nInSequenceNr;
	float flLatency = 0.0f;

	for (auto& seq : m_dSequences)
	{
		nInReliableState = seq.nInReliableState;
		nInSequenceNr = seq.nSequenceNr;
		flLatency = (I::GlobalVars->realtime - seq.flTime) - I::GlobalVars->interval_per_tick;

		// Stop if we've reached desired latency or limits
		if (flLatency > m_flWishFakeLatency || 
			m_nLastInSequenceNr >= seq.nSequenceNr || 
			flLatency > m_flMaxUnlag - flStaticReal)
			break;
	}

	// Failsafe: don't go over 1 second
	if (flLatency > 1.0f)
		return;

	// Apply the fake latency by rewinding sequence numbers
	pNetChan->m_nInSequenceNr = nInSequenceNr;
	pNetChan->m_nInReliableState = nInReliableState;
	
	m_nLastInSequenceNr = nInSequenceNr;

	// Smooth the fake latency value
	if (m_flWishFakeLatency > 0.0f || m_flFakeLatency > 0.0f)
	{
		float flDelta = flLatency - m_flFakeLatency;
		flDelta = std::clamp(flDelta, -I::GlobalVars->interval_per_tick, I::GlobalVars->interval_per_tick);
		m_flFakeLatency += flDelta * 0.1f;
		
		// Snap to 0 if very close
		if (!m_flWishFakeLatency && m_flFakeLatency < I::GlobalVars->interval_per_tick)
			m_flFakeLatency = 0.0f;
	}

	// NOTE: Anti-cheat compatibility clamping is done in GetFakeLatency() when the value is used,
	// not here. This preserves the internal state while only clamping what's sent to the server.
}

void CLagRecords::RestorePing(INetChannel* pNetChanInterface)
{
	auto pNetChan = reinterpret_cast<CNetChannel*>(pNetChanInterface);
	if (!pNetChan)
		return;

	// Restore original sequence numbers
	pNetChan->m_nInSequenceNr = m_nOldInSequenceNr;
	pNetChan->m_nInReliableState = m_nOldInReliableState;
}

float CLagRecords::GetFakeLatency() const
{
	// NOTE: Don't clamp fake latency for anti-cheat - Amalgam's GetFakeLatency() doesn't clamp.
	// Only GetFakeInterp() is clamped in Amalgam. The fake latency system works by manipulating
	// sequence numbers, which is separate from the interp system that sends cl_interp to server.
	return m_flFakeLatency;
}
