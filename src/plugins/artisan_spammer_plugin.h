#pragma once
#include "plugin.h"
#include "base.h"
#include <vector>
#include <utility>

class ArtisanSpammerPlugin : public IPlugin {
public:
    const char* GetName() const override { return "Artisan Spammer"; }
    void Update() override;
    void RenderUI() override;

private:
    static bool SendArtisanPacket(OBJID targetItemId, OBJID materialItemId, uint32_t action);

    int  m_selectedTarget = -1;     // index into unique equipment type list
    int  m_materialType = 0;        // 0=Meteor, 1=MeteorScroll, 2=DragonBall

    bool m_spamming = false;
    std::vector<std::pair<OBJID, OBJID>> m_queue;  // (target, material) pairs
    int  m_sentCount = 0;
    int  m_totalCount = 0;
    DWORD m_lastSendTick = 0;
    int  m_delayMs = 100;           // ms between packets
};
