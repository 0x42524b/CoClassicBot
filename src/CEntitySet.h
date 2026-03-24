#pragma once
#include "base.h"
#include <string>
#include <vector>

// =====================================================================
// Syndicate rank values (from rank lookup table at RVA 0x453BE8)
// =====================================================================
namespace SyndicateRank {
    constexpr int NONE       = 0;
    constexpr int MEMBER     = 50;
    constexpr int SUPERVISOR = 60;
    constexpr int AIDE       = 70;
    constexpr int MANAGER    = 80;
    constexpr int DEPUTY     = 90;
    constexpr int LEADER     = 100;
}

// =====================================================================
// CSyndicateEntry — a single syndicate record in the entity set
//
// Layout from CEntitySet::GetEntityName and
// CRole::RenderSyndicateInfo (RVA 0x1A3400).
// =====================================================================
#pragma pack(push, 1)
struct CSyndicateEntry
{
    int          m_id;           // +0x00  syndicate ID
    int          _pad04;         // +0x04
    std::string  m_szName;       // +0x08  syndicate name
    std::string  m_szSubTitle;   // +0x28  rank subtitle
    int          m_nColorType;   // +0x48  0=ally(green), 1=enemy(red), 2=neutral(yellow)
};
#pragma pack(pop)

static_assert(offsetof(CSyndicateEntry, m_id)         == 0x00, "CSyndicateEntry::m_id");
static_assert(offsetof(CSyndicateEntry, m_szName)      == 0x08, "CSyndicateEntry::m_szName");
static_assert(offsetof(CSyndicateEntry, m_szSubTitle)  == 0x28, "CSyndicateEntry::m_szSubTitle");
static_assert(offsetof(CSyndicateEntry, m_nColorType)  == 0x48, "CSyndicateEntry::m_nColorType");

// =====================================================================
// CEntitySet — game singleton managing syndicate entries
//
// Singleton located at base + 0x4DF5F0.
// Contains a vector<shared_ptr<CSyndicateEntry>> at +0x50.
// =====================================================================
#pragma pack(push, 1)
class CEntitySet
{
    BYTE _pad00[0x50];                                    // +0x00  vtable + internal fields
public:
    std::vector<std::shared_ptr<CSyndicateEntry>> m_vec;  // +0x50  syndicate entries

    static CEntitySet* GetInstance();

    const CSyndicateEntry* FindSyndicate(OBJID syndicateId) const;
    const char* GetSyndicateName(OBJID syndicateId) const;
};
#pragma pack(pop)

static_assert(offsetof(CEntitySet, m_vec) == 0x50, "CEntitySet::m_vec");

const char* GetSyndicateRankName(int rank);
