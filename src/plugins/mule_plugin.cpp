#include "mule_plugin.h"
#include "hooks.h"
#include "plugin_mgr.h"
#include "travel_plugin.h"
#include "CEntityInfo.h"
#include "game.h"
#include "gateway.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CRole.h"
#include "log.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace {
const Position kMarketWarehousePos = {182, 180};
constexpr DWORD kTradeStartIntervalMs = 1200;
constexpr DWORD kTradeAcceptIntervalMs = 1200;
constexpr DWORD kTradeSessionTimeoutMs = 8000;

std::string ToLowerCopy(const std::string& value)
{
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return lower;
}

std::vector<std::string> ParseTokens(const char* text)
{
    std::vector<std::string> tokens;
    if (!text || !text[0])
        return tokens;

    std::string current;
    while (*text) {
        const char ch = *text++;
        if (ch == ',' || ch == ';' || ch == '\n' || ch == '\r' || ch == '\t') {
            if (!current.empty()) {
                size_t start = 0;
                while (start < current.size() && std::isspace((unsigned char)current[start]))
                    ++start;
                size_t end = current.size();
                while (end > start && std::isspace((unsigned char)current[end - 1]))
                    --end;
                if (end > start)
                    tokens.push_back(ToLowerCopy(current.substr(start, end - start)));
                current.clear();
            }
            continue;
        }

        current.push_back(ch);
    }

    if (!current.empty()) {
        size_t start = 0;
        while (start < current.size() && std::isspace((unsigned char)current[start]))
            ++start;
        size_t end = current.size();
        while (end > start && std::isspace((unsigned char)current[end - 1]))
            --end;
        if (end > start)
            tokens.push_back(ToLowerCopy(current.substr(start, end - start)));
    }

    return tokens;
}

bool IsMovementCommandStillAdvancing(const CHero* hero)
{
    if (!hero)
        return false;

    const CCommand& cmd = hero->GetCommand();
    return (cmd.iType == _COMMAND_WALK
            || cmd.iType == _COMMAND_RUN
            || cmd.iType == _COMMAND_WALKFORWARD
            || cmd.iType == _COMMAND_RUNFORWARD
            || cmd.iType == _COMMAND_JUMP)
        && (cmd.posTarget.x != hero->m_posMap.x || cmd.posTarget.y != hero->m_posMap.y);
}
}

static MuleSettings g_muleSettings;
MuleSettings& GetMuleSettings() { return g_muleSettings; }

static const char* StateName(MuleState state)
{
    switch (state) {
        case MuleState::Idle:                 return "Idle";
        case MuleState::WaitingForGame:       return "Waiting For Game";
        case MuleState::TravelToMarket:       return "Travel To Market";
        case MuleState::MoveToWarehouseman:   return "Move To Warehouseman";
        case MuleState::WaitingForIncomingTrade: return "Waiting For Incoming Trade";
        case MuleState::AcceptIncomingTrade:  return "Accept Incoming Trade";
        case MuleState::ConfirmTrade:         return "Confirm Trade";
        default:                              return "Unknown";
    }
}

void MulePlugin::SetState(MuleState state, const char* statusText)
{
    if (m_state != state)
        spdlog::info("[mule] State: {} -> {} | {}", GetStateName(), StateName(state),
            statusText ? statusText : "");
    m_state = state;
    snprintf(m_statusText, sizeof(m_statusText), "%s", statusText ? statusText : "");
}

const char* MulePlugin::GetStateName() const
{
    return StateName(m_state);
}

bool MulePlugin::IsWhitelisted(const MuleSettings& settings, const char* playerName) const
{
    if (!playerName || !playerName[0])
        return false;

    const std::string lowerName = ToLowerCopy(playerName);
    for (const std::string& token : ParseTokens(settings.whitelistNames)) {
        if (token == lowerName)
            return true;
    }
    return false;
}

CRole* MulePlugin::FindNearbyRequester(const Position& spot, OBJID requesterId, int radius) const
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    CHero* hero = Game::GetHero();
    if (!mgr || !hero || requesterId == 0 || mgr->m_deqRole.empty() || mgr->m_deqRole.size() >= 10000)
        return nullptr;

    for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; ++i) {
        const auto& roleRef = mgr->m_deqRole[i];
        if (!roleRef)
            continue;

        CRole* role = roleRef.get();
        if (!role->IsPlayer() || role->GetID() == hero->GetID())
            continue;
        if (role->GetID() != requesterId)
            continue;

        const float dist = spot.DistanceTo(role->m_posMap);
        if (dist <= (float)radius)
            return role;
    }

    return nullptr;
}

