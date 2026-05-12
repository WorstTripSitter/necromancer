// ============================================
// CPrediction_RunSimulation Hook
// ============================================
// Ports Amalgam's tickbase fix for doubletap shifts.
//
// When we shift N ticks, the client creates N commands with tickbase
// T+1, T+2, ..., T+N. The server receives them all at once and processes
// them sequentially, advancing its tickbase by 1 per command.
//
// The problem: When Prediction::Update replays these commands via
// RunSimulation, m_nTickBase has already been advanced by the shift.
// The server hasn't ACKed them yet, so the client's predicted tickbase
// is ahead of what the server will confirm, causing prediction errors.
//
// The fix (from Amalgam): When a shift command is being simulated,
// subtract the shift amount from m_nTickBase. RunCommand then adds +1
// per command naturally, so the net tickbase advancement is correct
// from the server's perspective.
// ============================================

#include "../../SDK/SDK.h"
#include "../Features/EnginePrediction/EnginePrediction.h"
#include "../Features/TickbaseManip/TickbaseManip.h"

MAKE_SIGNATURE(CPrediction_RunSimulation, "client.dll", "48 83 EC 38 4C 8B 44", 0x0);

// Tickbase fix entries — recorded when a shift starts, applied during RunSimulation
struct TickbaseFix_t
{
    CUserCmd* m_pCmd;                  // The first shift command (identifies the shift batch)
    int m_iLastOutgoingCommand;        // lastoutgoingcommand at shift start
    int m_iTickbaseShift;              // Number of ticks shifted (N)
};
static std::vector<TickbaseFix_t> s_vTickbaseFixes = {};

MAKE_HOOK(CPrediction_RunSimulation, Signatures::CPrediction_RunSimulation.Get(), void, __fastcall,
    void* rcx, int current_command, float curtime, CUserCmd* cmd, C_TFPlayer* localPlayer)
{
    // Safety check
    if (!localPlayer || !cmd)
        return CALL_ORIGINAL(rcx, current_command, curtime, cmd, localPlayer);

    // During recharging, skip prediction for local player to avoid
    // stale prediction state (we're not sending commands)
    if (Shifting::bRecharging)
    {
        if (const auto pLocal = H::Entities->GetLocal())
        {
            if (localPlayer == pLocal)
                return;
        }
    }

    // Tickbase fix: when a shift starts, record the shift info
    // (bShifting is set true in CL_Move before the shift loop, and
    //  nCurrentShiftTick == 0 means this is the first shift tick)
    if (Shifting::bShifting && Shifting::nCurrentShiftTick == 0 && Shifting::nTotalShiftTicks > 0)
    {
        s_vTickbaseFixes.emplace_back(G::CurrentUserCmd, I::ClientState->lastoutgoingcommand, Shifting::nTotalShiftTicks);
    }

    // Apply tickbase fix for shift commands
    for (auto it = s_vTickbaseFixes.begin(); it != s_vTickbaseFixes.end();)
    {
        // Clean up entries that have been ACKed by the server
        if (it->m_iLastOutgoingCommand < I::ClientState->last_command_ack)
        {
            it = s_vTickbaseFixes.erase(it);
            continue;
        }
        // If this command is part of a shift batch, subtract the shift amount
        if (cmd == it->m_pCmd)
        {
            localPlayer->m_nTickBase() -= it->m_iTickbaseShift;
            break;
        }
        ++it;
    }

    F::EnginePrediction->AdjustPlayers(localPlayer);
    CALL_ORIGINAL(rcx, current_command, curtime, cmd, localPlayer);
    F::EnginePrediction->RestorePlayers();
}

// Public interface for level shutdown cleanup
namespace TickbaseFix
{
    inline void Clear() { s_vTickbaseFixes.clear(); }
    inline void OnShiftStart(int) {}
}
