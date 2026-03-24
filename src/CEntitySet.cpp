#include "CEntitySet.h"
#include "game.h"

CEntitySet* CEntitySet::GetInstance()
{
    return Game::GetEntitySet();
}

const CSyndicateEntry* CEntitySet::FindSyndicate(OBJID syndicateId) const
{
    for (auto& sp : m_vec) {
        if (sp && sp->m_id == static_cast<int>(syndicateId))
            return sp.get();
    }
    return nullptr;
}

const char* CEntitySet::GetSyndicateName(OBJID syndicateId) const
{
    auto* entry = FindSyndicate(syndicateId);
    return entry ? entry->m_szName.c_str() : nullptr;
}

const char* GetSyndicateRankName(int rank)
{
    switch (rank) {
        case SyndicateRank::LEADER:     return "Leader";
        case SyndicateRank::DEPUTY:     return "Deputy";
        case SyndicateRank::MANAGER:    return "Manager";
        case SyndicateRank::AIDE:       return "Aide";
        case SyndicateRank::SUPERVISOR: return "Supervisor";
        case SyndicateRank::MEMBER:     return "Member";
        default:                        return "";
    }
}
