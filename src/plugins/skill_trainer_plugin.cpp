#include "skill_trainer_plugin.h"
#include "CHero.h"
#include "game.h"
#include "config.h"
#include "inventory_utils.h"
#include "itemtype.h"
#include "log.h"
#include "imgui.h"
#include <cstdio>

static constexpr DWORD kPotionCooldownMs = 1000;

CMagic* SkillTrainerPlugin::GetSelectedSkill() const
{
    const SkillTrainerSettings& settings = GetSkillTrainerSettings();
    if (settings.selectedSkillId == 0)
        return nullptr;
    CHero* hero = Game::GetHero();
    if (!hero)
        return nullptr;
    return hero->FindMagicById(settings.selectedSkillId);
}

void SkillTrainerPlugin::StartTraining()
{
    CMagic* skill = GetSelectedSkill();
    if (!skill) {
        snprintf(m_statusText, sizeof(m_statusText), "No skill selected");
        return;
    }
    m_state = SkillTrainerState::Casting;
    m_lastCastTick = 0;
    m_lastPotionTick = 0;
    snprintf(m_statusText, sizeof(m_statusText), "Training %s", skill->GetName());
    spdlog::info("[skill_trainer] Started training {}", skill->GetName());
}

void SkillTrainerPlugin::StopTraining(const char* reason)
{
    m_state = SkillTrainerState::Idle;
    snprintf(m_statusText, sizeof(m_statusText), "Stopped: %s", reason);
    spdlog::info("[skill_trainer] Stopped: {}", reason);
}

bool SkillTrainerPlugin::TryCastSkill()
{
    CHero* hero = Game::GetHero();
    CMagic* skill = GetSelectedSkill();
    if (!hero || !skill)
        return false;

    hero->MagicAttack(skill->GetMagicType(), hero->GetID(), hero->m_posMap);
    m_lastCastTick = GetTickCount();
    snprintf(m_statusText, sizeof(m_statusText), "Casting %s (Lv%u, Exp %u/%u)",
        skill->GetName(), skill->GetLevel(), skill->GetExp(), skill->GetExpRequired());
    return true;
}

bool SkillTrainerPlugin::TryUsePotion()
{
    CHero* hero = Game::GetHero();
    if (!hero)
        return false;

    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef)
            continue;
        const ItemTypeInfo* info = GetItemTypeInfo(itemRef->GetTypeID());
        if (!info || !IsConsumablePotionType(*info, true))
            continue;
        if (info->mana == 0)
            continue;
        hero->UseItem(itemRef->GetID());
        m_lastPotionTick = GetTickCount();
        return true;
    }
    return false;
}

