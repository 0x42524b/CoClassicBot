#include "npc_utils.h"
#include "CRole.h"
#include "game.h"
#include <cstring>

CRole* FindNpcByName(const char* name, const Position& expectedPos, int radius)
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    if (!mgr || mgr->m_deqRole.empty() || mgr->m_deqRole.size() >= 10000)
        return nullptr;

    CRole* best = nullptr;
    float bestDist = (float)(radius + 1);
    for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; ++i) {
        const auto& roleRef = mgr->m_deqRole[i];
        if (!roleRef)
            continue;

        CRole* role = roleRef.get();
        if (role->IsPlayer() || role->IsMonster())
            continue;
        if (name && name[0] && _stricmp(role->GetName(), name) != 0)
            continue;

        const float dist = expectedPos.DistanceTo(role->m_posMap);
        if (dist < bestDist) {
            bestDist = dist;
            best = role;
        }
    }

    return best;
}
