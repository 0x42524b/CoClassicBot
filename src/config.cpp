#include "config.h"
#include "CHero.h"
#include "discord.h"
#include "game.h"
#include "pathfinder.h"
#include "plugins/aim_helper_plugin.h"
#include "plugins/mining_plugin.h"
#include "plugins/mule_plugin.h"
#include "plugins/follow_plugin.h"
#include "log.h"
#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

static MapSettings g_mapSettings;
MapSettings& GetMapSettings() { return g_mapSettings; }

static GuildSettings g_guildSettings;
GuildSettings& GetGuildSettings() { return g_guildSettings; }

static MiscSettings g_miscSettings;
MiscSettings& GetMiscSettings() { return g_miscSettings; }

static TravelSettings g_travelSettings;
TravelSettings& GetTravelSettings() { return g_travelSettings; }

static SkillTrainerSettings g_skillTrainerSettings;
SkillTrainerSettings& GetSkillTrainerSettings() { return g_skillTrainerSettings; }

static std::string g_activeCharacterKey;
static std::string g_lastObservedConfigSnapshot;
static std::string g_lastSavedConfigSnapshot;
static DWORD g_lastConfigChangeTick = 0;
static bool g_configAutosavePending = false;

static constexpr DWORD kConfigAutosaveDebounceMs = 500;

static std::string GetConfigDirectory()
{
    extern HMODULE g_hModule;  // from dllmain.cpp
    char buf[MAX_PATH];
    GetModuleFileNameA(g_hModule, buf, MAX_PATH);
    std::string path(buf);
    auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos)
        path = path.substr(0, pos + 1);
    return path;
}

static std::string GetLegacyConfigPath()
{
    return GetConfigDirectory() + "coclassic.ini";
}

static std::string GetConfigPathForKey(const std::string& characterKey)
{
    if (characterKey.empty())
        return GetLegacyConfigPath();
    return GetConfigDirectory() + "coclassic_" + characterKey + ".ini";
}

static bool FileExists(const char* path)
{
    return path && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// Helper: write int/float to INI
static void WriteInt(const char* file, const char* section, const char* key, int val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    WritePrivateProfileStringA(section, key, buf, file);
}

static void WriteFloat(const char* file, const char* section, const char* key, float val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6f", val);
    WritePrivateProfileStringA(section, key, buf, file);
}

static int ReadInt(const char* file, const char* section, const char* key, int def)
{
    return GetPrivateProfileIntA(section, key, def, file);
}

static float ReadFloat(const char* file, const char* section, const char* key, float def)
{
    char buf[32];
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), file);
    if (buf[0] == '\0') return def;
    return strtof(buf, nullptr);
}

static void ReadString(const char* file, const char* section, const char* key,
                       const char* def, char* out, DWORD outSize)
{
    GetPrivateProfileStringA(section, key, def, out, outSize, file);
}

static std::string SerializePositions(const std::vector<Position>& positions)
{
    std::string value;
    for (size_t i = 0; i < positions.size(); ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d:%d", positions[i].x, positions[i].y);
        if (!value.empty())
            value += ';';
        value += buf;
    }
    return value;
}

static void ParsePositions(const char* text, std::vector<Position>& out)
{
    out.clear();
    if (!text || !text[0])
        return;

    const char* cursor = text;
    while (*cursor) {
        int x = 0;
        int y = 0;
        if (sscanf(cursor, "%d:%d", &x, &y) == 2)
            out.push_back({x, y});

        const char* next = strchr(cursor, ';');
        if (!next)
            break;
        cursor = next + 1;
    }
}

static std::string SerializeU32List(const std::vector<uint32_t>& values)
{
    std::string value;
    for (uint32_t entry : values) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u", entry);
        if (!value.empty())
            value += ',';
        value += buf;
    }
    return value;
}