CRole* MulePlugin::FindNearbyWhitelistedTrader(const MuleSettings& settings, const Position& spot, int radius) const
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    CHero* hero = Game::GetHero();
    if (!mgr || !hero || mgr->m_deqRole.empty() || mgr->m_deqRole.size() >= 10000)
        return nullptr;

    CRole* best = nullptr;
    float bestDist = (float)(radius + 1);
    for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; ++i) {
        const auto& roleRef = mgr->m_deqRole[i];
        if (!roleRef)
            continue;

        CRole* role = roleRef.get();
        if (!role->IsPlayer() || role->GetID() == hero->GetID())
            continue;
        if (!IsWhitelisted(settings, role->GetName()))
            continue;

        const float dist = spot.DistanceTo(role->m_posMap);
        if (dist <= (float)radius && dist < bestDist) {
            best = role;
            bestDist = dist;
        }
    }

    return best;
}

void MulePlugin::BeginTravelToMarket(TravelPlugin* travel)
{
    if (!travel) {
        SetState(MuleState::WaitingForGame, "Travel plugin not available");
        return;
    }

    travel->StartTravel(MAP_MARKET, kMarketWarehousePos);
    SetState(MuleState::TravelToMarket, "Traveling to Market");
}

void MulePlugin::ResetTradeSession()
{
    m_tradePartnerId = 0;
    m_tradeSessionTick = 0;
    m_lastTradeStartTick = 0;
    m_lastTradeAcceptTick = 0;
}

