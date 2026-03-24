#include "revive_helper_plugin.h"
#include "config.h"
#include "game.h"
#include "CRole.h"
#include "CHero.h"
#include "CEntitySet.h"
#include "imgui.h"
#include <sstream>
#include <vector>
#include <string>

static std::vector<std::string> ParseGuildNames(const char* text)
{
    std::vector<std::string> result;
    std::istringstream ss(text);
    std::string token;
    while (std::getline(ss, token, ',')) {
        size_t start = token.find_first_not_of(' ');
        size_t end = token.find_last_not_of(' ');
        if (start != std::string::npos)
            result.push_back(token.substr(start, end - start + 1));
    }
    return result;
}

void ReviveHelperPlugin::RenderUI()
{
    GuildSettings& guild = GetGuildSettings();
    if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Show Dead Members Only", &guild.showDeadOnly);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Filter entity list and minimap to only show\ndead/ghost guild members (revive helper)");
        ImGui::InputText("Guild Whitelist", guild.guildWhitelist, IM_ARRAYSIZE(guild.guildWhitelist));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Comma-separated guild names whose dead members\nwill also be shown (in addition to your own guild)");
    }
}

bool ReviveHelperPlugin::OnPreRenderEntity(CRole* entity)
{
    GuildSettings& guild = GetGuildSettings();
    if (!guild.showDeadOnly) return true;

    CHero* h = Game::GetHero();
    if (!h || entity->GetID() == h->GetID())
        return true;

    if (!entity->IsPlayer())
        return false;

    if (!(entity->IsDead() || entity->TestState(USERSTATUS_GHOST)))
        return false;

    // Own guild — always show dead members
    if (h->HasSyndicate() && entity->m_idSyndicate == h->m_idSyndicate)
        return true;

    // Check whitelisted guilds
    if (guild.guildWhitelist[0] != '\0' && entity->m_idSyndicate != 0) {
        CEntitySet* es = CEntitySet::GetInstance();
        if (es) {
            const char* synName = es->GetSyndicateName(entity->m_idSyndicate);
            if (synName && synName[0] != '\0') {
                for (const std::string& name : ParseGuildNames(guild.guildWhitelist)) {
                    if (_stricmp(name.c_str(), synName) == 0)
                        return true;
                }
            }
        }
    }

    return false;
}