static void ParseU32List(const char* text, std::vector<uint32_t>& out)
{
    out.clear();
    if (!text || !text[0])
        return;

    const char* cursor = text;
    while (*cursor) {
        char* end = nullptr;
        unsigned long value = strtoul(cursor, &end, 10);
        if (end != cursor)
            out.push_back((uint32_t)value);

        if (!end || *end == '\0')
            break;
        cursor = end + 1;
    }
}

static std::string SanitizeIniToken(const char* text)
{
    std::string token;
    if (!text)
        return token;

    while (*text) {
        const unsigned char ch = (unsigned char)*text++;
        if (std::isalnum(ch)) {
            token.push_back((char)ch);
        } else if (ch == ' ' || ch == '_' || ch == '-') {
            token.push_back('_');
        }
    }

    if (token.empty())
        token = "Hero";
    return token;
}

static std::string GetCharacterConfigKey(const CHero* hero)
{
    if (!hero || hero->GetID() == 0)
        return {};

    char buf[96];
    const std::string name = SanitizeIniToken(hero->GetName());
    snprintf(buf, sizeof(buf), "%s_%u", name.c_str(), hero->GetID());
    return buf;
}

static std::string ResolveLoadConfigPath(const std::string& characterKey)
{
    const std::string characterPath = GetConfigPathForKey(characterKey);
    if (!characterKey.empty() && FileExists(characterPath.c_str()))
        return characterPath;

    const std::string legacyPath = GetLegacyConfigPath();
    if (FileExists(legacyPath.c_str()))
        return legacyPath;

    return characterPath;
}

static void AppendBoolSnapshot(std::string& snapshot, const char* key, bool value)
{
    snapshot += key;
    snapshot += '=';
    snapshot += value ? '1' : '0';
    snapshot += '\n';
}

static void AppendIntSnapshot(std::string& snapshot, const char* key, int value)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s=%d\n", key, value);
    snapshot += buf;
}

static void AppendFloatSnapshot(std::string& snapshot, const char* key, float value)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s=%.6f\n", key, value);
    snapshot += buf;
}

static void AppendStringSnapshot(std::string& snapshot, const char* key, const char* value)
{
    snapshot += key;
    snapshot += '=';
    if (value)
        snapshot += value;
    snapshot += '\n';
}

