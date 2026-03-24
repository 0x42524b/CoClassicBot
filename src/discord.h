#pragma once
#include <string>

struct DiscordSettings
{
    bool webhookEnabled = false;
    char webhookUrl[512] = "";
    char mentionUserId[64] = "";
};

DiscordSettings& GetDiscordSettings();

std::string JsonEscape(const std::string& text);
std::string SanitizeDiscordUserId(const char* text);
std::string BuildDiscordWebhookPayload(const std::string& content, const char* mentionUserId);
bool SendDiscordWebhookPayload(const std::string& webhookUrl, const std::string& jsonPayload);
void SendDiscordWebhookPayloadAsync(std::string webhookUrl, std::string jsonPayload);

// Convenience: send a message using the global DiscordSettings if configured.
// If mention is true, the configured mention user ID is included.
void SendDiscordNotification(const std::string& message, bool mention = true);

// Item acquisition tracking — call once per frame from the overlay loop.
// Detects newly acquired items and sends Discord notifications for tracked types.
void UpdateItemNotifications();
