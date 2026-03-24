#include "gateway.h"
#include "CItem.h"
#include <algorithm>
#include <climits>
#include <unordered_map>
#include <queue>
#include <unordered_set>

namespace
{
    constexpr uint32_t kConductressSilverCost = 100;
    constexpr uint32_t kGuildGatewaySilverCost = 1000;
    constexpr int kMinSameMapItemGateDistance = 160;

    Gateway WithSilverCost(Gateway gateway, uint32_t silverCost)
    {
        gateway.silverCost = silverCost;
        return gateway;
    }
}

// =====================================================================
// Hardcoded gateway table
// =====================================================================
static const std::unordered_map<OBJID, std::vector<Gateway>> s_gateways = {
    {
        MAP_MARKET, {
            {GatewayType::Npc, MAP_MARKET, {215, 220}, MAP_TWIN_CITY, {430, 380}, "Mark.Controller"},
            {GatewayType::Npc, MAP_MARKET, {215, 220}, MAP_DESERT_CITY, {493, 650}, "Mark.Controller"},
            {GatewayType::Npc, MAP_MARKET, {215, 220}, MAP_PHOENIX_CASTLE, {193, 266}, "Mark.Controller"},
            {GatewayType::Npc, MAP_MARKET, {215, 220}, MAP_APE_MOUNTAIN, {566, 565}, "Mark.Controller"},
            {GatewayType::Npc, MAP_MARKET, {215, 220}, MAP_BIRD_ISLAND, {717, 577}, "Mark.Controller"},
        }
    },
    {
        MAP_TWIN_CITY, {
            // PC
            WithSilverCost({GatewayType::Npc, MAP_TWIN_CITY, {435, 440}, MAP_TWIN_CITY, {958, 555}, "Conductress"},
                           kConductressSilverCost),
            // DC
            WithSilverCost({
                               GatewayType::Npc, MAP_TWIN_CITY, {435, 440}, MAP_TWIN_CITY, {69, 473}, "Conductress", 1,
                               {1}
                           }, kConductressSilverCost),
            // AC
            WithSilverCost({
                               GatewayType::Npc, MAP_TWIN_CITY, {435, 440}, MAP_TWIN_CITY, {555, 957}, "Conductress", 1,
                               {2}
                           }, kConductressSilverCost),
            // BI
            WithSilverCost({
                               GatewayType::Npc, MAP_TWIN_CITY, {435, 440}, MAP_TWIN_CITY, {232, 190}, "Conductress", 1,
                               {3}
                           }, kConductressSilverCost),
            // Mine
            WithSilverCost({
                               GatewayType::Npc, MAP_TWIN_CITY, {435, 440}, MAP_TWIN_CITY, {53, 399}, "Conductress", 1,
                               {4}
                           }, kConductressSilverCost),
            // Market
            WithSilverCost({GatewayType::Npc, MAP_TWIN_CITY, {435, 440}, MAP_MARKET, {211, 196}, "Conductress", 1, {5}},
                           kConductressSilverCost),

            // GuildController
            WithSilverCost({GatewayType::Npc, MAP_TWIN_CITY, {350, 337}, MAP_GUILD, {351, 341}, "GuildController"},
                           kGuildGatewaySilverCost),


            // DC
            {GatewayType::Npc, MAP_TWIN_CITY, {60, 463}, MAP_DESERT_CITY, {971, 666}, "GeneralPeace"},

            // Portals
            {GatewayType::Portal, MAP_TWIN_CITY, {962, 557}, MAP_PHOENIX_CASTLE, {11, 376}},
            {GatewayType::Portal, MAP_TWIN_CITY, {556, 964}, MAP_APE_MOUNTAIN, {381, 021}},
            {GatewayType::Portal, MAP_TWIN_CITY, {224, 196}, MAP_BIRD_ISLAND, {1010, 710}},
            {GatewayType::Portal, MAP_TWIN_CITY, {45, 395}, MAP_MINE_CAVE, {160, 96}}
        }
    },
    {
        MAP_DESERT_CITY, {
            // TC
            WithSilverCost({GatewayType::Npc, MAP_DESERT_CITY, {478, 631}, MAP_DESERT_CITY, {971, 668}, "Conductress"},
                           kConductressSilverCost),
            // Mystic Castle
            WithSilverCost({
                               GatewayType::Npc, MAP_DESERT_CITY, {478, 631}, MAP_DESERT_CITY, {85, 323}, "Conductress",
                               1, {1}
                           }, kConductressSilverCost),
            // Market
            WithSilverCost({
                               GatewayType::Npc, MAP_DESERT_CITY, {478, 631}, MAP_MARKET, {211, 196}, "Conductress", 1,
                               {2}
                           }, kConductressSilverCost),
            // Portals
            {GatewayType::Portal, MAP_DESERT_CITY, {977, 668}, MAP_TWIN_CITY, {69, 473}},
            {GatewayType::Portal, MAP_DESERT_CITY, {77, 320}, MAP_MYSTIC_CASTLE, {312, 646}},
        }
    },
    {
        MAP_MYSTIC_CASTLE, {
            {GatewayType::Portal, MAP_MYSTIC_CASTLE, {311, 650}, MAP_DESERT_CITY, {85, 323}},
        }
    },
    {
        MAP_PHOENIX_CASTLE, {
            // TC
            WithSilverCost({
                               GatewayType::Npc, MAP_PHOENIX_CASTLE, {228, 255}, MAP_PHOENIX_CASTLE, {11, 376},
                               "Conductress"
                           }, kConductressSilverCost),
            // Market
            WithSilverCost({
                               GatewayType::Npc, MAP_PHOENIX_CASTLE, {228, 255}, MAP_MARKET, {211, 196}, "Conductress",
                               1, {1}
                           }, kConductressSilverCost),
            // Portals
            {GatewayType::Portal, MAP_PHOENIX_CASTLE, {6, 376}, MAP_TWIN_CITY, {958, 555}},
        }
    },
    {
        MAP_APE_MOUNTAIN, {
            // TC
            WithSilverCost({GatewayType::Npc, MAP_APE_MOUNTAIN, {566, 622}, MAP_APE_MOUNTAIN, {381, 21}, "Conductress"},
                           kConductressSilverCost),
            // Market
            WithSilverCost({
                               GatewayType::Npc, MAP_APE_MOUNTAIN, {566, 622}, MAP_MARKET, {211, 196}, "Conductress", 1,
                               {1}
                           }, kConductressSilverCost),
            // Portals
            {GatewayType::Portal, MAP_APE_MOUNTAIN, {377, 9}, MAP_TWIN_CITY, {555, 957}},
        }
    },
    {
        MAP_BIRD_ISLAND, {
            // TC
            WithSilverCost({GatewayType::Npc, MAP_BIRD_ISLAND, {789, 566}, MAP_BIRD_ISLAND, {1010, 710}, "Conductress"},
                           kConductressSilverCost),
            // Market
            WithSilverCost({
                               GatewayType::Npc, MAP_BIRD_ISLAND, {789, 566}, MAP_MARKET, {211, 196}, "Conductress", 1,
                               {1}
                           }, kConductressSilverCost),
            // Portals
            {GatewayType::Portal, MAP_BIRD_ISLAND, {1018, 710}, MAP_TWIN_CITY, {232, 190}},
        }
    },
    {
        MAP_MINE_CAVE, {
            // Portals
            {GatewayType::Portal, MAP_MINE_CAVE, {157, 91}, MAP_TWIN_CITY, {53, 399}},
            {GatewayType::Portal, MAP_MINE_CAVE, {49, 71}, MAP_GROTTO1, {333, 354}},
        }
    },
    {
        MAP_GUILD, {
            // TC
            WithSilverCost({GatewayType::Npc, MAP_GUILD, {354, 345}, MAP_TWIN_CITY, {430, 380}, "GuildKeeper"},
                           kGuildGatewaySilverCost),
            // GC 1
            WithSilverCost({GatewayType::Npc, MAP_GUILD, {331, 338}, MAP_OCEAN, {63, 133}, "GuildConductor1"},
                           kGuildGatewaySilverCost),
        }
    },
    {
        MAP_GROTTO1, {
            {GatewayType::Portal, MAP_GROTTO1, {333, 354}, MAP_MINE_CAVE, {49, 75}},
            {GatewayType::Portal, MAP_GROTTO1, {291, 64}, MAP_GROTTO2, {385, 628}},
            WithSilverCost({GatewayType::Npc, MAP_GROTTO1, {348, 474}, MAP_GUILD, {339, 353}, "GuildTeleporter2"},
                           kGuildGatewaySilverCost)
        }
    },
    {
        MAP_GROTTO2, {
            {GatewayType::Portal, MAP_GROTTO2, {385, 633}, MAP_GROTTO1, {293, 67}},
        }
    },
    {
        MAP_OCEAN, {
            WithSilverCost({GatewayType::Npc, MAP_OCEAN, {67, 119}, MAP_GUILD, {351, 341}, "GuildTeleporter1"},
                           kGuildGatewaySilverCost)
        }
    }
};