static std::string BuildCurrentConfigSnapshot()
{
    std::string snapshot;
    snapshot.reserve(8192);

    snapshot += "[Meta]\n";
    AppendStringSnapshot(snapshot, "characterKey", g_activeCharacterKey.c_str());

    const AimHelperSettings& aim = GetAimSettings();
    snapshot += "[AimHelper]\n";
    AppendBoolSnapshot(snapshot, "enabled", aim.enabled);
    AppendBoolSnapshot(snapshot, "showPlayers", aim.showPlayers);
    AppendBoolSnapshot(snapshot, "showMonsters", aim.showMonsters);
    AppendBoolSnapshot(snapshot, "ignoreGuild", aim.ignoreGuild);
    AppendIntSnapshot(snapshot, "markerSize", aim.markerSize);
    AppendIntSnapshot(snapshot, "markerThickness", aim.markerThickness);
    AppendFloatSnapshot(snapshot, "colorR", aim.color[0]);
    AppendFloatSnapshot(snapshot, "colorG", aim.color[1]);
    AppendFloatSnapshot(snapshot, "colorB", aim.color[2]);
    AppendFloatSnapshot(snapshot, "colorA", aim.color[3]);

    const MapSettings& map = GetMapSettings();
    snapshot += "[Map]\n";
    AppendFloatSnapshot(snapshot, "cellSize", map.cellSize);
    AppendBoolSnapshot(snapshot, "showEntities", map.showEntities);
    AppendBoolSnapshot(snapshot, "followHero", map.followHero);

    const GuildSettings& guild = GetGuildSettings();
    snapshot += "[Guild]\n";
    AppendBoolSnapshot(snapshot, "showDeadOnly", guild.showDeadOnly);
    AppendStringSnapshot(snapshot, "guildWhitelist", guild.guildWhitelist);

    const MiningSettings& mining = GetMiningSettings();
    snapshot += "[Mining]\n";
    AppendBoolSnapshot(snapshot, "enabled", mining.enabled);
    AppendBoolSnapshot(snapshot, "autoReviveInTown", mining.autoReviveInTown);
    AppendBoolSnapshot(snapshot, "useTwinCityWarehouse", mining.useTwinCityWarehouse);
    AppendBoolSnapshot(snapshot, "useTwinCityGate", mining.useTwinCityGate);
    AppendBoolSnapshot(snapshot, "buyTwinCityGates", mining.buyTwinCityGates);
    AppendBoolSnapshot(snapshot, "tradeReturnItemsToMule", mining.tradeReturnItemsToMule);
    AppendIntSnapshot(snapshot, "twinCityGateTargetCount", mining.twinCityGateTargetCount);
    AppendIntSnapshot(snapshot, "dropItemThreshold", mining.dropItemThreshold);
    AppendIntSnapshot(snapshot, "townBagThreshold", mining.townBagThreshold);
    AppendIntSnapshot(snapshot, "movementIntervalMs", mining.movementIntervalMs);
    AppendIntSnapshot(snapshot, "mineMapId", mining.mineMapId);
    AppendIntSnapshot(snapshot, "minePosX", mining.minePos.x);
    AppendIntSnapshot(snapshot, "minePosY", mining.minePos.y);
    AppendStringSnapshot(snapshot, "muleName", mining.muleName);
    AppendStringSnapshot(snapshot, "returnItemIds", SerializeU32List(mining.returnItemIds).c_str());
    AppendStringSnapshot(snapshot, "depositItemIds", SerializeU32List(mining.depositItemIds).c_str());
    AppendStringSnapshot(snapshot, "sellItemIds", SerializeU32List(mining.sellItemIds).c_str());
    AppendStringSnapshot(snapshot, "dropItemIds", SerializeU32List(mining.dropItemIds).c_str());

    const MuleSettings& mule = GetMuleSettings();
    snapshot += "[Mule]\n";
    AppendBoolSnapshot(snapshot, "enabled", mule.enabled);
    AppendStringSnapshot(snapshot, "whitelistNames", mule.whitelistNames);

    const FollowSettings& follow = GetFollowSettings();
    snapshot += "[Follow]\n";
    AppendBoolSnapshot(snapshot, "enabled", follow.enabled);
    AppendStringSnapshot(snapshot, "targetName", follow.targetName);
    AppendIntSnapshot(snapshot, "followDistance", follow.followDistance);
    AppendIntSnapshot(snapshot, "dodgeRadius", follow.dodgeRadius);

    const TravelSettings& travel = GetTravelSettings();
    snapshot += "[Travel]\n";
    AppendBoolSnapshot(snapshot, "usePacketJump", travel.usePacketJump);

    const SkillTrainerSettings& trainer = GetSkillTrainerSettings();
    snapshot += "[SkillTrainer]\n";
    AppendIntSnapshot(snapshot, "castDelayMs", trainer.castDelayMs);
    AppendBoolSnapshot(snapshot, "autoMpPotion", trainer.autoMpPotion);
    AppendIntSnapshot(snapshot, "selectedSkillId", static_cast<int>(trainer.selectedSkillId));

    const DiscordSettings& discord = GetDiscordSettings();
    snapshot += "[Discord]\n";
    AppendBoolSnapshot(snapshot, "webhookEnabled", discord.webhookEnabled);
    AppendStringSnapshot(snapshot, "webhookUrl", discord.webhookUrl);
    AppendStringSnapshot(snapshot, "mentionUserId", discord.mentionUserId);

    const MiscSettings& misc = GetMiscSettings();
    snapshot += "[Misc]\n";
    AppendBoolSnapshot(snapshot, "whisperNotifyEnabled", misc.whisperNotifyEnabled);
    AppendBoolSnapshot(snapshot, "itemNotifyEnabled", misc.itemNotifyEnabled);
    AppendBoolSnapshot(snapshot, "lootDropNotifyEnabled", misc.lootDropNotifyEnabled);
    AppendStringSnapshot(snapshot, "notifyItemIds", SerializeU32List(misc.notifyItemIds).c_str());
    AppendStringSnapshot(snapshot, "mentionItemIds", SerializeU32List(misc.mentionItemIds).c_str());

    return snapshot;
}

