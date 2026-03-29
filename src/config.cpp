#include "config.h"
#include "CHero.h"
#include "discord.h"
#include "game.h"
#include "pathfinder.h"
#include "plugins/aim_helper_plugin.h"
#include "hunt_settings.h"
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

    const AutoHuntSettings& autoHunt = GetAutoHuntSettings();
    snapshot += "[AutoHunt]\n";
    AppendBoolSnapshot(snapshot, "enabled", autoHunt.enabled);
    AppendBoolSnapshot(snapshot, "usePotions", autoHunt.usePotions);
    AppendBoolSnapshot(snapshot, "autoRepair", autoHunt.autoRepair);
    AppendBoolSnapshot(snapshot, "autoStore", autoHunt.autoStore);
    AppendBoolSnapshot(snapshot, "autoReviveInTown", autoHunt.autoReviveInTown);
    // Snapshot skill priority order
    for (int i = 0; i < kHuntSkillCount; i++) {
        char key[32];
        snprintf(key, sizeof(key), "skill%d", i);
        AppendIntSnapshot(snapshot, key, static_cast<int>(autoHunt.skillPriorities[i].type));
        snprintf(key, sizeof(key), "skill%d_en", i);
        AppendBoolSnapshot(snapshot, key, autoHunt.skillPriorities[i].enabled);
    }
    AppendBoolSnapshot(snapshot, "supermanBeforeCyclone", autoHunt.supermanBeforeCyclone);
    AppendBoolSnapshot(snapshot, "useAccuracyIfCycloneActive", autoHunt.useAccuracyIfCycloneActive);
    AppendBoolSnapshot(snapshot, "archerMode", autoHunt.archerMode);
    AppendBoolSnapshot(snapshot, "flyOnlyWithCyclone", autoHunt.flyOnlyWithCyclone);
    AppendBoolSnapshot(snapshot, "useScatterLogic", autoHunt.useScatterLogic);
    AppendBoolSnapshot(snapshot, "pickupNearbyHpPotionWhenLow", autoHunt.pickupNearbyHpPotionWhenLow);
    AppendBoolSnapshot(snapshot, "pickupNearbyManaPotionForStigma", autoHunt.pickupNearbyManaPotionForStigma);
    AppendBoolSnapshot(snapshot, "prioritizeMobClumps", autoHunt.prioritizeMobClumps);
    AppendBoolSnapshot(snapshot, "prioritizeScatterClumps", autoHunt.prioritizeScatterClumps);
    AppendIntSnapshot(snapshot, "mobSearchRange", autoHunt.mobSearchRange);
    AppendBoolSnapshot(snapshot, "immediateReturnOnPriorityItems", autoHunt.immediateReturnOnPriorityItems);
    AppendBoolSnapshot(snapshot, "storeTreasureBank", autoHunt.storeTreasureBank);
    AppendBoolSnapshot(snapshot, "storeComposeBank", autoHunt.storeComposeBank);
    AppendBoolSnapshot(snapshot, "autoDepositSilver", autoHunt.autoDepositSilver);
    AppendIntSnapshot(snapshot, "silverKeepAmount", autoHunt.silverKeepAmount);
    AppendBoolSnapshot(snapshot, "packMeteorsIntoScrolls", autoHunt.packMeteorsIntoScrolls);
    AppendBoolSnapshot(snapshot, "lootRefined", autoHunt.lootRefined);
    AppendBoolSnapshot(snapshot, "lootUnique", autoHunt.lootUnique);
    AppendBoolSnapshot(snapshot, "lootElite", autoHunt.lootElite);
    AppendBoolSnapshot(snapshot, "lootSuper", autoHunt.lootSuper);
    AppendBoolSnapshot(snapshot, "storeRefined", autoHunt.storeRefined);
    AppendBoolSnapshot(snapshot, "storeUnique", autoHunt.storeUnique);
    AppendBoolSnapshot(snapshot, "storeElite", autoHunt.storeElite);
    AppendBoolSnapshot(snapshot, "storeSuper", autoHunt.storeSuper);
    AppendBoolSnapshot(snapshot, "buyArrows", autoHunt.buyArrows);
    AppendIntSnapshot(snapshot, "arrowTypeId", (int)autoHunt.arrowTypeId);
    AppendIntSnapshot(snapshot, "arrowBuyCount", autoHunt.arrowBuyCount);
    AppendIntSnapshot(snapshot, "hpPotionPercent", autoHunt.hpPotionPercent);
    AppendIntSnapshot(snapshot, "manaPotionPercent", autoHunt.manaPotionPercent);
    AppendIntSnapshot(snapshot, "repairPercent", autoHunt.repairPercent);
    AppendIntSnapshot(snapshot, "bagStoreThreshold", autoHunt.bagStoreThreshold);
    AppendIntSnapshot(snapshot, "clumpRadius", autoHunt.clumpRadius);
    AppendIntSnapshot(snapshot, "minimumMobClump", autoHunt.minimumMobClump);
    AppendIntSnapshot(snapshot, "minimumScatterHits", autoHunt.minimumScatterHits);
    AppendIntSnapshot(snapshot, "scatterRangeOverride", autoHunt.scatterRangeOverride);
    AppendIntSnapshot(snapshot, "actionRadius", autoHunt.actionRadius);
    AppendIntSnapshot(snapshot, "rangedAttackRange", autoHunt.rangedAttackRange);
    AppendIntSnapshot(snapshot, "archerSafetyDistance", autoHunt.archerSafetyDistance);
    AppendIntSnapshot(snapshot, "lootRange", autoHunt.lootRange);
    AppendIntSnapshot(snapshot, "movementIntervalMs", autoHunt.movementIntervalMs);
    AppendIntSnapshot(snapshot, "attackIntervalMs", autoHunt.attackIntervalMs);
    AppendIntSnapshot(snapshot, "cycloneAttackIntervalMs", autoHunt.cycloneAttackIntervalMs);
    AppendIntSnapshot(snapshot, "targetSwitchAttackIntervalMs", autoHunt.targetSwitchAttackIntervalMs);
    AppendIntSnapshot(snapshot, "itemActionIntervalMs", autoHunt.itemActionIntervalMs);
    AppendIntSnapshot(snapshot, "lootSpawnGraceMs", autoHunt.lootSpawnGraceMs);
    AppendIntSnapshot(snapshot, "selfCastIntervalMs", autoHunt.selfCastIntervalMs);
    AppendIntSnapshot(snapshot, "npcActionIntervalMs", autoHunt.npcActionIntervalMs);
    AppendIntSnapshot(snapshot, "lootPickupIgnoreMs", autoHunt.lootPickupIgnoreMs);
    AppendIntSnapshot(snapshot, "reviveDelayMs", autoHunt.reviveDelayMs);
    AppendIntSnapshot(snapshot, "reviveRetryIntervalMs", autoHunt.reviveRetryIntervalMs);
    AppendIntSnapshot(snapshot, "minimumLootPlus", autoHunt.minimumLootPlus);
    AppendIntSnapshot(snapshot, "minimumStorePlus", autoHunt.minimumStorePlus);
    AppendIntSnapshot(snapshot, "zoneMapId", autoHunt.zoneMapId);
    AppendIntSnapshot(snapshot, "combatMode", static_cast<int>(autoHunt.combatMode));
    AppendIntSnapshot(snapshot, "zoneMode", static_cast<int>(autoHunt.zoneMode));
    AppendIntSnapshot(snapshot, "zoneCenterX", autoHunt.zoneCenter.x);
    AppendIntSnapshot(snapshot, "zoneCenterY", autoHunt.zoneCenter.y);
    AppendIntSnapshot(snapshot, "zoneRadius", autoHunt.zoneRadius);
    AppendStringSnapshot(snapshot, "monsterNames", autoHunt.monsterNames);
    AppendStringSnapshot(snapshot, "monsterIgnoreNames", autoHunt.monsterIgnoreNames);
    AppendStringSnapshot(snapshot, "monsterPreferNames", autoHunt.monsterPreferNames);
    AppendStringSnapshot(snapshot, "playerWhitelist", autoHunt.playerWhitelist);
    AppendBoolSnapshot(snapshot, "usePacketJump", autoHunt.usePacketJump);
    AppendBoolSnapshot(snapshot, "safetyEnabled", autoHunt.safetyEnabled);
    AppendBoolSnapshot(snapshot, "safetyNotifyDiscord", autoHunt.safetyNotifyDiscord);
    AppendIntSnapshot(snapshot, "safetyPlayerRange", autoHunt.safetyPlayerRange);
    AppendIntSnapshot(snapshot, "safetyDetectionSec", autoHunt.safetyDetectionSec);
    AppendIntSnapshot(snapshot, "safetyRestSec", autoHunt.safetyRestSec);
    AppendStringSnapshot(snapshot, "zonePolygon", SerializePositions(autoHunt.zonePolygon).c_str());
    AppendStringSnapshot(snapshot, "lootItemIds", SerializeU32List(autoHunt.lootItemIds).c_str());
    AppendStringSnapshot(snapshot, "warehouseItemIds", SerializeU32List(autoHunt.warehouseItemIds).c_str());
    AppendStringSnapshot(snapshot, "priorityReturnItemIds", SerializeU32List(autoHunt.priorityReturnItemIds).c_str());

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

