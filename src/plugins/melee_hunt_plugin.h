#pragma once
#include "base_hunt_plugin.h"

class MeleeHuntPlugin : public BaseHuntPlugin {
public:
    const char* GetName() const override { return "Melee Hunt"; }

protected:
    CRole* FindBestTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        Position* outApproachPos, Position* outAttackPos,
        int* outClumpSize, bool* outUseScatter) override;

    void HandleCombatApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& approachPos, bool movementCommitted) override;

    void HandleCombatAttack(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& attackPos, DWORD now) override;

    void RenderCombatUI(AutoHuntSettings& settings) override;
    AutoHuntCombatMode GetExpectedCombatMode() const override { return AutoHuntCombatMode::Melee; }

private:
    CRole* FindBestMeleeTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        Position* outApproachPos, int* outClumpSize) const;

    bool FindBestClumpApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        const std::vector<CRole*>& targets, Position& outApproachPos,
        CRole*& outPrimaryTarget, int& outClumpSize) const;
};
