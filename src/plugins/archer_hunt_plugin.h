#pragma once
#include "base_hunt_plugin.h"

class CMagic;

class ArcherHuntPlugin : public BaseHuntPlugin {
public:
    const char* GetName() const override { return "Archer Hunt"; }
    int GetLastScatterRange() const override { return m_lastScatterRange; }

protected:
    // Required virtual overrides
    CRole* FindBestTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        Position* outApproachPos, Position* outAttackPos,
        int* outClumpSize, bool* outUseScatter) override;
    void HandleCombatApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& approachPos, bool movementCommitted) override;
    void HandleCombatAttack(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& attackPos, DWORD now) override;
    bool HandleCombatRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target) override;
    bool HandleCombatItems(CHero* hero, const AutoHuntSettings& settings) override;
    bool NeedsTownRunArrows(const CHero* hero, const AutoHuntSettings& settings) const override;
    void RenderCombatUI(AutoHuntSettings& settings) override;
    bool HandleNoTargetIdle(CHero* hero, CGameMap* map, const AutoHuntSettings& settings) override;
    void RefreshCombatState(CHero* hero, const AutoHuntSettings& settings) override;
    AutoHuntCombatMode GetExpectedCombatMode() const override { return AutoHuntCombatMode::Archer; }

private:
    // Scatter geometry
    CMagic* FindScatterMagic(const CHero* hero) const;
    int GetScatterRange(const CHero* hero) const;
    bool IsTargetInScatterSector(const Position& origin, const Position& castPos,
        const Position& targetPos, int range) const;
    bool FindBestScatterShot(const std::vector<CRole*>& targets, const Position& origin,
        int range, int minimumHits, Position& outCastPos,
        CRole*& outPrimaryTarget, int& outHitCount) const;
    bool FindBestScatterApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        const std::vector<CRole*>& targets, int range, int minimumHits,
        Position& outApproachPos, Position& outCastPos,
        CRole*& outPrimaryTarget, int& outHitCount,
        bool preferFarTiles = false) const;

    // Retreat
    bool TryArcherDangerRetreat(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, CRole* target);
    bool FindSafeArcherRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        const std::vector<CRole*>& threats, CRole* target, Position& outRetreatPos,
        int safetyDistOverride = 0) const;

    // Target finding
    CRole* FindBestArcherTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        Position* outApproachPos, Position* outAttackPos,
        int* outClumpSize, bool* outUseScatter) const;
    bool FindArcherPatrolPosition(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, Position& outPatrolPos) const;

    // Arrow management
    bool TryManageArrows(CHero* hero, const AutoHuntSettings& settings);
    bool NeedsArrows(const CHero* hero, const AutoHuntSettings& settings) const;
    int CountUsableArrowPacks(const CHero* hero) const;

    // Archer-specific state
    int m_lastScatterRange = 0;
    int m_lastScatterHitCount = 0;
    mutable DWORD m_lastScatterApproachTick = 0;
    Position m_lastFailedRetreatDest = {};
    DWORD m_lastFailedRetreatTick = 0;
    DWORD m_retreatCooldownTick = 0;
    DWORD m_retreatHoldUntilTick = 0;
    DWORD m_lastArrowTick = 0;
};