static void SaveAutoHuntSection(const char* file, const char* section)
{
    AutoHuntSettings& autoHunt = GetAutoHuntSettings();
    WriteInt(file, section, "enabled", autoHunt.enabled ? 1 : 0);
    WriteInt(file, section, "usePotions", autoHunt.usePotions ? 1 : 0);
    WriteInt(file, section, "autoRepair", autoHunt.autoRepair ? 1 : 0);
    WriteInt(file, section, "autoStore", autoHunt.autoStore ? 1 : 0);
    WriteInt(file, section, "autoReviveInTown", autoHunt.autoReviveInTown ? 1 : 0);
    // Skill priority order: "Superman:1,Cyclone:1,XpFly:0,Fly:0,Stigma:0"
    {
        std::string skillOrder;
        for (int i = 0; i < kHuntSkillCount; i++) {
            if (!skillOrder.empty()) skillOrder += ',';
            const auto& entry = autoHunt.skillPriorities[i];
            const char* name = nullptr;
            switch (entry.type) {
                case HuntSkillType::Superman: name = "Superman"; break;
                case HuntSkillType::Cyclone:  name = "Cyclone";  break;
                case HuntSkillType::Accuracy: name = "Accuracy"; break;
                case HuntSkillType::XpFly:    name = "XpFly";    break;
                case HuntSkillType::Fly:      name = "Fly";      break;
                case HuntSkillType::Stigma:   name = "Stigma";   break;
                default: name = "Unknown"; break;
            }
            skillOrder += name;
            skillOrder += ':';
            skillOrder += (entry.enabled ? '1' : '0');
        }
        WritePrivateProfileStringA(section, "skillOrder", skillOrder.c_str(), file);
    }
    // Legacy bools for backwards compatibility
    WriteInt(file, section, "castSuperman", autoHunt.castSuperman ? 1 : 0);
    WriteInt(file, section, "castCyclone", autoHunt.castCyclone ? 1 : 0);
    WriteInt(file, section, "supermanBeforeCyclone", autoHunt.supermanBeforeCyclone ? 1 : 0);
    WriteInt(file, section, "useAccuracyIfCycloneActive", autoHunt.useAccuracyIfCycloneActive ? 1 : 0);
    WriteInt(file, section, "useStigma", autoHunt.useStigma ? 1 : 0);
    WriteInt(file, section, "archerMode", autoHunt.archerMode ? 1 : 0);
    WriteInt(file, section, "castXpFly", autoHunt.castXpFly ? 1 : 0);
    WriteInt(file, section, "castFlyStamina", autoHunt.castFly ? 1 : 0);
    WriteInt(file, section, "flyOnlyWithCyclone", autoHunt.flyOnlyWithCyclone ? 1 : 0);
    WriteInt(file, section, "useScatterLogic", autoHunt.useScatterLogic ? 1 : 0);
    WriteInt(file, section, "pickupNearbyHpPotionWhenLow", autoHunt.pickupNearbyHpPotionWhenLow ? 1 : 0);
    WriteInt(file, section, "pickupNearbyManaPotionForStigma", autoHunt.pickupNearbyManaPotionForStigma ? 1 : 0);
    WriteInt(file, section, "prioritizeMobClumps", autoHunt.prioritizeMobClumps ? 1 : 0);
    WriteInt(file, section, "prioritizeScatterClumps", autoHunt.prioritizeScatterClumps ? 1 : 0);
    WriteInt(file, section, "mobSearchRange", autoHunt.mobSearchRange);
    WriteInt(file, section, "immediateReturnOnPriorityItems", autoHunt.immediateReturnOnPriorityItems ? 1 : 0);
    WriteInt(file, section, "storeTreasureBank", autoHunt.storeTreasureBank ? 1 : 0);
    WriteInt(file, section, "storeComposeBank", autoHunt.storeComposeBank ? 1 : 0);
    WriteInt(file, section, "autoDepositSilver", autoHunt.autoDepositSilver ? 1 : 0);
    WriteInt(file, section, "silverKeepAmount", autoHunt.silverKeepAmount);
    WriteInt(file, section, "packMeteorsIntoScrolls", autoHunt.packMeteorsIntoScrolls ? 1 : 0);
    WriteInt(file, section, "lootRefined", autoHunt.lootRefined ? 1 : 0);
    WriteInt(file, section, "lootUnique", autoHunt.lootUnique ? 1 : 0);
    WriteInt(file, section, "lootElite", autoHunt.lootElite ? 1 : 0);
    WriteInt(file, section, "lootSuper", autoHunt.lootSuper ? 1 : 0);
    WriteInt(file, section, "storeRefined", autoHunt.storeRefined ? 1 : 0);
    WriteInt(file, section, "storeUnique", autoHunt.storeUnique ? 1 : 0);
    WriteInt(file, section, "storeElite", autoHunt.storeElite ? 1 : 0);
    WriteInt(file, section, "storeSuper", autoHunt.storeSuper ? 1 : 0);
    WriteInt(file, section, "buyArrows", autoHunt.buyArrows ? 1 : 0);
    WriteInt(file, section, "arrowTypeId", (int)autoHunt.arrowTypeId);
    WriteInt(file, section, "arrowBuyCount", autoHunt.arrowBuyCount);
    WriteInt(file, section, "hpPotionPercent", autoHunt.hpPotionPercent);
    WriteInt(file, section, "manaPotionPercent", autoHunt.manaPotionPercent);
    WriteInt(file, section, "repairPercent", autoHunt.repairPercent);
    WriteInt(file, section, "bagStoreThreshold", autoHunt.bagStoreThreshold);
    WriteInt(file, section, "clumpRadius", autoHunt.clumpRadius);
    WriteInt(file, section, "minimumMobClump", autoHunt.minimumMobClump);
    WriteInt(file, section, "minimumScatterHits", autoHunt.minimumScatterHits);
    WriteInt(file, section, "scatterRangeOverride", autoHunt.scatterRangeOverride);
    WriteInt(file, section, "actionRadius", autoHunt.actionRadius);
    WriteInt(file, section, "rangedAttackRange", autoHunt.rangedAttackRange);
    WriteInt(file, section, "archerSafetyDistance", autoHunt.archerSafetyDistance);
    WriteInt(file, section, "lootRange", autoHunt.lootRange);
    WriteInt(file, section, "movementIntervalMs", autoHunt.movementIntervalMs);
    WriteInt(file, section, "attackIntervalMs", autoHunt.attackIntervalMs);
    WriteInt(file, section, "cycloneAttackIntervalMs", autoHunt.cycloneAttackIntervalMs);
    WriteInt(file, section, "targetSwitchAttackIntervalMs", autoHunt.targetSwitchAttackIntervalMs);
    WriteInt(file, section, "itemActionIntervalMs", autoHunt.itemActionIntervalMs);
    WriteInt(file, section, "lootSpawnGraceMs", autoHunt.lootSpawnGraceMs);
    WriteInt(file, section, "selfCastIntervalMs", autoHunt.selfCastIntervalMs);
    WriteInt(file, section, "npcActionIntervalMs", autoHunt.npcActionIntervalMs);
    WriteInt(file, section, "lootPickupIgnoreMs", autoHunt.lootPickupIgnoreMs);
    WriteInt(file, section, "reviveDelayMs", autoHunt.reviveDelayMs);
    WriteInt(file, section, "reviveRetryIntervalMs", autoHunt.reviveRetryIntervalMs);
    WriteInt(file, section, "minimumLootPlus", autoHunt.minimumLootPlus);
    WriteInt(file, section, "minimumStorePlus", autoHunt.minimumStorePlus);
    WriteInt(file, section, "zoneMapId", autoHunt.zoneMapId);
    WriteInt(file, section, "combatMode", static_cast<int>(autoHunt.combatMode));
    WriteInt(file, section, "zoneMode", static_cast<int>(autoHunt.zoneMode));
    WriteInt(file, section, "zoneCenterX", autoHunt.zoneCenter.x);
    WriteInt(file, section, "zoneCenterY", autoHunt.zoneCenter.y);
    WriteInt(file, section, "zoneRadius", autoHunt.zoneRadius);
    WritePrivateProfileStringA(section, "monsterNames", autoHunt.monsterNames, file);
    WritePrivateProfileStringA(section, "monsterIgnoreNames", autoHunt.monsterIgnoreNames, file);
    WritePrivateProfileStringA(section, "monsterPreferNames", autoHunt.monsterPreferNames, file);
    WritePrivateProfileStringA(section, "playerWhitelist", autoHunt.playerWhitelist, file);
    WriteInt(file, section, "usePacketJump", autoHunt.usePacketJump ? 1 : 0);
    WriteInt(file, section, "safetyEnabled", autoHunt.safetyEnabled ? 1 : 0);
    WriteInt(file, section, "safetyNotifyDiscord", autoHunt.safetyNotifyDiscord ? 1 : 0);
    WriteInt(file, section, "safetyPlayerRange", autoHunt.safetyPlayerRange);
    WriteInt(file, section, "safetyDetectionSec", autoHunt.safetyDetectionSec);
    WriteInt(file, section, "safetyRestSec", autoHunt.safetyRestSec);
    const std::string polygon = SerializePositions(autoHunt.zonePolygon);
    WritePrivateProfileStringA(section, "zonePolygon", polygon.c_str(), file);
    const std::string lootIds = SerializeU32List(autoHunt.lootItemIds);
    WritePrivateProfileStringA(section, "lootItemIds", lootIds.c_str(), file);
    const std::string warehouseIds = SerializeU32List(autoHunt.warehouseItemIds);
    WritePrivateProfileStringA(section, "warehouseItemIds", warehouseIds.c_str(), file);
    const std::string priorityReturnIds = SerializeU32List(autoHunt.priorityReturnItemIds);
    WritePrivateProfileStringA(section, "priorityReturnItemIds", priorityReturnIds.c_str(), file);
}