// =====================================================================
// Named destinations (map ID + target position)
// =====================================================================
static const Destination s_destinations[] = {
    {"Twin City", MAP_TWIN_CITY, {430, 380}},
    {"Desert City", MAP_DESERT_CITY, {493, 650}},
    {"Mystic Castle", MAP_MYSTIC_CASTLE, {312, 646}},
    {"Phoenix Castle", MAP_PHOENIX_CASTLE, {193, 266}},
    {"Ape Mountain", MAP_APE_MOUNTAIN, {566, 565}},
    {"Bird Island", MAP_BIRD_ISLAND, {717, 577}},
    {"Mine Cave", MAP_MINE_CAVE, {157, 91}},
    {"Market", MAP_MARKET, {211, 196}},
    {"Guild Area", MAP_GUILD, {351, 341}},
    {"Frozen Grotto 1", MAP_GROTTO1, {356, 482}},
    {"Frozen Grotto 2", MAP_GROTTO2, {385, 628}}
};

const Destination* GetDestinations(size_t& count)
{
    count = sizeof(s_destinations) / sizeof(s_destinations[0]);
    return s_destinations;
}

const char* GetMapName(OBJID mapId)
{
    for (const auto& d : s_destinations)
        if (d.mapId == mapId) return d.name;
    return "Unknown";
}

