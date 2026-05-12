#include "TickbaseManip.h"
#include "../CFG.h"

// ============================================
// Auto Settings Implementation
// ============================================
// Ping brackets (ms):
//   Low:    0-50   — LAN/localhost, minimal compensation needed
//   Medium: 50-90  — Typical casual server
//   High:   90-150 — Distant community server
//   VHigh:  150-250 — Overseas server
//   Extreme: 250+   — Severe lag, conservative settings
// ============================================

float CTickbaseManip::GetPingMs()
{
	float flLatency = 0.0f;
	if (const auto pNetChannel = I::EngineClient->GetNetChannelInfo())
	{
		flLatency = pNetChannel->GetLatency(FLOW_OUTGOING) + pNetChannel->GetLatency(FLOW_INCOMING);
	}
	return flLatency * 1000.0f;
}

int CTickbaseManip::GetOptimalRechargeLimit()
{
	if (!CFG::Exploits_RapidFire_Auto_Settings)
		return CFG::Exploits_Shifting_Recharge_Limit;

	// Cap by sv_maxusrcmdprocessticks — recharging beyond what the server
	// can process is wasteful and causes deficit
	const int nServerMax = Shifting::nMaxUsrCmdProcessTicks;
	const int nHardCap = std::min(nServerMax, 24);

	float flPing = GetPingMs();
	int nLimit;
	if (flPing <= 50.0f)
		nLimit = nHardCap;         // Low ping: max out
	else if (flPing <= 90.0f)
		nLimit = nHardCap - 1;    // Medium: one less for safety margin
	else if (flPing <= 150.0f)
		nLimit = nHardCap - 2;    // High: account for jitter
	else if (flPing <= 250.0f)
		nLimit = nHardCap - 3;    // VHigh: conservative
	else
		nLimit = nHardCap - 4;    // Extreme: very conservative

	// If we have deficit, reduce limit further — server is already overwhelmed
	if (Shifting::nDeficit > 0)
		nLimit = std::max(nLimit - Shifting::nDeficit, 8);

	return std::clamp(nLimit, 8, 24);
}

int CTickbaseManip::GetOptimalDTTicks()
{
	if (!CFG::Exploits_RapidFire_Auto_Settings)
		return CFG::Exploits_RapidFire_Ticks;

	// DT ticks = how many ticks we shift at once.
	// Must not exceed sv_maxusrcmdprocessticks or the server drops commands.
	const int nServerMax = Shifting::nMaxUsrCmdProcessTicks;

	float flPing = GetPingMs();
	int nTicks;
	if (flPing <= 50.0f)
		nTicks = std::min(nServerMax, 22);   // Low: aggressive
	else if (flPing <= 90.0f)
		nTicks = std::min(nServerMax - 1, 21);  // Medium
	else if (flPing <= 150.0f)
		nTicks = std::min(nServerMax - 2, 20);  // High
	else if (flPing <= 250.0f)
		nTicks = std::min(nServerMax - 3, 18);  // VHigh
	else
		nTicks = std::min(nServerMax - 4, 16);  // Extreme

	// If tick tracking says we're too far ahead, reduce shift ticks
	if (Shifting::nTicksAhead > 0)
	{
		// Each tick ahead = one less we can safely shift
		int nReduction = std::min(Shifting::nTicksAhead / 2, nTicks - 4);
		nTicks -= std::max(nReduction, 0);
	}

	// If we have deficit, reduce further
	if (Shifting::nDeficit > 0)
		nTicks = std::max(nTicks - Shifting::nDeficit, 4);

	return std::clamp(nTicks, 4, 22);
}

int CTickbaseManip::GetOptimalDelayTicks()
{
	if (!CFG::Exploits_RapidFire_Auto_Settings)
		return CFG::Exploits_RapidFire_Min_Ticks_Target_Same;

	// Delay ticks = how long we wait after CanFire before doubletapping.
	// Higher ping = more delay to ensure server-side CanFire is true.
	float flPing = GetPingMs();
	if (flPing <= 50.0f)
		return 1;
	else if (flPing <= 90.0f)
		return 2;
	else if (flPing <= 150.0f)
		return 3;
	else if (flPing <= 250.0f)
		return 4;
	else
		return 5;
}

int CTickbaseManip::GetOptimalTickTrackingMode()
{
	if (!CFG::Exploits_RapidFire_Auto_Settings)
		return CFG::Exploits_RapidFire_Tick_Tracking;

	// Auto-select tick tracking mode based on conditions:
	// - Low ping + no deficit: disabled (unnecessary overhead)
	// - Medium ping or slight ahead: adaptive (mode 1)
	// - High ping or deficit: always adaptive
	float flPing = GetPingMs();
	if (flPing <= 50.0f && Shifting::nDeficit == 0 && Shifting::nTicksAhead <= 3)
		return 0;   // Disabled — not needed at low ping with no issues
	return 1;       // Adaptive — latency + deficit aware
}

bool CTickbaseManip::GetOptimalDeficitTracking()
{
	if (!CFG::Exploits_RapidFire_Auto_Settings)
		return CFG::Exploits_RapidFire_Deficit_Tracking;

	// Auto-enable deficit tracking when ping is high enough that
	// the server might drop commands
	float flPing = GetPingMs();
	return flPing > 50.0f;   // Enable for anything above LAN ping
}

int CTickbaseManip::GetMaxSafeShiftTicks()
{
	// How many ticks can we safely shift without the server rejecting commands?
	// This is bounded by sv_maxusrcmdprocessticks minus any existing deficit,
	// minus ticks we're already ahead, and minus choked commands that will
	// be sent in the same packet (they also consume server processing budget).
	const int nServerMax = Shifting::nMaxUsrCmdProcessTicks;
	int nSafe = nServerMax;

	// Subtract choked commands — they'll be sent alongside the shift commands
	// and consume the server's m_nMovementTicksForUserCmdProcessingRemaining
	const int nChoked = I::ClientState->chokedcommands;
	if (nChoked > 0)
		nSafe -= nChoked;

	// Subtract deficit — server is already behind
	if (Shifting::nDeficit > 0)
		nSafe -= Shifting::nDeficit;

	// Subtract ticks ahead — we're already sending more than server has processed
	if (Shifting::nTicksAhead > 0)
		nSafe -= std::min(Shifting::nTicksAhead, nSafe - 1);

	return std::clamp(nSafe, 1, nServerMax);
}
