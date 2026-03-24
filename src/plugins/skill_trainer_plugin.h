#pragma once
#include "plugin.h"
#include "base.h"

class CMagic;

enum class SkillTrainerState
{
    Idle,
    Casting,
    Sitting,
    UsingPotion
};

class SkillTrainerPlugin : public IPlugin {
public:
    const char* GetName() const override { return "Skill Trainer"; }
    void Update() override;
    void RenderUI() override;

private:
    void StartTraining();
    void StopTraining(const char* reason);
    bool TryCastSkill();
    bool TryUsePotion();
    CMagic* GetSelectedSkill() const;

    SkillTrainerState m_state = SkillTrainerState::Idle;
    DWORD m_lastCastTick = 0;
    DWORD m_lastPotionTick = 0;
    char  m_statusText[128] = "Idle";
};
