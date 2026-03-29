#include "plugin_mgr.h"
#include "travel_plugin.h"
#include "base_hunt_plugin.h"
#include "melee_hunt_plugin.h"
#include "archer_hunt_plugin.h"
#include "mining_plugin.h"
#include "mule_plugin.h"
#include "aim_helper_plugin.h"
#include "revive_helper_plugin.h"
#include "follow_plugin.h"
#include "artisan_spammer_plugin.h"
#include "skill_trainer_plugin.h"
#include "log.h"
#include "imgui.h"

PluginManager& PluginManager::Get()
{
    static PluginManager instance;
    return instance;
}

void PluginManager::Init()
{
    m_plugins.push_back(std::make_unique<MeleeHuntPlugin>());
    m_plugins.push_back(std::make_unique<ArcherHuntPlugin>());
    m_plugins.push_back(std::make_unique<MiningPlugin>());
    m_plugins.push_back(std::make_unique<MulePlugin>());
    m_plugins.push_back(std::make_unique<TravelPlugin>());
    m_plugins.push_back(std::make_unique<FollowPlugin>());
    m_plugins.push_back(std::make_unique<AimHelperPlugin>());
    m_plugins.push_back(std::make_unique<ReviveHelperPlugin>());
    m_plugins.push_back(std::make_unique<ArtisanSpammerPlugin>());
    m_plugins.push_back(std::make_unique<SkillTrainerPlugin>());
    spdlog::info("[plugins] Initialized {} plugins", m_plugins.size());
    for (auto& p : m_plugins)
        spdlog::info("[plugins]   - {}", p->GetName());
}

void PluginManager::UpdateAll()
{
    for (auto& p : m_plugins) {
        if (p->m_enabled)
            p->Update();
    }
}

void PluginManager::RenderAllUI()
{
    if (m_plugins.empty()) return;

    if (ImGui::BeginTabItem("Plugins")) {
        constexpr float kSidebarWidth = 120.0f;

        // Build sidebar entries — hunt plugins are merged into one "Hunting" entry
        // kHuntingIndex is a sentinel meaning "show the Hunting composite view"
        constexpr int kHuntingIndex = -2;

        // ── Left sidebar ──
        ImGui::BeginChild("##plugin_sidebar", ImVec2(kSidebarWidth, 0), true);
        bool huntEntryRendered = false;
        for (size_t i = 0; i < m_plugins.size(); i++) {
            auto& p = m_plugins[i];
            if (dynamic_cast<BaseHuntPlugin*>(p.get())) {
                if (huntEntryRendered)
                    continue;
                huntEntryRendered = true;
                if (ImGui::Selectable("Hunting", m_selectedPlugin == kHuntingIndex))
                    m_selectedPlugin = kHuntingIndex;
                continue;
            }

            if (ImGui::Selectable(p->GetName(), m_selectedPlugin == (int)i))
                m_selectedPlugin = (int)i;
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // ── Right content ──
        ImGui::BeginChild("##plugin_content", ImVec2(0, 0), true);
        if (m_selectedPlugin == kHuntingIndex) {
            // Hunting composite: shared settings, then sub-tabs for each mode
            // Use the enabled hunt plugin for general/settings UI (runtime stats, safety, etc.)
            BaseHuntPlugin* activeHunt = nullptr;
            BaseHuntPlugin* firstHunt = nullptr;
            for (auto& p : m_plugins) {
                if (auto* hunt = dynamic_cast<BaseHuntPlugin*>(p.get())) {
                    if (!firstHunt) firstHunt = hunt;
                    if (hunt->m_enabled) activeHunt = hunt;
                }
            }
            BaseHuntPlugin* uiHunt = activeHunt ? activeHunt : firstHunt;
            if (uiHunt)
                uiHunt->RenderGeneralUI();

            ImGui::Separator();
            if (ImGui::BeginTabBar("##huntmodetabs")) {
                for (auto& p : m_plugins) {
                    if (auto* hunt = dynamic_cast<BaseHuntPlugin*>(p.get())) {
                        if (ImGui::BeginTabItem(hunt->GetName())) {
                            hunt->RenderUI();
                            ImGui::EndTabItem();
                        }
                    }
                }
                ImGui::EndTabBar();
            }

            if (uiHunt)
                uiHunt->RenderSettingsUI();
        } else if (m_selectedPlugin >= 0 && m_selectedPlugin < (int)m_plugins.size()) {
            m_plugins[m_selectedPlugin]->RenderUI();
        }
        ImGui::EndChild();

        ImGui::EndTabItem();
    }
}

bool PluginManager::PreRenderEntity(CRole* entity)
{
    for (auto& p : m_plugins) {
        if (p->m_enabled && !p->OnPreRenderEntity(entity))
            return false;
    }
    return true;
}

void PluginManager::PostRenderEntity(CRole* entity)
{
    for (auto& p : m_plugins) {
        if (p->m_enabled)
            p->OnPostRenderEntity(entity);
    }
}

bool PluginManager::HandleMapClick(const Position& tile)
{
    for (auto& p : m_plugins) {
        if (p->OnMapClick(tile))
            return true;
    }
    return false;
}

void PluginManager::Shutdown()
{
    spdlog::info("[plugins] Shutting down {} plugins", m_plugins.size());
    m_plugins.clear();
}