static void ResetConfigAutosaveState()
{
    const std::string snapshot = BuildCurrentConfigSnapshot();
    g_lastObservedConfigSnapshot = snapshot;
    g_lastSavedConfigSnapshot = snapshot;
    g_lastConfigChangeTick = 0;
    g_configAutosavePending = false;
}

static void SaveMiningSection(const char* file, const char* section)
{
    MiningSettings& mining = GetMiningSettings();
    WriteInt(file, section, "enabled", mining.enabled ? 1 : 0);
    WriteInt(file, section, "autoReviveInTown", mining.autoReviveInTown ? 1 : 0);
    WriteInt(file, section, "useTwinCityWarehouse", mining.useTwinCityWarehouse ? 1 : 0);
    WriteInt(file, section, "useTwinCityGate", mining.useTwinCityGate ? 1 : 0);
    WriteInt(file, section, "buyTwinCityGates", mining.buyTwinCityGates ? 1 : 0);
    WriteInt(file, section, "tradeReturnItemsToMule", mining.tradeReturnItemsToMule ? 1 : 0);
    WriteInt(file, section, "twinCityGateTargetCount", mining.twinCityGateTargetCount);
    WriteInt(file, section, "dropItemThreshold", mining.dropItemThreshold);
    WriteInt(file, section, "townBagThreshold", mining.townBagThreshold);
    WriteInt(file, section, "movementIntervalMs", mining.movementIntervalMs);
    WriteInt(file, section, "mineMapId", mining.mineMapId);
    WriteInt(file, section, "minePosX", mining.minePos.x);
    WriteInt(file, section, "minePosY", mining.minePos.y);
    WritePrivateProfileStringA(section, "muleName", mining.muleName, file);
    const std::string miningReturnIds = SerializeU32List(mining.returnItemIds);
    WritePrivateProfileStringA(section, "returnItemIds", miningReturnIds.c_str(), file);
    const std::string miningDepositIds = SerializeU32List(mining.depositItemIds);
    WritePrivateProfileStringA(section, "depositItemIds", miningDepositIds.c_str(), file);
    const std::string miningSellIds = SerializeU32List(mining.sellItemIds);
    WritePrivateProfileStringA(section, "sellItemIds", miningSellIds.c_str(), file);
    const std::string miningDropIds = SerializeU32List(mining.dropItemIds);
    WritePrivateProfileStringA(section, "dropItemIds", miningDropIds.c_str(), file);
}

