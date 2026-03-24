#include <windows.h>
#include <cstdio>
#include "overlay.h"
#include "discord.h"
#include "hooks.h"
#include "packets.h"
#include "game.h"
#include "config.h"
#include "plugin_mgr.h"
#include "itemtype.h"
#include "log.h"

ULONG64 g_qwModuleBase = 0;
HMODULE g_hModule = nullptr;

static DWORD WINAPI InitThread(LPVOID)
{
    // The game's login process sends an integrity/environment check to the server.
    // If we patch code or create D3D devices before login completes, the server
    // rejects the connection with "virtual machine detected!".
    //
    // Strategy: sit quietly until the hero pointer becomes valid (= logged in),
    // then do all initialization.

    AllocConsole();
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);

    Log::Init();
    spdlog::info("[init] Console attached, logger ready");

    // Install HWID spoof hooks early — before the game collects hardware
    // identifiers for the login packet.  These hook Windows system DLLs
    // (kernel32, iphlpapi), NOT game code, so they don't trigger the
    // server's VM/integrity check.
    // HwidSpoof::Init(g_hModule);

    Game::Init();   // benign - just reads the module base address

    // Poll for login completion - hero pointer exists early, but UID
    // is only assigned once the server confirms the login.
    spdlog::info("[init] Waiting for login...");
    while (true) {
        CHero* hero = Game::GetHero();
        if (hero && hero->GetID() > 0)
            break;
        Sleep(500);
    }

    // Player is logged in - safe to initialize everything now.
    spdlog::info("[init] DLL attached (post-login init)");
    spdlog::info("[init] Game base: 0x{:X}", (uintptr_t)Game::Base());

    CHero* hero = Game::GetHero();
    if (hero) {
        spdlog::info("[init] Hero: {} (ID={})", hero->GetName(), hero->GetID());
    }

    LoadConfig();
    LoadItemTypes();
    InitHooks();
    SetWhisperCallback([](const std::string& sender, const std::string& message) {
        const MiscSettings& misc = GetMiscSettings();
        if (!misc.whisperNotifyEnabled)
            return;
        CHero* hero = Game::GetHero();
        char buf[512];
        snprintf(buf, sizeof(buf), "[%s] Whisper from %s: %s",
                 hero ? hero->GetName() : "?", sender.c_str(), message.c_str());
        SendDiscordNotification(buf);
    });
    InitPacketHook();

    // Small extra delay ensures the game's D3D10 swapchain is stable
    Sleep(1000);
    InitOverlay();
    PluginManager::Get().Init();
    spdlog::info("[init] All systems initialized");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        spdlog::info("[shutdown] DLL detaching");
        PluginManager::Get().Shutdown();
        SaveConfig();
        ShutdownOverlay();
        CleanupPacketHook();
        CleanupHooks();
        // HwidSpoof::Shutdown();
        spdlog::info("[shutdown] Cleanup complete");
        Log::Shutdown();
        break;
    }

    return TRUE;
}
