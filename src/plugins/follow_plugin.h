#pragma once
#include "plugin.h"

struct FollowSettings
{
    bool enabled            = false;
    char targetName[16]     = "";
    int  followDistance     = 3;
    int  dodgeRadius       = 5;
};

FollowSettings& GetFollowSettings();

class FollowPlugin : public IPlugin {
public:
    const char* GetName() const override { return "Follow"; }
    void Update() override;
    void RenderUI() override;

private:
    enum class State { Idle, Following, Dodging };

    CRole* FindTarget() const;
    int  NearestMobDistance() const;

    State m_state        = State::Idle;
    DWORD m_lastJumpTick = 0;
};
