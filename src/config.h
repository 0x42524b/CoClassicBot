#pragma once
#include <cstdint>
#include <vector>

struct MapSettings
{
    float cellSize    = 1.0f;  // zoom multiplier (1.0 = auto-fit full map)
    bool showEntities = true;
    bool followHero   = true;
};

struct GuildSettings
{
    bool showDeadOnly = false;
    char guildWhitelist[256] = "";  // comma-separated guild names to also show dead members
};

struct MiscSettings
{
    bool whisperNotifyEnabled = false;
    bool itemNotifyEnabled = false;
    bool lootDropNotifyEnabled = false;
    std::vector<uint32_t> notifyItemIds;
    std::vector<uint32_t> mentionItemIds;  // subset of notifyItemIds that also @mention
};

struct TravelSettings
{
    bool usePacketJump = false;
};

struct SkillTrainerSettings
{
    int  castDelayMs = 1000;
    bool autoMpPotion = false;
    uint32_t selectedSkillId = 0;
};

MapSettings& GetMapSettings();
GuildSettings& GetGuildSettings();
MiscSettings& GetMiscSettings();
TravelSettings& GetTravelSettings();
SkillTrainerSettings& GetSkillTrainerSettings();

// Save/load all settings to/from a per-character coclassic_<name_uid>.ini next to the DLL.
// Falls back to coclassic.ini only when loading a character without its own file yet.
void LoadConfig();
void SaveConfig();
void MaybeAutoSaveConfig();
void UpdateCharacterConfigBinding();
