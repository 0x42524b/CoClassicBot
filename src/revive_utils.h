#pragma once
#include "base.h"
#include <cstdint>

class CHero;

struct ReviveState {
    DWORD deathTick = 0;
    DWORD lastReviveAttemptTick = 0;
};

// Returns true if hero is dead and revive handling is active (caller should return from Update).
// Manages the death-detection, delay countdown, and periodic revive-attempt logic.
// If autoReviveInTown is false, returns false immediately so the caller can handle the
// dead-but-no-revive case (e.g. transition to a Failed state).
// On first death detection the caller is responsible for any plugin-specific cleanup
// (stopping travel, resetting sequences, etc.) via the onJustDied callback if needed;
// this function calls Pathfinder::Get().Stop() unconditionally on first death.
// statusText/statusSize receive countdown or "Pressing Revive..." messages.
bool HandleRevive(CHero* hero, ReviveState& state,
    DWORD reviveDelayMs, DWORD reviveRetryIntervalMs,
    bool autoReviveInTown, char* statusText, size_t statusSize);
