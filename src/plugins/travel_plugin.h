#pragma once
#include "plugin.h"
#include "base.h"
#include "gateway.h"
#include <vector>

class CHero;
class CGameMap;
class CRole;

enum class TravelState {
    Idle,
    PathfindToGateway,
    WaitArrival,
    UseGatewayAction,
    ActivateNpc,
    AnswerDialog,
    WaitMapChange,
    WaitMapStabilize,
    PathfindToFinalPos,
    WaitFinalArrival,
    Complete,
    Failed
};

class TravelPlugin : public IPlugin {
public:
    const char* GetName() const override { return "Travel"; }
    void Update() override;
    void RenderUI() override;

    void StartTravel(OBJID destMapId, Position destPos = {0, 0});
    void CancelTravel();
    bool IsTraveling() const;
    TravelState GetState() const { return m_state; }
    const char* GetStatusText() const { return m_statusText; }
    OBJID GetDestination() const { return m_destMapId; }

private:
    void SetState(TravelState state);
    void BeginFinalPathfind(CHero* hero, CGameMap* map);
    DWORD ElapsedMs() const;
    CRole* FindNpcNear(const char* npcName, const Position& expectedPos, int radius = 10);
    Position ResolveGatewayTargetPos(const Gateway& gw);
    bool TryProcessGatewayNpc(CHero* hero, const Gateway& gw);
    bool HasDestPos() const { return m_destPos.x != 0 || m_destPos.y != 0; }
    void StartPathAndTrack(const std::vector<Position>& waypoints);

    TravelState             m_state = TravelState::Idle;
    OBJID                   m_destMapId = 0;
    Position                m_destPos = {0, 0};
    std::vector<Gateway>    m_gatewayPath;
    size_t                  m_gatewayIndex = 0;
    DWORD                   m_stateStartTick = 0;
    int                     m_answerIndex = 0;
    DWORD                   m_lastAnswerTick = 0;
    char                    m_statusText[128] = "";
    uint32_t                m_pathGeneration = 0;
    Position                m_lastHeroPos = {0, 0};
    Position                m_gatewayApproachPos = {0, 0};
};
