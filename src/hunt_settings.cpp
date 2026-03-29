#include "hunt_settings.h"

const char* HuntSkillName(HuntSkillType type)
{
    switch (type) {
        case HuntSkillType::Superman: return "Superman";
        case HuntSkillType::Cyclone:  return "Cyclone";
        case HuntSkillType::Accuracy: return "Accuracy";
        case HuntSkillType::XpFly:    return "XP Fly";
        case HuntSkillType::Fly:      return "Stamina Fly";
        case HuntSkillType::Stigma:   return "Stigma";
        default:                      return "Unknown";
    }
}

void AutoHuntSettings::SyncSkillBoolsFromPriorities()
{
    castSuperman = false;
    castCyclone = false;
    castXpFly = false;
    castFly = false;
    useStigma = false;
    for (int i = 0; i < kHuntSkillCount; i++) {
        const auto& entry = skillPriorities[i];
        if (!entry.enabled) continue;
        switch (entry.type) {
            case HuntSkillType::Superman: castSuperman = true; break;
            case HuntSkillType::Cyclone:  castCyclone = true;  break;
            case HuntSkillType::XpFly:    castXpFly = true;    break;
            case HuntSkillType::Fly:      castFly = true;      break;
            case HuntSkillType::Stigma:   useStigma = true;    break;
            default: break;
        }
    }
}

static AutoHuntSettings g_autoHuntSettings;
AutoHuntSettings& GetAutoHuntSettings() { return g_autoHuntSettings; }

bool IsZeroPos(const Position& pos)
{
    return pos.x == 0 && pos.y == 0;
}

Position PolygonCentroid(const std::vector<Position>& points)
{
    if (points.empty())
        return {};

    long long sumX = 0;
    long long sumY = 0;
    for (const Position& point : points) {
        sumX += point.x;
        sumY += point.y;
    }

    return {
        (int)(sumX / (long long)points.size()),
        (int)(sumY / (long long)points.size())
    };
}

bool PointInPolygon(const Position& point, const std::vector<Position>& polygon)
{
    if (polygon.size() < 3)
        return false;

    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Position& a = polygon[i];
        const Position& b = polygon[j];
        const bool intersects =
            ((a.y > point.y) != (b.y > point.y)) &&
            (point.x < (b.x - a.x) * (point.y - a.y) / (double)(b.y - a.y) + a.x);
        if (intersects)
            inside = !inside;
    }

    return inside;
}

bool HasValidHuntZone(const AutoHuntSettings& settings)
{
    if (settings.zoneMapId == 0)
        return false;

    if (settings.zoneMode == AutoHuntZoneMode::Circle)
        return !IsZeroPos(settings.zoneCenter) && settings.zoneRadius > 0;

    return settings.zonePolygon.size() >= 3;
}

bool IsPointInHuntZone(const AutoHuntSettings& settings, OBJID mapId, const Position& pos)
{
    if (mapId != settings.zoneMapId)
        return false;

    if (settings.zoneMode == AutoHuntZoneMode::Circle) {
        if (IsZeroPos(settings.zoneCenter) || settings.zoneRadius <= 0)
            return false;
        return settings.zoneCenter.DistanceTo(pos) <= (float)settings.zoneRadius;
    }

    return PointInPolygon(pos, settings.zonePolygon);
}

Position GetHuntZoneAnchor(const AutoHuntSettings& settings)
{
    if (settings.zoneMode == AutoHuntZoneMode::Polygon)
        return PolygonCentroid(settings.zonePolygon);
    return settings.zoneCenter;
}
