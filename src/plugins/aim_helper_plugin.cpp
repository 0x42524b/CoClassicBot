#include "aim_helper_plugin.h"
#include "game.h"
#include "gfx.h"
#include "CRole.h"
#include "CHero.h"
#include "CGameMap.h"
#include "imgui.h"

static AimHelperSettings g_aimSettings;
AimHelperSettings& GetAimSettings() { return g_aimSettings; }

void AimHelperPlugin::RenderUI()
{
    AimHelperSettings& aim = GetAimSettings();
    if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enabled", &aim.enabled);
        ImGui::Checkbox("Show Players", &aim.showPlayers);
        ImGui::SameLine();
        ImGui::Checkbox("Show Monsters", &aim.showMonsters);
        ImGui::Checkbox("Ignore Guild Members", &aim.ignoreGuild);
        ImGui::SliderInt("Marker Size", &aim.markerSize, 1, 30);
        ImGui::SliderInt("Marker Thickness", &aim.markerThickness, 1, 8);
        ImGui::ColorEdit4("Marker Color", aim.color,
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
    }
}

void AimHelperPlugin::OnPostRenderEntity(CRole* entity)
{
    if (!g_aimSettings.enabled) return;

    bool isPlayer  = entity->IsPlayer();
    bool isMonster = entity->IsMonster();
    if (isPlayer  && !g_aimSettings.showPlayers)  return;
    if (isMonster && !g_aimSettings.showMonsters) return;
    if (!isPlayer && !isMonster) return;

    CHero* hero = Game::GetHero();
    if (hero && entity->GetID() == hero->GetID()) return;

    if (isPlayer && g_aimSettings.ignoreGuild
        && hero && hero->HasSyndicate()
        && entity->m_idSyndicate == hero->m_idSyndicate)
        return;

    CGameMap* map = Game::GetMap();
    if (!map) return;

    if (!entity->IsJumping()) return;

    const CCommand& cmd = entity->GetCommand();
    Position dest = cmd.posTarget;
    if (dest.x == entity->m_posMap.x && dest.y == entity->m_posMap.y) return;

    int destWorldX = (dest.x - dest.y) * 32 + map->m_posCameraPos.x;
    int destWorldY = (dest.x + dest.y) * 16 + map->m_posCameraPos.y;
    int screenX = destWorldX - map->m_posViewport.x;
    int screenY = destWorldY - map->m_posViewport.y;

    uint8_t r = (uint8_t)(g_aimSettings.color[0] * 255.0f);
    uint8_t g = (uint8_t)(g_aimSettings.color[1] * 255.0f);
    uint8_t b = (uint8_t)(g_aimSettings.color[2] * 255.0f);
    uint8_t a = (uint8_t)(g_aimSettings.color[3] * 255.0f);
    uint32_t argb = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

    Gfx::DrawCross(screenX, screenY,
                   g_aimSettings.markerSize,
                   g_aimSettings.markerThickness, argb);
}