static void LoadMiningSection(const char* file, const char* section)
{
    MiningSettings& mining = GetMiningSettings();
    mining = MiningSettings{};
    mining.enabled = ReadInt(file, section, "enabled", 0) != 0;
    mining.autoReviveInTown = ReadInt(file, section, "autoReviveInTown", 1) != 0;
    mining.useTwinCityWarehouse = ReadInt(file, section, "useTwinCityWarehouse", 0) != 0;
    mining.useTwinCityGate = ReadInt(file, section, "useTwinCityGate", 0) != 0;
    mining.buyTwinCityGates = ReadInt(file, section, "buyTwinCityGates", 0) != 0;
    mining.tradeReturnItemsToMule = ReadInt(file, section, "tradeReturnItemsToMule", 0) != 0;
    mining.twinCityGateTargetCount = ReadInt(file, section, "twinCityGateTargetCount", 1);
    if (mining.twinCityGateTargetCount < 1)
        mining.twinCityGateTargetCount = 1;
    mining.dropItemThreshold = ReadInt(file, section, "dropItemThreshold", 36);
    if (mining.dropItemThreshold < 1)
        mining.dropItemThreshold = 1;
    if (mining.dropItemThreshold > CHero::MAX_BAG_ITEMS)
        mining.dropItemThreshold = CHero::MAX_BAG_ITEMS;
    mining.townBagThreshold = ReadInt(file, section, "townBagThreshold", 0);
    if (mining.townBagThreshold < 0)
        mining.townBagThreshold = 0;
    if (mining.townBagThreshold > CHero::MAX_BAG_ITEMS)
        mining.townBagThreshold = CHero::MAX_BAG_ITEMS;
    mining.movementIntervalMs = ReadInt(file, section, "movementIntervalMs", 900);
    if (mining.movementIntervalMs < 100)
        mining.movementIntervalMs = 100;
    if (mining.movementIntervalMs > 5000)
        mining.movementIntervalMs = 5000;
    mining.mineMapId = ReadInt(file, section, "mineMapId", 0);
    mining.minePos.x = ReadInt(file, section, "minePosX", 0);
    mining.minePos.y = ReadInt(file, section, "minePosY", 0);
    ReadString(file, section, "muleName", "", mining.muleName, sizeof(mining.muleName));
    char miningReturnBuf[4096] = {};
    ReadString(file, section, "returnItemIds", "", miningReturnBuf, sizeof(miningReturnBuf));
    ParseU32List(miningReturnBuf, mining.returnItemIds);
    char miningDepositBuf[4096] = {};
    ReadString(file, section, "depositItemIds", "__MISSING__", miningDepositBuf, sizeof(miningDepositBuf));
    if (strcmp(miningDepositBuf, "__MISSING__") == 0) {
        mining.depositItemIds = mining.returnItemIds;
    } else {
        ParseU32List(miningDepositBuf, mining.depositItemIds);
    }
    char miningSellBuf[4096] = {};
    ReadString(file, section, "sellItemIds", "", miningSellBuf, sizeof(miningSellBuf));
    ParseU32List(miningSellBuf, mining.sellItemIds);
    char miningDropBuf[4096] = {};
    ReadString(file, section, "dropItemIds", "", miningDropBuf, sizeof(miningDropBuf));
    ParseU32List(miningDropBuf, mining.dropItemIds);
}

static void SaveMuleSection(const char* file, const char* section)
{
    MuleSettings& mule = GetMuleSettings();
    WriteInt(file, section, "enabled", mule.enabled ? 1 : 0);
    WritePrivateProfileStringA(section, "whitelistNames", mule.whitelistNames, file);
}

static void LoadMuleSection(const char* file, const char* section)
{
    MuleSettings& mule = GetMuleSettings();
    mule = MuleSettings{};
    mule.enabled = ReadInt(file, section, "enabled", 0) != 0;
    ReadString(file, section, "whitelistNames", "", mule.whitelistNames, sizeof(mule.whitelistNames));
}

static void SaveFollowSection(const char* file, const char* section)
{
    FollowSettings& follow = GetFollowSettings();
    WriteInt(file, section, "enabled", follow.enabled ? 1 : 0);
    WritePrivateProfileStringA(section, "targetName", follow.targetName, file);
    WriteInt(file, section, "followDistance", follow.followDistance);
    WriteInt(file, section, "dodgeRadius", follow.dodgeRadius);
}

static void LoadFollowSection(const char* file, const char* section)
{
    FollowSettings& follow = GetFollowSettings();
    follow = FollowSettings{};
    follow.enabled = ReadInt(file, section, "enabled", 0) != 0;
    ReadString(file, section, "targetName", "", follow.targetName, sizeof(follow.targetName));
    follow.followDistance = ReadInt(file, section, "followDistance", 3);
    if (follow.followDistance < 1) follow.followDistance = 1;
    if (follow.followDistance > 30) follow.followDistance = 30;
    follow.dodgeRadius = ReadInt(file, section, "dodgeRadius", 5);
    if (follow.dodgeRadius < 1) follow.dodgeRadius = 1;
    if (follow.dodgeRadius > 15) follow.dodgeRadius = 15;
}

