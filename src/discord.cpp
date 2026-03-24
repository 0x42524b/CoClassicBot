#include "discord.h"
#include "config.h"
#include "CHero.h"
#include "game.h"
#include "itemtype.h"
#include "log.h"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <thread>
#include <unordered_map>
#include <cctype>

// SDB msg: [14:12:18.757] [info] [chat] ch=0x7DB style=0 color=0x00FF0000 sender='SYSTEM' msg='A Super DragonBall has dropped from a monster killed by IWinAgain'

static DiscordSettings g_discordSettings;
DiscordSettings& GetDiscordSettings() { return g_discordSettings; }

static std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
        return {};

    int wideLength = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    UINT codePage = CP_UTF8;
    if (wideLength <= 0) {
        codePage = CP_ACP;
        wideLength = MultiByteToWideChar(codePage, 0, text.c_str(), -1, nullptr, 0);
    }
    if (wideLength <= 1)
        return {};

    std::wstring wide((size_t)wideLength, L'\0');
    MultiByteToWideChar(codePage, 0, text.c_str(), -1, wide.data(), wideLength);
    wide.resize((size_t)wideLength - 1);
    return wide;
}

std::string JsonEscape(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size() + 32);
    for (unsigned char ch : text) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"':  escaped += "\\\""; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    escaped += buf;
                } else {
                    escaped.push_back((char)ch);
                }
                break;
        }
    }
    return escaped;
}

std::string SanitizeDiscordUserId(const char* text)
{
    std::string userId;
    if (!text)
        return userId;

    while (*text) {
        const unsigned char ch = (unsigned char)*text++;
        if (std::isdigit(ch))
            userId.push_back((char)ch);
    }
    return userId;
}

std::string BuildDiscordWebhookPayload(const std::string& content, const char* mentionUserId)
{
    const std::string userId = SanitizeDiscordUserId(mentionUserId);
    const std::string message = userId.empty() ? content : "<@" + userId + "> " + content;

    std::string payload = "{\"content\":\"";
    payload += JsonEscape(message);
    payload += "\"";
    if (!userId.empty()) {
        payload += ",\"allowed_mentions\":{\"users\":[\"";
        payload += userId;
        payload += "\"]}";
    }
    payload += "}";
    return payload;
}

bool SendDiscordWebhookPayload(const std::string& webhookUrl, const std::string& jsonPayload)
{
    std::wstring url = Utf8ToWide(webhookUrl);
    if (url.empty())
        return false;

    URL_COMPONENTS components = {};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = (DWORD)-1;
    components.dwHostNameLength = (DWORD)-1;
    components.dwUrlPathLength = (DWORD)-1;
    components.dwExtraInfoLength = (DWORD)-1;

    std::wstring mutableUrl = url;
    if (!WinHttpCrackUrl(mutableUrl.data(), 0, 0, &components))
        return false;

    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring objectName(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0)
        objectName.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    if (host.empty() || objectName.empty())
        return false;

    HINTERNET session = WinHttpOpen(L"coclassic/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session)
        return false;

    WinHttpSetTimeouts(session, 3000, 3000, 5000, 5000);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    const DWORD requestFlags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect,
        L"POST",
        objectName.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        requestFlags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    static constexpr wchar_t kJsonHeaders[] = L"Content-Type: application/json\r\n";
    const DWORD payloadSize = (DWORD)jsonPayload.size();
    const BOOL sendOk = WinHttpSendRequest(request,
        kJsonHeaders,
        (DWORD)-1L,
        payloadSize > 0 ? (LPVOID)jsonPayload.data() : nullptr,
        payloadSize,
        payloadSize,
        0);
    bool success = false;
    if (sendOk && WinHttpReceiveResponse(request, nullptr)) {
        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        if (WinHttpQueryHeaders(request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusCodeSize,
                WINHTTP_NO_HEADER_INDEX)) {
            success = statusCode >= 200 && statusCode < 300;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return success;
}

void SendDiscordWebhookPayloadAsync(std::string webhookUrl, std::string jsonPayload)
{
    std::thread([webhookUrl = std::move(webhookUrl), jsonPayload = std::move(jsonPayload)]() mutable {
        SendDiscordWebhookPayload(webhookUrl, jsonPayload);
    }).detach();
}

void SendDiscordNotification(const std::string& message, bool mention)
{
    const DiscordSettings& settings = GetDiscordSettings();
    if (!settings.webhookEnabled || settings.webhookUrl[0] == '\0')
        return;

    const char* mentionId = mention ? settings.mentionUserId : "";
    std::string payload = BuildDiscordWebhookPayload(message, mentionId);
    SendDiscordWebhookPayloadAsync(settings.webhookUrl, std::move(payload));
}

// =====================================================================
// Item acquisition tracking
// =====================================================================
static std::unordered_map<OBJID, uint32_t> g_lastInventory;
static OBJID g_trackedHeroId = 0;

void UpdateItemNotifications()
{
    const MiscSettings& misc = GetMiscSettings();
    if (!misc.itemNotifyEnabled || misc.notifyItemIds.empty())
        return;

    CHero* hero = Game::GetHero();
    if (!hero || hero->GetID() == 0)
        return;

    if (g_trackedHeroId != hero->GetID()) {
        g_lastInventory.clear();
        g_trackedHeroId = hero->GetID();
        for (const auto& itemRef : hero->m_deqItem) {
            if (itemRef)
                g_lastInventory[itemRef->GetID()] = itemRef->GetTypeID();
        }
        return;
    }

    std::unordered_map<OBJID, uint32_t> currentInventory;
    currentInventory.reserve(hero->m_deqItem.size());

    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef)
            continue;

        const uint32_t typeId = itemRef->GetTypeID();
        currentInventory[itemRef->GetID()] = typeId;

        if (g_lastInventory.find(itemRef->GetID()) == g_lastInventory.end()) {
            if (std::find(misc.notifyItemIds.begin(), misc.notifyItemIds.end(), typeId)
                != misc.notifyItemIds.end()) {
                const bool mention = std::find(misc.mentionItemIds.begin(),
                    misc.mentionItemIds.end(), typeId) != misc.mentionItemIds.end();
                const std::string itemName = FormatItemName(typeId, itemRef->GetPlus());
                const CGameMap* map = Game::GetMap();
                char msg[512];
                snprintf(msg, sizeof(msg), "[%s] Acquired %s (%u) on map %u at (%d,%d). Bag %d/%d.",
                         hero->GetName(), itemName.c_str(), typeId,
                         map ? map->GetId() : 0,
                         hero->m_posMap.x, hero->m_posMap.y,
                         (int)hero->m_deqItem.size(), CHero::MAX_BAG_ITEMS);
                SendDiscordNotification(msg, mention);
            }
        }
    }

    g_lastInventory = std::move(currentInventory);
}
