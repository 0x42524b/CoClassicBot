#include "plugin_mgr.h"
#include "travel_plugin.h"
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

        // ── Left sidebar ──
        ImGui::BeginChild("##plugin_sidebar", ImVec2(kSidebarWidth, 0), true);
        for (size_t i = 0; i < m_plugins.size(); i++) {
            if (ImGui::Selectable(m_plugins[i]->GetName(), m_selectedPlugin == (int)i))
                m_selectedPlugin = (int)i;
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // ── Right content ──
        ImGui::BeginChild("##plugin_content", ImVec2(0, 0), true);
        if (m_selectedPlugin >= 0 && m_selectedPlugin < (int)m_plugins.size()) {
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