static void LoadAutoHuntSection(const char* file, const char* section)
{
    AutoHuntSettings& autoHunt = GetAutoHuntSettings();
    autoHunt = AutoHuntSettings{};
    autoHunt.enabled = ReadInt(file, section, "enabled", 0) != 0;
    autoHunt.usePotions = ReadInt(file, section, "usePotions", 1) != 0;
    autoHunt.autoRepair = ReadInt(file, section, "autoRepair", 1) != 0;
    autoHunt.autoStore = ReadInt(file, section, "autoStore", 1) != 0;
    autoHunt.autoReviveInTown = ReadInt(file, section, "autoReviveInTown", 1) != 0;
    // Load legacy individual skill bools first (used for migration)
    autoHunt.castSuperman = ReadInt(file, section, "castSuperman", 0) != 0;
    autoHunt.castCyclone = ReadInt(file, section, "castCyclone", 1) != 0;
    autoHunt.supermanBeforeCyclone = ReadInt(file, section, "supermanBeforeCyclone", 0) != 0;
    autoHunt.useAccuracyIfCycloneActive = ReadInt(file, section, "useAccuracyIfCycloneActive", 0) != 0;
    autoHunt.useStigma = ReadInt(file, section, "useStigma", 0) != 0;
    autoHunt.archerMode = ReadInt(file, section, "archerMode", 0) != 0;
    autoHunt.castXpFly = ReadInt(file, section, "castXpFly", -1) != 0;
    if (ReadInt(file, section, "castXpFly", -1) == -1)  // key absent — migrate old "castFly"
        autoHunt.castXpFly = ReadInt(file, section, "castFly", 0) != 0;
    autoHunt.castFly = ReadInt(file, section, "castFlyStamina", 0) != 0;
    autoHunt.flyOnlyWithCyclone = ReadInt(file, section, "flyOnlyWithCyclone", 0) != 0;

    // Load skill priority order — if present, overrides legacy bools
    {
        char skillOrderBuf[512] = {};
        ReadString(file, section, "skillOrder", "", skillOrderBuf, sizeof(skillOrderBuf));
        if (skillOrderBuf[0] != '\0') {
            // Parse "Superman:1,Cyclone:1,XpFly:0,Fly:0,Stigma:0"
            HuntSkillEntry parsed[kHuntSkillCount] = {};
            int count = 0;
            char* ctx = nullptr;
            char* token = strtok_s(skillOrderBuf, ",", &ctx);
            while (token && count < kHuntSkillCount) {
                char* colon = strchr(token, ':');
                if (colon) {
                    *colon = '\0';
                    const char* name = token;
                    bool enabled = (atoi(colon + 1) != 0);
                    HuntSkillType type = HuntSkillType::Count;
                    if (strcmp(name, "Superman") == 0) type = HuntSkillType::Superman;
                    else if (strcmp(name, "Cyclone") == 0) type = HuntSkillType::Cyclone;
                    else if (strcmp(name, "Accuracy") == 0) type = HuntSkillType::Accuracy;
                    else if (strcmp(name, "XpFly") == 0) type = HuntSkillType::XpFly;
                    else if (strcmp(name, "Fly") == 0) type = HuntSkillType::Fly;
                    else if (strcmp(name, "Stigma") == 0) type = HuntSkillType::Stigma;
                    if (type != HuntSkillType::Count) {
                        parsed[count].type = type;
                        parsed[count].enabled = enabled;
                        count++;
                    }
                }
                token = strtok_s(nullptr, ",", &ctx);
            }
            if (count == kHuntSkillCount) {
                for (int i = 0; i < kHuntSkillCount; i++)
                    autoHunt.skillPriorities[i] = parsed[i];
            }
            // else: malformed, keep defaults
        } else {
            // No skillOrder key — migrate from legacy individual bools
            autoHunt.skillPriorities[0] = { HuntSkillType::Superman, autoHunt.castSuperman };
            autoHunt.skillPriorities[1] = { HuntSkillType::Cyclone, autoHunt.castCyclone };
            autoHunt.skillPriorities[2] = { HuntSkillType::Accuracy, autoHunt.useAccuracyIfCycloneActive };
            autoHunt.skillPriorities[3] = { HuntSkillType::XpFly, autoHunt.castXpFly };
            autoHunt.skillPriorities[4] = { HuntSkillType::Fly, autoHunt.castFly };
            autoHunt.skillPriorities[5] = { HuntSkillType::Stigma, autoHunt.useStigma };
        }
        autoHunt.SyncSkillBoolsFromPriorities();
    }
    autoHunt.useScatterLogic = ReadInt(file, section, "useScatterLogic", 1) != 0;
    autoHunt.pickupNearbyHpPotionWhenLow = ReadInt(file, section, "pickupNearbyHpPotionWhenLow", 0) != 0;
    autoHunt.pickupNearbyManaPotionForStigma = ReadInt(file, section, "pickupNearbyManaPotionForStigma", 0) != 0;
    autoHunt.prioritizeMobClumps = ReadInt(file, section, "prioritizeMobClumps", 1) != 0;
    autoHunt.prioritizeScatterClumps = ReadInt(file, section, "prioritizeScatterClumps", 1) != 0;
    autoHunt.mobSearchRange = ReadInt(file, section, "mobSearchRange", 0);
    if (autoHunt.mobSearchRange < 0)
        autoHunt.mobSearchRange = 0;
    if (autoHunt.mobSearchRange > 18)
        autoHunt.mobSearchRange = 18;
    autoHunt.immediateReturnOnPriorityItems = ReadInt(file, section, "immediateReturnOnPriorityItems", 0) != 0;
    autoHunt.storeTreasureBank = ReadInt(file, section, "storeTreasureBank", 1) != 0;
    autoHunt.storeComposeBank = ReadInt(file, section, "storeComposeBank", 1) != 0;
    autoHunt.autoDepositSilver = ReadInt(file, section, "autoDepositSilver", 0) != 0;
    autoHunt.silverKeepAmount = ReadInt(file, section, "silverKeepAmount", 0);
    if (autoHunt.silverKeepAmount < 0)
        autoHunt.silverKeepAmount = 0;
    autoHunt.packMeteorsIntoScrolls = ReadInt(file, section, "packMeteorsIntoScrolls", 0) != 0;
    autoHunt.lootRefined = ReadInt(file, section, "lootRefined", 0) != 0;
    autoHunt.lootUnique = ReadInt(file, section, "lootUnique", 0) != 0;
    autoHunt.lootElite = ReadInt(file, section, "lootElite", 0) != 0;
    autoHunt.lootSuper = ReadInt(file, section, "lootSuper", 0) != 0;
    autoHunt.storeRefined = ReadInt(file, section, "storeRefined", 0) != 0;
    autoHunt.storeUnique = ReadInt(file, section, "storeUnique", 0) != 0;
    autoHunt.storeElite = ReadInt(file, section, "storeElite", 0) != 0;
    autoHunt.storeSuper = ReadInt(file, section, "storeSuper", 0) != 0;
    autoHunt.buyArrows = ReadInt(file, section, "buyArrows", 0) != 0;
    autoHunt.arrowTypeId = (uint32_t)ReadInt(file, section, "arrowTypeId", 1050002);
    autoHunt.arrowBuyCount = ReadInt(file, section, "arrowBuyCount", 3);
    if (autoHunt.arrowBuyCount < 1)
        autoHunt.arrowBuyCount = 1;
    if (autoHunt.arrowBuyCount > 10)
        autoHunt.arrowBuyCount = 10;
    autoHunt.hpPotionPercent = ReadInt(file, section, "hpPotionPercent", 50);
    autoHunt.manaPotionPercent = ReadInt(file, section, "manaPotionPercent", 35);
    autoHunt.repairPercent = ReadInt(file, section, "repairPercent", 70);
    autoHunt.bagStoreThreshold = ReadInt(file, section, "bagStoreThreshold", 36);
    const int legacyClumpRadius = ReadInt(file, section, "aoeRadius", 8);
    autoHunt.clumpRadius = ReadInt(file, section, "clumpRadius", legacyClumpRadius);
    if (autoHunt.clumpRadius < 1)
        autoHunt.clumpRadius = 1;
    if (autoHunt.clumpRadius > 18)
        autoHunt.clumpRadius = 18;
    autoHunt.minimumMobClump = ReadInt(file, section, "minimumMobClump", 5);
    autoHunt.minimumScatterHits = ReadInt(file, section, "minimumScatterHits", 3);
    if (autoHunt.minimumScatterHits < 1)
        autoHunt.minimumScatterHits = 1;
    if (autoHunt.minimumScatterHits > 12)
        autoHunt.minimumScatterHits = 12;
    autoHunt.scatterRangeOverride = ReadInt(file, section, "scatterRangeOverride", 0);
    if (autoHunt.scatterRangeOverride < 0)
        autoHunt.scatterRangeOverride = 0;
    if (autoHunt.scatterRangeOverride > 18)
        autoHunt.scatterRangeOverride = 18;
    autoHunt.actionRadius = ReadInt(file, section, "actionRadius", 6);
    if (autoHunt.actionRadius < 1)
        autoHunt.actionRadius = 1;
    if (autoHunt.actionRadius > 18)
        autoHunt.actionRadius = 18;
    autoHunt.rangedAttackRange = ReadInt(file, section, "rangedAttackRange", 0);
    if (autoHunt.rangedAttackRange < 0)
        autoHunt.rangedAttackRange = 0;
    if (autoHunt.rangedAttackRange > 18)
        autoHunt.rangedAttackRange = 18;
    autoHunt.archerSafetyDistance = ReadInt(file, section, "archerSafetyDistance", 0);
    if (autoHunt.archerSafetyDistance < 0)
        autoHunt.archerSafetyDistance = 0;
    if (autoHunt.archerSafetyDistance > 18)
        autoHunt.archerSafetyDistance = 18;
    const int combatMode = ReadInt(file, section, "combatMode", autoHunt.archerMode ? 1 : 0);
    autoHunt.combatMode = combatMode == static_cast<int>(AutoHuntCombatMode::Archer)
        ? AutoHuntCombatMode::Archer
        : AutoHuntCombatMode::Melee;
    autoHunt.lootRange = ReadInt(file, section, "lootRange", 5);
    if (autoHunt.lootRange < 0)
        autoHunt.lootRange = 0;
    if (autoHunt.lootRange > 18)
        autoHunt.lootRange = 18;
    autoHunt.movementIntervalMs = ReadInt(file, section, "movementIntervalMs", 900);
    if (autoHunt.movementIntervalMs < 100)
        autoHunt.movementIntervalMs = 100;
    if (autoHunt.movementIntervalMs > 5000)
        autoHunt.movementIntervalMs = 5000;
    autoHunt.attackIntervalMs = ReadInt(file, section, "attackIntervalMs", autoHunt.attackIntervalMs);
    if (autoHunt.attackIntervalMs < 25)
        autoHunt.attackIntervalMs = 25;
    if (autoHunt.attackIntervalMs > 5000)
        autoHunt.attackIntervalMs = 5000;
    autoHunt.cycloneAttackIntervalMs = ReadInt(file, section, "cycloneAttackIntervalMs", autoHunt.cycloneAttackIntervalMs);
    if (autoHunt.cycloneAttackIntervalMs < 25)
        autoHunt.cycloneAttackIntervalMs = 25;
    if (autoHunt.cycloneAttackIntervalMs > 5000)
        autoHunt.cycloneAttackIntervalMs = 5000;
    autoHunt.targetSwitchAttackIntervalMs = ReadInt(file, section, "targetSwitchAttackIntervalMs", autoHunt.targetSwitchAttackIntervalMs);
    if (autoHunt.targetSwitchAttackIntervalMs < 0)
        autoHunt.targetSwitchAttackIntervalMs = 0;
    if (autoHunt.targetSwitchAttackIntervalMs > 5000)
        autoHunt.targetSwitchAttackIntervalMs = 5000;
    autoHunt.itemActionIntervalMs = ReadInt(file, section, "itemActionIntervalMs", autoHunt.itemActionIntervalMs);
    if (autoHunt.itemActionIntervalMs < 100)
        autoHunt.itemActionIntervalMs = 100;
    if (autoHunt.itemActionIntervalMs > 5000)
        autoHunt.itemActionIntervalMs = 5000;
    autoHunt.lootSpawnGraceMs = ReadInt(file, section, "lootSpawnGraceMs", autoHunt.lootSpawnGraceMs);
    if (autoHunt.lootSpawnGraceMs < 0)
        autoHunt.lootSpawnGraceMs = 0;
    if (autoHunt.lootSpawnGraceMs > 5000)
        autoHunt.lootSpawnGraceMs = 5000;
    autoHunt.selfCastIntervalMs = ReadInt(file, section, "selfCastIntervalMs", autoHunt.selfCastIntervalMs);
    if (autoHunt.selfCastIntervalMs < 100)
        autoHunt.selfCastIntervalMs = 100;
    if (autoHunt.selfCastIntervalMs > 5000)
        autoHunt.selfCastIntervalMs = 5000;
    autoHunt.npcActionIntervalMs = ReadInt(file, section, "npcActionIntervalMs", autoHunt.npcActionIntervalMs);
    if (autoHunt.npcActionIntervalMs < 100)
        autoHunt.npcActionIntervalMs = 100;
    if (autoHunt.npcActionIntervalMs > 2000)
        autoHunt.npcActionIntervalMs = 2000;
    autoHunt.lootPickupIgnoreMs = ReadInt(file, section, "lootPickupIgnoreMs", autoHunt.lootPickupIgnoreMs);
    if (autoHunt.lootPickupIgnoreMs < 0)
        autoHunt.lootPickupIgnoreMs = 0;
    if (autoHunt.lootPickupIgnoreMs > 300000)
        autoHunt.lootPickupIgnoreMs = 300000;
    autoHunt.reviveDelayMs = ReadInt(file, section, "reviveDelayMs", autoHunt.reviveDelayMs);
    if (autoHunt.reviveDelayMs < 0)
        autoHunt.reviveDelayMs = 0;
    if (autoHunt.reviveDelayMs > 60000)
        autoHunt.reviveDelayMs = 60000;
    autoHunt.reviveRetryIntervalMs = ReadInt(file, section, "reviveRetryIntervalMs", autoHunt.reviveRetryIntervalMs);
    if (autoHunt.reviveRetryIntervalMs < 100)
        autoHunt.reviveRetryIntervalMs = 100;
    if (autoHunt.reviveRetryIntervalMs > 10000)
        autoHunt.reviveRetryIntervalMs = 10000;
    autoHunt.minimumLootPlus = ReadInt(file, section, "minimumLootPlus", 0);
    autoHunt.minimumStorePlus = ReadInt(file, section, "minimumStorePlus", autoHunt.minimumLootPlus);
    autoHunt.zoneMapId = ReadInt(file, section, "zoneMapId", 0);
    autoHunt.zoneMode = static_cast<AutoHuntZoneMode>(ReadInt(file, section, "zoneMode", 0));
    autoHunt.zoneCenter.x = ReadInt(file, section, "zoneCenterX", 0);
    autoHunt.zoneCenter.y = ReadInt(file, section, "zoneCenterY", 0);
    autoHunt.zoneRadius = ReadInt(file, section, "zoneRadius", 12);
    ReadString(file, section, "monsterNames", "", autoHunt.monsterNames, sizeof(autoHunt.monsterNames));
    ReadString(file, section, "monsterIgnoreNames", "", autoHunt.monsterIgnoreNames, sizeof(autoHunt.monsterIgnoreNames));
    ReadString(file, section, "monsterPreferNames", "", autoHunt.monsterPreferNames, sizeof(autoHunt.monsterPreferNames));
    ReadString(file, section, "playerWhitelist", "", autoHunt.playerWhitelist, sizeof(autoHunt.playerWhitelist));
    autoHunt.usePacketJump = ReadInt(file, section, "usePacketJump",
        ReadInt(file, "Misc", "usePacketJump", 0)) != 0;
    autoHunt.safetyEnabled = ReadInt(file, section, "safetyEnabled", 0) != 0;
    autoHunt.safetyNotifyDiscord = ReadInt(file, section, "safetyNotifyDiscord", 0) != 0;
    autoHunt.safetyPlayerRange = ReadInt(file, section, "safetyPlayerRange", 15);
    autoHunt.safetyDetectionSec = ReadInt(file, section, "safetyDetectionSec", 30);
    autoHunt.safetyRestSec = ReadInt(file, section, "safetyRestSec", 120);
    char polygonBuf[2048] = {};
    ReadString(file, section, "zonePolygon", "", polygonBuf, sizeof(polygonBuf));
    ParsePositions(polygonBuf, autoHunt.zonePolygon);
    char lootBuf[4096] = {};
    ReadString(file, section, "lootItemIds", "", lootBuf, sizeof(lootBuf));
    ParseU32List(lootBuf, autoHunt.lootItemIds);
    char warehouseBuf[4096] = {};
    ReadString(file, section, "warehouseItemIds", "__unset__", warehouseBuf, sizeof(warehouseBuf));
    if (strcmp(warehouseBuf, "__unset__") != 0) {
        ParseU32List(warehouseBuf, autoHunt.warehouseItemIds);
    } else {
        autoHunt.warehouseItemIds = autoHunt.lootItemIds;
    }
    char priorityReturnBuf[4096] = {};
    ReadString(file, section, "priorityReturnItemIds", "", priorityReturnBuf, sizeof(priorityReturnBuf));
    ParseU32List(priorityReturnBuf, autoHunt.priorityReturnItemIds);
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
    SaveAutoHuntSection(file, "AutoHunt");
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
    LoadAutoHuntSection(file, "AutoHunt");
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