static const std::vector<Gateway> s_empty;

const std::vector<Gateway>& GetGateways(OBJID mapId)
{
    auto it = s_gateways.find(mapId);
    return it != s_gateways.end() ? it->second : s_empty;
}

// =====================================================================
// Dijkstra pathfinding through gateway graph (minimizes walking distance)
// =====================================================================

struct GwNode
{
    OBJID mapId;
    Position pos; // current position on this map
    int walkCost; // cumulative walking distance (Manhattan)
    uint32_t silverLeft; // remaining silver after paid gateways
    std::vector<Gateway> path;
    bool atGoal = false;

    bool operator>(const GwNode& o) const { return walkCost > o.walkCost; }
};

// Key: (mapId, quantized position) — distinguishes different regions on the same map
struct GwNodeKey
{
    OBJID mapId;
    uint32_t qx;
    uint32_t qy;
    uint32_t silverLeft;

    bool operator==(const GwNodeKey& other) const
    {
        return mapId == other.mapId
            && qx == other.qx
            && qy == other.qy
            && silverLeft == other.silverLeft;
    }
};

struct GwNodeKeyHash
{
    size_t operator()(const GwNodeKey& key) const
    {
        size_t h = static_cast<size_t>(key.mapId);
        h ^= static_cast<size_t>(key.qx) << 8;
        h ^= static_cast<size_t>(key.qy) << 24;
        h ^= static_cast<size_t>(key.silverLeft) << 1;
        return h;
    }
};

static inline GwNodeKey GwNodeStateKey(OBJID mapId, const Position& pos, uint32_t silverLeft)
{
    uint32_t qx = (uint32_t)(pos.x / 32);
    uint32_t qy = (uint32_t)(pos.y / 32);
    return {mapId, qx, qy, silverLeft};
}

static inline int ManhattanDist(const Position& a, const Position& b)
{
    return abs(a.x - b.x) + abs(a.y - b.y);
}

static void AppendVipTeleportGateways(std::vector<Gateway>& gateways, OBJID mapId)
{
    gateways.push_back({GatewayType::VipTeleport, mapId, {0, 0}, MAP_TWIN_CITY, {430, 380}});
    gateways.push_back({GatewayType::VipTeleport, mapId, {0, 0}, MAP_PHOENIX_CASTLE, {193, 266}});
    gateways.push_back({GatewayType::VipTeleport, mapId, {0, 0}, MAP_APE_MOUNTAIN, {566, 565}});
    gateways.push_back({GatewayType::VipTeleport, mapId, {0, 0}, MAP_DESERT_CITY, {493, 650}});
    gateways.push_back({GatewayType::VipTeleport, mapId, {0, 0}, MAP_BIRD_ISLAND, {717, 577}});
}

static bool HasItemGatewayType(const std::vector<uint32_t>& availableItemTypes, uint32_t itemTypeId)
{
    return std::find(availableItemTypes.begin(), availableItemTypes.end(), itemTypeId) != availableItemTypes.end();
}

