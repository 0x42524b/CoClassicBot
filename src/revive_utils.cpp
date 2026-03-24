#include "revive_utils.h"
#include "CHero.h"
#include "pathfinder.h"
#include <spdlog/spdlog.h>
#include <cstdio>

bool HandleRevive(CHero* hero, ReviveState& state,
    DWORD reviveDelayMs, DWORD reviveRetryIntervalMs,
    bool autoReviveInTown, char* statusText, size_t statusSize)
{
    if (!hero->IsDead()) {
        state.deathTick = 0;
        state.lastReviveAttemptTick = 0;
        return false;
    }

    if (!autoReviveInTown)
        return false;

    const DWORD now = GetTickCount();
    if (state.deathTick == 0) {
        state.deathTick = now;
        state.lastReviveAttemptTick = 0;
        Pathfinder::Get().Stop();
        spdlog::info("[revive] Hero died, waiting {}ms before revive attempt", reviveDelayMs);
        if (statusText && statusSize > 0)
            snprintf(statusText, statusSize, "Died, waiting to revive...");
    }

    const DWORD elapsed = now - state.deathTick;
    if (elapsed < reviveDelayMs) {
        const DWORD remaining = (reviveDelayMs - elapsed + 999) / 1000;
        if (statusText && statusSize > 0)
            snprintf(statusText, statusSize, "Dead, reviving in %lu s", (unsigned long)remaining);
        return true;
    }

    if (now - state.lastReviveAttemptTick >= reviveRetryIntervalMs) {
        hero->ReviveInTown();
        state.lastReviveAttemptTick = now;
    }

    if (statusText && statusSize > 0)
        snprintf(statusText, statusSize, "Pressing Revive to return to town");
    return true;
}
