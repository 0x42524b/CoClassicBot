#pragma once
#include "hunt_settings.h"
#include "base.h"
#include <vector>

class CRole;
class CHero;
class CGameMap;

// Collect all live monster targets that satisfy zone/name filters.
// If preferredOnly is true, only monsters matching monsterPreferNames are returned.
std::vector<CRole*> CollectHuntTargets(const AutoHuntSettings& settings, bool preferredOnly = false);

// Count targets within a Euclidean radius of center.
int CountTargetsInRadius(const std::vector<CRole*>& targets, const Position& center, float radius);

// Return the closest target within maxTileRange (Chebyshev). Pass -1 for no range limit.
CRole* FindClosestTarget(const std::vector<CRole*>& targets, const Position& from, int maxTileRange = -1);

// Return a random target within maxTileRange (Chebyshev).
CRole* FindRandomTarget(const std::vector<CRole*>& targets, const Position& from, int maxTileRange);

// Return the target that is the center of the densest cluster within radius.
// Writes cluster size into *outClusterSize if non-null.
CRole* FindBestClusterTarget(const std::vector<CRole*>& targets, const Position& from,
    float radius, int* outClusterSize);

// Find the best single-jump approach position to attack target from heroPos.
// heroPos should be GetEffectiveHeroPosition(hero) from the caller.
// Returns true and writes outApproachPos if a valid tile is found.
bool FindBestSingleTargetApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target, const Position& heroPos, Position& outApproachPos, int attackRange);