void MulePlugin::Update()
{
    MuleSettings& settings = GetMuleSettings();
    TravelPlugin* travel = PluginManager::Get().GetPlugin<TravelPlugin>();
    CHero* hero = Game::GetHero();
    CGameMap* map = Game::GetMap();
    const DWORD now = GetTickCount();

    m_lastHeroPos = hero ? hero->m_posMap : Position{};
    m_lastMapId = map ? map->GetId() : 0;
    if (hero && hero->m_deqItem.size() != m_lastBagCount)
        m_lastBagChangeTick = GetTickCount();
    m_lastBagCount = hero ? hero->m_deqItem.size() : 0;

    if (!settings.enabled) {
        if (m_state != MuleState::Idle) {
            ResetTradeSession();
            if (travel && travel->IsTraveling())
                travel->CancelTravel();
        }
        SetState(MuleState::Idle, "Disabled");
        return;
    }

    if (!hero || !map) {
        SetState(MuleState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    if (map->GetId() != MAP_MARKET) {
        ResetTradeSession();
        if (!travel) {
            SetState(MuleState::WaitingForGame, "Travel plugin not available");
            return;
        }
        if (travel->GetState() == TravelState::Failed) {
            SetState(MuleState::WaitingForGame, "Failed to reach Market");
            return;
        }
        if (!travel->IsTraveling() || travel->GetDestination() != MAP_MARKET)
            BeginTravelToMarket(travel);
        else
            SetState(MuleState::TravelToMarket, "Traveling to Market");
        return;
    }

    if (travel && travel->IsTraveling()) {
        if (travel->GetState() == TravelState::Failed) {
            SetState(MuleState::WaitingForGame, "Failed to reach Warehouseman");
            return;
        }
        SetState(MuleState::MoveToWarehouseman, "Moving near Warehouseman");
        return;
    }

    const int warehouseDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
        kMarketWarehousePos.x, kMarketWarehousePos.y);
    if (warehouseDist > 4) {
        ResetTradeSession();
        if (!travel) {
            SetState(MuleState::WaitingForGame, "Travel plugin not available");
            return;
        }
        BeginTravelToMarket(travel);
        SetState(MuleState::MoveToWarehouseman, "Moving near Warehouseman");
        return;
    }

    if (hero->IsJumping() || IsMovementCommandStillAdvancing(hero)) {
        ResetTradeSession();
        SetState(MuleState::MoveToWarehouseman, "Settling near Warehouseman");
        return;
    }

    const Position anchorPos = hero->m_posMap;
    CRole* preferredTrader = nullptr;
    if (HasIncomingTradeRequest()) {
        const OBJID requesterId = GetIncomingTradeRequesterId();
        const char* requesterName = GetIncomingTradeRequesterName();
        if (IsWhitelisted(settings, requesterName))
            preferredTrader = FindNearbyRequester(anchorPos, requesterId, 8);
    }
    if (!preferredTrader)
        preferredTrader = FindNearbyWhitelistedTrader(settings, anchorPos, 8);

    if (m_tradePartnerId != 0) {
        if (preferredTrader && preferredTrader->GetID() != m_tradePartnerId) {
            ResetTradeSession();
            m_tradePartnerId = preferredTrader->GetID();
            m_tradeSessionTick = now;
            m_lastBagChangeTick = now;
        }

        if (now - m_lastTradeStartTick >= kTradeStartIntervalMs) {
            hero->StartTrade(m_tradePartnerId);
            m_lastTradeStartTick = now;
            SetState(MuleState::AcceptIncomingTrade, "Requesting trade with whitelisted player");
            return;
        }

        if (now - m_tradeSessionTick >= 1000 && now - m_lastTradeAcceptTick >= kTradeAcceptIntervalMs) {
            hero->AcceptTrade(m_tradePartnerId);
            m_lastTradeAcceptTick = now;
            SetState(MuleState::ConfirmTrade, "Accepting trade window");
            return;
        }

        if (m_lastBagChangeTick != 0 && now - m_lastBagChangeTick > kTradeSessionTimeoutMs) {
            ResetTradeSession();
            SetState(MuleState::WaitingForIncomingTrade, "Trade complete, waiting for next request");
            return;
        }

        SetState(MuleState::ConfirmTrade, "Waiting for whitelisted trader items");
        return;
    }

    if (!preferredTrader) {
        ResetTradeSession();
        SetState(MuleState::WaitingForIncomingTrade, "Waiting for nearby whitelisted player");
        return;
    }

    if (m_tradePartnerId != preferredTrader->GetID()) {
        ResetTradeSession();
        m_tradePartnerId = preferredTrader->GetID();
        m_tradeSessionTick = now;
        m_lastBagChangeTick = now;
    }

    if (now - m_lastTradeStartTick >= kTradeStartIntervalMs) {
        hero->StartTrade(m_tradePartnerId);
        m_lastTradeStartTick = now;
        if (HasIncomingTradeRequest() && GetIncomingTradeRequesterId() == m_tradePartnerId)
            ConsumeIncomingTradeRequest();
        SetState(MuleState::AcceptIncomingTrade, "Requesting trade with whitelisted player");
        return;
    }

    if (now - m_tradeSessionTick >= 1000 && now - m_lastTradeAcceptTick >= kTradeAcceptIntervalMs) {
        hero->AcceptTrade(m_tradePartnerId);
        m_lastTradeAcceptTick = now;
        SetState(MuleState::ConfirmTrade, "Confirming trade with whitelisted player");
        return;
    }

    if (m_lastBagChangeTick != 0 && now - m_lastBagChangeTick > kTradeSessionTimeoutMs) {
        ResetTradeSession();
        SetState(MuleState::WaitingForIncomingTrade, "Trade complete, waiting for next request");
        return;
    }

    SetState(MuleState::ConfirmTrade, "Waiting for whitelisted trader items");
}

void MulePlugin::RenderUI()
{
    MuleSettings& settings = GetMuleSettings();
    CEntityInfo* entityInfo = Game::GetEntityInfo();
    const bool hasIncomingRequest = HasIncomingTradeRequest();
    const OBJID requesterId = hasIncomingRequest ? GetIncomingTradeRequesterId() : 0;
    const char* requesterName = hasIncomingRequest ? GetIncomingTradeRequesterName() : "";
    const uint64_t requestState = entityInfo ? entityInfo->m_nTradeRequestState : GetLastTradeRequestRawState();

    if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enabled", &settings.enabled);
        ImGui::InputText("Whitelisted Players", settings.whitelistNames, IM_ARRAYSIZE(settings.whitelistNames));
        ImGui::TextDisabled("Automatically travels to Market and settles near Warehouseman.");
        ImGui::TextDisabled("Accepts active incoming trade requests from nearby whitelisted players only.");
    }

    if (ImGui::CollapsingHeader("Runtime", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("State: %s", GetStateName());
        ImGui::Text("Status: %s", m_statusText);
        ImGui::Text("Map: %u", m_lastMapId);
        ImGui::Text("Hero Pos: (%d, %d)", m_lastHeroPos.x, m_lastHeroPos.y);
        ImGui::Text("Bag Items: %d / %d", (int)m_lastBagCount, CHero::MAX_BAG_ITEMS);
        ImGui::Text("Incoming Trade: %s", hasIncomingRequest ? "Yes" : "No");
        ImGui::Text("Trade Request State: %llu", (unsigned long long)requestState);
        if (requesterId != 0 || (requesterName && requesterName[0]))
            ImGui::Text("Requester: %s (%u)", requesterName && requesterName[0] ? requesterName : "<unknown>", requesterId);
        if (m_tradePartnerId != 0)
            ImGui::Text("Trade Partner: %u", m_tradePartnerId);
    }
}

bool MulePlugin::OnMapClick(const Position& tile)
{
    (void)tile;
    return false;
}