static void SaveTravelSection(const char* file, const char* section)
{
    TravelSettings& travel = GetTravelSettings();
    WriteInt(file, section, "usePacketJump", travel.usePacketJump ? 1 : 0);
}

static void LoadTravelSection(const char* file, const char* section)
{
    TravelSettings& travel = GetTravelSettings();
    travel = TravelSettings{};
    travel.usePacketJump = ReadInt(file, section, "usePacketJump", 0) != 0;
}

static void SaveSkillTrainerSection(const char* file, const char* section)
{
    SkillTrainerSettings& trainer = GetSkillTrainerSettings();
    WriteInt(file, section, "castDelayMs", trainer.castDelayMs);
    WriteInt(file, section, "autoMpPotion", trainer.autoMpPotion ? 1 : 0);
    WriteInt(file, section, "selectedSkillId", static_cast<int>(trainer.selectedSkillId));
}

static void LoadSkillTrainerSection(const char* file, const char* section)
{
    SkillTrainerSettings& trainer = GetSkillTrainerSettings();
    trainer = SkillTrainerSettings{};
    trainer.castDelayMs = ReadInt(file, section, "castDelayMs", 1000);
    if (trainer.castDelayMs < 100) trainer.castDelayMs = 100;
    if (trainer.castDelayMs > 10000) trainer.castDelayMs = 10000;
    trainer.autoMpPotion = ReadInt(file, section, "autoMpPotion", 0) != 0;
    trainer.selectedSkillId = static_cast<uint32_t>(ReadInt(file, section, "selectedSkillId", 0));
}

static void SaveSharedSections(const char* file)
{
    AimHelperSettings& aim = GetAimSettings();
    WriteInt(file, "AimHelper", "enabled", aim.enabled ? 1 : 0);
    WriteInt(file, "AimHelper", "showPlayers", aim.showPlayers ? 1 : 0);
    WriteInt(file, "AimHelper", "showMonsters", aim.showMonsters ? 1 : 0);
    WriteInt(file, "AimHelper", "ignoreGuild", aim.ignoreGuild ? 1 : 0);
    WriteInt(file, "AimHelper", "markerSize", aim.markerSize);
    WriteInt(file, "AimHelper", "markerThickness", aim.markerThickness);
    WriteFloat(file, "AimHelper", "colorR", aim.color[0]);
    WriteFloat(file, "AimHelper", "colorG", aim.color[1]);
    WriteFloat(file, "AimHelper", "colorB", aim.color[2]);
    WriteFloat(file, "AimHelper", "colorA", aim.color[3]);

    MapSettings& map = GetMapSettings();
    WriteFloat(file, "Map", "cellSize", map.cellSize);
    WriteInt(file, "Map", "showEntities", map.showEntities ? 1 : 0);
    WriteInt(file, "Map", "followHero", map.followHero ? 1 : 0);

    auto& pf = Pathfinder::Get();
    WriteInt(file, "Travel", "avoidMobs", pf.GetAvoidMobs() ? 1 : 0);
    WriteInt(file, "Travel", "avoidMobRadius", pf.GetAvoidMobRadius());

    GuildSettings& guild = GetGuildSettings();
    WriteInt(file, "Guild", "ShowDeadOnly", guild.showDeadOnly ? 1 : 0);
    WritePrivateProfileStringA("Guild", "guildWhitelist", guild.guildWhitelist, file);

    DiscordSettings& discord = GetDiscordSettings();
    WriteInt(file, "Discord", "webhookEnabled", discord.webhookEnabled ? 1 : 0);
    WritePrivateProfileStringA("Discord", "webhookUrl", discord.webhookUrl, file);
    WritePrivateProfileStringA("Discord", "mentionUserId", discord.mentionUserId, file);

    MiscSettings& misc = GetMiscSettings();
    WriteInt(file, "Misc", "whisperNotifyEnabled", misc.whisperNotifyEnabled ? 1 : 0);
    WriteInt(file, "Misc", "itemNotifyEnabled", misc.itemNotifyEnabled ? 1 : 0);
    WriteInt(file, "Misc", "lootDropNotifyEnabled", misc.lootDropNotifyEnabled ? 1 : 0);
    const std::string miscNotifyIds = SerializeU32List(misc.notifyItemIds);
    WritePrivateProfileStringA("Misc", "notifyItemIds", miscNotifyIds.c_str(), file);
    const std::string miscMentionIds = SerializeU32List(misc.mentionItemIds);
    WritePrivateProfileStringA("Misc", "mentionItemIds", miscMentionIds.c_str(), file);
}

