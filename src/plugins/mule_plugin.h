#pragma once
#include "plugin.h"
#include "base.h"

enum class MuleState {
    Idle,
    WaitingForGame,
    TravelToMarket,
    MoveToWarehouseman,
    WaitingForIncomingTrade,
    AcceptIncomingTrade,
    ConfirmTrade,
};

struct MuleSettings
{
    bool enabled = false;
    char whitelistNames[256] = "";
};

MuleSettings& GetMuleSettings();

class CHero;
class CGameMap;
class CRole;
class CEntityInfo;
class TravelPlugin;

class MulePlugin : public IPlugin {
public:
    const char* GetName() const override { return "Mule"; }
    void Update() override;
    void RenderUI() override;
    bool OnMapClick(const Position& tile) override;

private:
    void SetState(MuleState state, const char* statusText);
    const char* GetStateName() const;
    bool IsWhitelisted(const MuleSettings& settings, const char* playerName) const;
    CRole* FindNearbyRequester(const Position& spot, OBJID requesterId, int radius) const;
    CRole* FindNearbyWhitelistedTrader(const MuleSettings& settings, const Position& spot, int radius) const;
    void BeginTravelToMarket(TravelPlugin* travel);
    void ResetTradeSession();

    MuleState m_state = MuleState::Idle;
    char m_statusText[128] = "Disabled";
    Position m_lastHeroPos = {0, 0};
    OBJID m_lastMapId = 0;
    size_t m_lastBagCount = 0;
    OBJID m_tradePartnerId = 0;
    DWORD m_tradeSessionTick = 0;
    DWORD m_lastTradeStartTick = 0;
    DWORD m_lastTradeAcceptTick = 0;
    DWORD m_lastBagChangeTick = 0;
};