static void AppendItemGateways(std::vector<Gateway>& gateways, OBJID mapId,
                               const std::vector<uint32_t>& availableItemTypes)
{
    if (HasItemGatewayType(availableItemTypes, ItemTypeId::TWIN_CITY_GATE))
        gateways.push_back({
            GatewayType::Item, mapId, {0, 0}, MAP_TWIN_CITY, {430, 380}, nullptr, 0, {}, 101, 0,
            ItemTypeId::TWIN_CITY_GATE
        });
    if (HasItemGatewayType(availableItemTypes, ItemTypeId::DESERT_CITY_GATE))
        gateways.push_back({
            GatewayType::Item, mapId, {0, 0}, MAP_DESERT_CITY, {493, 650}, nullptr, 0, {}, 101, 0,
            ItemTypeId::DESERT_CITY_GATE
        });
    if (HasItemGatewayType(availableItemTypes, ItemTypeId::APE_CITY_GATE))
        gateways.push_back({
            GatewayType::Item, mapId, {0, 0}, MAP_APE_MOUNTAIN, {566, 565}, nullptr, 0, {}, 101, 0,
            ItemTypeId::APE_CITY_GATE
        });
    if (HasItemGatewayType(availableItemTypes, ItemTypeId::CASTLE_GATE))
        gateways.push_back({
            GatewayType::Item, mapId, {0, 0}, MAP_PHOENIX_CASTLE, {193, 266}, nullptr, 0, {}, 101, 0,
            ItemTypeId::CASTLE_GATE
        });
    if (HasItemGatewayType(availableItemTypes, ItemTypeId::BIRD_ISLAND_GATE))
        gateways.push_back({
            GatewayType::Item, mapId, {0, 0}, MAP_BIRD_ISLAND, {717, 577}, nullptr, 0, {}, 101, 0,
            ItemTypeId::BIRD_ISLAND_GATE
        });
    if (HasItemGatewayType(availableItemTypes, ItemTypeId::STONE_CITY_GATE))
        gateways.push_back({
            GatewayType::Item, mapId, {0, 0}, MAP_MYSTIC_CASTLE, {312, 646}, nullptr, 0, {}, 101, 0,
            ItemTypeId::STONE_CITY_GATE
        });
}

std::vector<Gateway> FindGatewayPath(
    OBJID from,
    OBJID to,
    Position heroPos,
    uint32_t availableSilver,
    bool allowVipTeleport,
    Position finalPos,
    std::vector<uint32_t> availableItemTypes)
{
    const bool hasFinalPos = finalPos.x != 0 || finalPos.y != 0;

    std::priority_queue<GwNode, std::vector<GwNode>, std::greater<GwNode>> open;
    std::unordered_map<GwNodeKey, int, GwNodeKeyHash> bestCost;
    int bestGoalCost = INT_MAX;

    open.push({from, heroPos, 0, availableSilver, {}});

    while (!open.empty())
    {
        GwNode current = std::move(const_cast<GwNode&>(open.top()));
        open.pop();

        if (current.atGoal)
            return current.path;

        const GwNodeKey curKey = GwNodeStateKey(current.mapId, current.pos, current.silverLeft);
        auto it = bestCost.find(curKey);
        if (it != bestCost.end() && current.walkCost > it->second)
            continue;

        if (current.mapId == to)
        {
            const int goalCost = current.walkCost + (hasFinalPos ? ManhattanDist(current.pos, finalPos) : 0);
            if (goalCost < bestGoalCost)
            {
                bestGoalCost = goalCost;
                open.push({current.mapId, current.pos, goalCost, current.silverLeft, current.path, true});
            }
        }

        std::vector<Gateway> gateways = GetGateways(current.mapId);
        if (allowVipTeleport)
            AppendVipTeleportGateways(gateways, current.mapId);
        if (!availableItemTypes.empty())
            AppendItemGateways(gateways, current.mapId, availableItemTypes);

        for (auto& gw : gateways)
        {
            // Skip Market outbound Mark.Controller routes when Market is not the
            // origin. Instant gateways remain valid from Market.
            if (current.mapId == MAP_MARKET && from != MAP_MARKET && !gw.IsInstant())
                continue;
            if (gw.silverCost > current.silverLeft)
                continue;
            if (gw.type == GatewayType::Item
                && gw.destMapId == current.mapId
                && gw.HasDestPos()
                && ManhattanDist(current.pos, gw.destPos) < kMinSameMapItemGateDistance)
            {
                continue;
            }

            // Cost = walking distance from current position to this gateway.
            // VIP teleports leaving the origin map get a penalty so the pathfinder
            // prefers staying on the hero's current map when multiple routes exist.
            int stepCost = gw.IsInstant() ? 0 : ManhattanDist(current.pos, gw.pos);
            if (gw.type == GatewayType::VipTeleport && gw.destMapId != from)
                stepCost += 500;
            int newCost = current.walkCost + stepCost;
            uint32_t remainingSilver = current.silverLeft - gw.silverCost;

            OBJID dest = gw.destMapId;
            Position landingPos = gw.HasDestPos() ? gw.destPos : Position{0, 0};

            GwNodeKey key = GwNodeStateKey(dest, landingPos, remainingSilver);
            auto dit = bestCost.find(key);
            if (dit != bestCost.end() && newCost >= dit->second)
                continue;
            bestCost[key] = newCost;

            std::vector<Gateway> newPath = current.path;
            newPath.push_back(gw);

            open.push({dest, landingPos, newCost, remainingSilver, std::move(newPath)});
        }
    }

    return {}; // no path found
}