static void LoadSharedSections(const char* file)
{
    AimHelperSettings& aim = GetAimSettings();
    aim = AimHelperSettings{};
    aim.enabled = ReadInt(file, "AimHelper", "enabled", 0) != 0;
    aim.showPlayers = ReadInt(file, "AimHelper", "showPlayers", 1) != 0;
    aim.showMonsters = ReadInt(file, "AimHelper", "showMonsters", 0) != 0;
    aim.ignoreGuild = ReadInt(file, "AimHelper", "ignoreGuild", 0) != 0;
    aim.markerSize = ReadInt(file, "AimHelper", "markerSize", 8);
    aim.markerThickness = ReadInt(file, "AimHelper", "markerThickness", 2);
    aim.color[0] = ReadFloat(file, "AimHelper", "colorR", 1.0f);
    aim.color[1] = ReadFloat(file, "AimHelper", "colorG", 0.0f);
    aim.color[2] = ReadFloat(file, "AimHelper", "colorB", 0.0f);
    aim.color[3] = ReadFloat(file, "AimHelper", "colorA", 1.0f);

    MapSettings& map = GetMapSettings();
    map = MapSettings{};
    map.cellSize = ReadFloat(file, "Map", "cellSize", 3.0f);
    map.showEntities = ReadInt(file, "Map", "showEntities", 1) != 0;
    map.followHero = ReadInt(file, "Map", "followHero", 1) != 0;

    auto& pf = Pathfinder::Get();
    pf.SetAvoidMobs(ReadInt(file, "Travel", "avoidMobs", 0) != 0);
    int avoidRadius = ReadInt(file, "Travel", "avoidMobRadius", 5);
    if (avoidRadius < 1) avoidRadius = 1;
    if (avoidRadius > 10) avoidRadius = 10;
    pf.SetAvoidMobRadius(avoidRadius);

    GuildSettings& guild = GetGuildSettings();
    guild = GuildSettings{};
    guild.showDeadOnly = ReadInt(file, "Guild", "ShowDeadOnly", 0) != 0;
    ReadString(file, "Guild", "guildWhitelist", "", guild.guildWhitelist, sizeof(guild.guildWhitelist));

    DiscordSettings& discord = GetDiscordSettings();
    discord = DiscordSettings{};
    discord.webhookEnabled = ReadInt(file, "Discord", "webhookEnabled", 0) != 0;
    ReadString(file, "Discord", "webhookUrl", "", discord.webhookUrl, sizeof(discord.webhookUrl));
    ReadString(file, "Discord", "mentionUserId", "", discord.mentionUserId, sizeof(discord.mentionUserId));

    MiscSettings& misc = GetMiscSettings();
    misc = MiscSettings{};
    misc.whisperNotifyEnabled = ReadInt(file, "Misc", "whisperNotifyEnabled", 0) != 0;
    misc.itemNotifyEnabled = ReadInt(file, "Misc", "itemNotifyEnabled", 0) != 0;
    misc.lootDropNotifyEnabled = ReadInt(file, "Misc", "lootDropNotifyEnabled", 0) != 0;
    char miscNotifyBuf[4096] = {};
    ReadString(file, "Misc", "notifyItemIds", "", miscNotifyBuf, sizeof(miscNotifyBuf));
    ParseU32List(miscNotifyBuf, misc.notifyItemIds);
    char miscMentionBuf[4096] = {};
    ReadString(file, "Misc", "mentionItemIds", "", miscMentionBuf, sizeof(miscMentionBuf));
    ParseU32List(miscMentionBuf, misc.mentionItemIds);
}