void SkillTrainerPlugin::Update()
{
    if (m_state == SkillTrainerState::Idle)
        return;

    CHero* hero = Game::GetHero();
    if (!hero) {
        StopTraining("no hero");
        return;
    }

    if (hero->IsDead()) {
        StopTraining("hero died");
        return;
    }

    if (hero->IsMagicShieldActive()) {
        snprintf(m_statusText, sizeof(m_statusText), "Waiting (shielded)");
        return;
    }

    CMagic* skill = GetSelectedSkill();
    if (!skill) {
        StopTraining("skill not found");
        return;
    }

    if (skill->GetExpRequired() > 0 && skill->GetExp() >= skill->GetExpRequired()) {
        StopTraining("skill at max level");
        return;
    }

    const SkillTrainerSettings& settings = GetSkillTrainerSettings();
    const DWORD now = GetTickCount();

    switch (m_state) {
    case SkillTrainerState::Casting: {
        if (skill->GetStaminaCost() > 0 &&
            hero->GetStamina() < static_cast<int>(skill->GetStaminaCost())) {
            hero->Sit();
            m_state = SkillTrainerState::Sitting;
            snprintf(m_statusText, sizeof(m_statusText), "Sitting (stamina %d/%d)",
                hero->GetStamina(), hero->GetMaxStamina());
            return;
        }

        if (skill->GetMpCost() > 0 &&
            hero->GetCurrentMana() < static_cast<int>(skill->GetMpCost())) {
            if (settings.autoMpPotion) {
                m_state = SkillTrainerState::UsingPotion;
                return;
            }
            StopTraining("not enough mana");
            return;
        }

        if (now - m_lastCastTick < static_cast<DWORD>(settings.castDelayMs))
            return;

        TryCastSkill();
        break;
    }

    case SkillTrainerState::Sitting: {
        snprintf(m_statusText, sizeof(m_statusText), "Sitting (stamina %d/%d)",
            hero->GetStamina(), hero->GetMaxStamina());

        if (hero->GetStamina() >= hero->GetMaxStamina()) {
            m_state = SkillTrainerState::Casting;
            snprintf(m_statusText, sizeof(m_statusText), "Resuming training %s",
                skill->GetName());
        }
        break;
    }

    case SkillTrainerState::UsingPotion: {
        if (now - m_lastPotionTick < kPotionCooldownMs)
            return;

        if (hero->GetCurrentMana() >= static_cast<int>(skill->GetMpCost())) {
            m_state = SkillTrainerState::Casting;
            return;
        }

        if (!TryUsePotion()) {
            StopTraining("no MP potions left");
            return;
        }
        snprintf(m_statusText, sizeof(m_statusText), "Using MP potion (mana %d)",
            hero->GetCurrentMana());
        break;
    }

    default:
        break;
    }
}

void SkillTrainerPlugin::RenderUI()
{
    SkillTrainerSettings& settings = GetSkillTrainerSettings();
    CHero* hero = Game::GetHero();

    ImGui::Text("Skill:");
    ImGui::SameLine();

    const char* previewName = "None";
    CMagic* selectedSkill = GetSelectedSkill();
    if (selectedSkill)
        previewName = selectedSkill->GetName();

    if (ImGui::BeginCombo("##skill_select", previewName)) {
        if (ImGui::Selectable("None", settings.selectedSkillId == 0))
            settings.selectedSkillId = 0;

        if (hero) {
            for (const auto& magic : hero->m_vecMagic) {
                if (!magic || !magic->IsEnabled())
                    continue;
                char label[128];
                snprintf(label, sizeof(label), "%s (Lv%u, Exp %u/%u)",
                    magic->GetName(), magic->GetLevel(),
                    magic->GetExp(), magic->GetExpRequired());

                const bool isSelected = (settings.selectedSkillId == magic->GetMagicType());
                if (ImGui::Selectable(label, isSelected))
                    settings.selectedSkillId = magic->GetMagicType();
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    if (selectedSkill) {
        if (selectedSkill->GetMpCost() > 0)
            ImGui::Text("MP Cost: %u", selectedSkill->GetMpCost());
        if (selectedSkill->GetStaminaCost() > 0)
            ImGui::Text("Stamina Cost: %u", selectedSkill->GetStaminaCost());
    }

    ImGui::SliderInt("Cast Delay (ms)", &settings.castDelayMs, 100, 10000);
    ImGui::Checkbox("Auto MP Potion", &settings.autoMpPotion);

    ImGui::Separator();

    if (m_state == SkillTrainerState::Idle) {
        if (ImGui::Button("Start Training"))
            StartTraining();
    } else {
        if (ImGui::Button("Stop Training"))
            StopTraining("user stopped");
    }

    ImGui::SameLine();
    ImGui::TextColored(
        m_state == SkillTrainerState::Idle ? ImVec4(0.6f, 0.6f, 0.6f, 1.0f) : ImVec4(0.2f, 1.0f, 0.2f, 1.0f),
        "%s", m_statusText);

    if (hero && m_state != SkillTrainerState::Idle) {
        ImGui::Text("Mana: %d / %d", hero->GetCurrentMana(), hero->GetMaxMana());
        ImGui::Text("Stamina: %d / %d", hero->GetStamina(), hero->GetMaxStamina());
    }
}