static std::string SaveCharacterConfigForKey(const std::string& characterKey)
{
    const std::string path = GetConfigPathForKey(characterKey);
    const char* file = path.c_str();
    SaveSharedSections(file);
    SaveMiningSection(file, "Mining");
    SaveMuleSection(file, "Mule");
    SaveFollowSection(file, "Follow");
    SaveTravelSection(file, "Travel");
    SaveSkillTrainerSection(file, "SkillTrainer");
    return path;
}

static std::string LoadCharacterConfigForKey(const std::string& characterKey)
{
    const std::string path = ResolveLoadConfigPath(characterKey);
    const char* file = path.c_str();
    LoadSharedSections(file);
    LoadMiningSection(file, "Mining");
    LoadMuleSection(file, "Mule");
    LoadFollowSection(file, "Follow");
    LoadTravelSection(file, "Travel");
    LoadSkillTrainerSection(file, "SkillTrainer");
    return path;
}

void SaveConfig()
{
    std::string characterKey = GetCharacterConfigKey(Game::GetHero());
    if (characterKey.empty())
        characterKey = g_activeCharacterKey;
    const std::string path = SaveCharacterConfigForKey(characterKey);
    if (!characterKey.empty())
        g_activeCharacterKey = characterKey;

    const std::string snapshot = BuildCurrentConfigSnapshot();
    g_lastObservedConfigSnapshot = snapshot;
    g_lastSavedConfigSnapshot = snapshot;
    g_lastConfigChangeTick = 0;
    g_configAutosavePending = false;

    spdlog::info("[config] Saved to {}", path);
}

void LoadConfig()
{
    g_activeCharacterKey = GetCharacterConfigKey(Game::GetHero());
    const std::string path = LoadCharacterConfigForKey(g_activeCharacterKey);
    ResetConfigAutosaveState();
    if (!FileExists(path.c_str()))
        spdlog::warn("[config] No config file found, using defaults");

    spdlog::info("[config] Loaded from {}", path);
}

void MaybeAutoSaveConfig()
{
    const std::string snapshot = BuildCurrentConfigSnapshot();
    if (g_lastObservedConfigSnapshot.empty() && g_lastSavedConfigSnapshot.empty()) {
        g_lastObservedConfigSnapshot = snapshot;
        g_lastSavedConfigSnapshot = snapshot;
        return;
    }

    if (snapshot != g_lastObservedConfigSnapshot) {
        g_lastObservedConfigSnapshot = snapshot;
        g_lastConfigChangeTick = GetTickCount();
        g_configAutosavePending = true;
    }

    if (!g_configAutosavePending)
        return;

    const DWORD now = GetTickCount();
    if (now - g_lastConfigChangeTick < kConfigAutosaveDebounceMs)
        return;

    if (snapshot == g_lastSavedConfigSnapshot) {
        g_configAutosavePending = false;
        return;
    }

    SaveConfig();
}

void UpdateCharacterConfigBinding()
{
    const std::string newCharacterKey = GetCharacterConfigKey(Game::GetHero());
    if (newCharacterKey.empty() || newCharacterKey == g_activeCharacterKey)
        return;

    if (!g_activeCharacterKey.empty())
        SaveCharacterConfigForKey(g_activeCharacterKey);

    const std::string path = LoadCharacterConfigForKey(newCharacterKey);
    g_activeCharacterKey = newCharacterKey;
    ResetConfigAutosaveState();
    spdlog::info("[config] Switched character profile to {} ({})", g_activeCharacterKey, path);
}
