#include "overlay.h"
#include "discord.h"
#include "game.h"
#include "hooks.h"
#include "itemtype.h"
#include "packets.h"
#include "config.h"
#include "pathfinder.h"
#include "plugin_mgr.h"
#include "gateway.h"
#include "itemtype.h"
#include "CEntitySet.h"
#include "CEntityInfo.h"
#include "plugins/travel_plugin.h"
#include "log.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d10_1.h>
#include <dxgi.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx10.h"
#include "imgui_impl_win32.h"

#include <detours.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// =====================================================================
// Draw isometric diamond tiles on the minimap
// =====================================================================
static void DrawMapCells(ImDrawList* dl, CellInfo* cells, int mapW, int mapH,
                         float camX, float camY, int viewRadius, float fs,
                         float centerX, float centerY,
                         float clipL, float clipT, float clipR, float clipB)
{
    int camIX = (int)camX;
    int camIY = (int)camY;
    for (int tdy = -viewRadius; tdy <= viewRadius; tdy++) {
        int tileY = camIY + tdy;
        if (tileY < 0 || tileY >= mapH) continue;
        for (int tdx = -viewRadius; tdx <= viewRadius; tdx++) {
            int tileX = camIX + tdx;
            if (tileX < 0 || tileX >= mapW) continue;

            float cx = centerX + ((float)tileX - camX - ((float)tileY - camY)) * fs;
            float cy = centerY + ((float)tileX - camX + ((float)tileY - camY)) * fs;

            if (cx + fs < clipL || cx - fs > clipR) continue;
            if (cy + fs < clipT || cy - fs > clipB) continue;

            CellInfo& cell = cells[tileX + tileY * mapW];
            uint16_t mask = CGameMap::GetMask(&cell);
            ImU32 col;
            if (mask == 1)
                col = IM_COL32(40, 40, 40, 255);       // blocked
            else if (mask != 0)
                col = IM_COL32(180, 60, 220, 255);      // portal / special
            else
                col = IM_COL32(60, 100, 60, 255);       // walkable

            dl->AddQuadFilled(
                ImVec2(cx, cy - fs),
                ImVec2(cx + fs, cy),
                ImVec2(cx, cy + fs),
                ImVec2(cx - fs, cy),
                col);
        }
    }
}

// =====================================================================
// State
// =====================================================================
static bool                    g_showOverlay   = true;
static bool                    g_initialized   = false;

// ── Map camera state ──
static float g_mapCamX = 0.0f;   // camera center (tile coords)
static float g_mapCamY = 0.0f;
static bool  g_mapDragging = false;
static float g_mapDragStartMouseX = 0.0f;
static float g_mapDragStartMouseY = 0.0f;
static float g_mapDragStartCamX = 0.0f;
static float g_mapDragStartCamY = 0.0f;

// Entity filter (shared between minimap + entity table)
// 0=All, 1=NPCs, 2=Players, 3=Monsters, 4=Items
static int g_entityFilter = 0;

// Game window + WndProc
static HWND                    g_hGameWnd      = nullptr;
static WNDPROC                 g_origWndProc   = nullptr;

// D3D10 device (the game's actual device)
static ID3D10Device*           g_pDevice       = nullptr;

// Present hook
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
static PresentFn OrigPresent = nullptr;

// =====================================================================
// WndProc subclass on GAME window — forward input to ImGui
// =====================================================================
static LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Toggle with Insert
    if (msg == WM_KEYDOWN && wParam == VK_INSERT) {
        g_showOverlay = !g_showOverlay;
        spdlog::debug("[overlay] {}", g_showOverlay ? "shown" : "hidden");
        return 0;
    }

    // Feed input to ImGui when visible
    if (g_showOverlay) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return 0;

        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse && (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST))
            return 0;
        if (io.WantCaptureKeyboard && (msg >= WM_KEYFIRST && msg <= WM_KEYLAST))
            return 0;
    }

    return CallWindowProcW(g_origWndProc, hWnd, msg, wParam, lParam);
}

// =====================================================================
// Find IDXGISwapChain::Present address via dummy device
// =====================================================================
static uintptr_t FindPresentAddress()
{
    // Register temp window class
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "coclassic_dummy";
    RegisterClassExA(&wc);

    // Create 1x1 hidden window
    HWND hDummy = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hDummy) {
        spdlog::error("[overlay] CreateWindowEx for dummy failed");
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return 0;
    }

    // Create dummy D3D11 device + swapchain (just to find Present vtable address)
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 1;
    sd.BufferDesc.Height = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hDummy;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* dummyDevice = nullptr;
    IDXGISwapChain* dummySwapChain = nullptr;
    ID3D11DeviceContext* dummyCtx = nullptr;
    D3D_FEATURE_LEVEL fl;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &dummySwapChain, &dummyDevice, &fl, &dummyCtx);

    if (FAILED(hr)) {
        spdlog::error("[overlay] Dummy D3D11CreateDeviceAndSwapChain: 0x{:08X}", (unsigned long)hr);
        DestroyWindow(hDummy);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return 0;
    }

    // Read vtable: Present is at index 8
    void** vtable = *(void***)dummySwapChain;
    uintptr_t presentAddr = (uintptr_t)vtable[8];
    spdlog::info("[overlay] Present address: 0x{:X}", presentAddr);

    // Cleanup dummy resources
    dummyCtx->Release();
    dummySwapChain->Release();
    dummyDevice->Release();
    DestroyWindow(hDummy);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);

    return presentAddr;
}

// =====================================================================
// Present hook — renders ImGui into the game's back buffer
// =====================================================================
static HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* pSwapChain, UINT sync, UINT flags)
{
    // ── Lazy init (first call only) ──
    if (!g_initialized) {
        g_initialized = true;
        spdlog::info("[overlay] Present hook firing - initializing ImGui (D3D10 backend)");

        // Get the game's D3D10 device from the swapchain
        HRESULT hr = pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&g_pDevice);
        if (FAILED(hr) || !g_pDevice) {
            spdlog::error("[overlay] GetDevice(ID3D10Device) failed: 0x{:08X}", (unsigned long)hr);
            return OrigPresent(pSwapChain, sync, flags);
        }

        // Get game HWND from swapchain desc
        DXGI_SWAP_CHAIN_DESC desc{};
        pSwapChain->GetDesc(&desc);
        g_hGameWnd = desc.OutputWindow;
        spdlog::info("[overlay] D3D10 Device=0x{:X}, HWND=0x{:X}", (uintptr_t)g_pDevice, (uintptr_t)g_hGameWnd);

        // Init ImGui with D3D10 backend
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();

        ImGui_ImplWin32_Init(g_hGameWnd);
        ImGui_ImplDX10_Init(g_pDevice);

        // Subclass game WndProc for input forwarding
        g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hGameWnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
        spdlog::info("[overlay] Initialized (WndProc=0x{:X})", (uintptr_t)g_origWndProc);
    }

    if (!g_pDevice) return OrigPresent(pSwapChain, sync, flags);

    // ── Background logic (runs every frame, even with overlay hidden) ──
    {
        CHero* hero = Game::GetHero();
        if (hero) {
            UpdateCharacterConfigBinding();
            Pathfinder::Get().Update();
            PluginManager::Get().UpdateAll();
            UpdateItemNotifications();
            MaybeAutoSaveConfig();
        }
    }

    // ── Render ImGui frame ──
    if (g_showOverlay) {
        // Create a fresh RTV from the current back buffer each frame
        ID3D10Texture2D* backBuffer = nullptr;
        HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&backBuffer);
        if (FAILED(hr) || !backBuffer)
            return OrigPresent(pSwapChain, sync, flags);

        ID3D10RenderTargetView* rtv = nullptr;
        hr = g_pDevice->CreateRenderTargetView(backBuffer, nullptr, &rtv);
        backBuffer->Release();
        if (FAILED(hr) || !rtv)
            return OrigPresent(pSwapChain, sync, flags);

        ImGui_ImplDX10_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Bot control panel
        ImGui::SetNextWindowSize(ImVec2(420, 400), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("CoClassic Bot", &g_showOverlay)) {
            ImGui::Text("Press INSERT to toggle overlay.");
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            if (ImGui::Button("Save Settings"))
                SaveConfig();
            ImGui::SameLine();
            ImGui::TextDisabled("Autosaves shortly after changes");
            ImGui::Separator();

            CHero* hero = Game::GetHero();
            if (!hero) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                    "Waiting for player entity...");
                ImGui::Text("(Log in to a character first)");
            } else {
                if (ImGui::BeginTabBar("##tabs")) {
                // ── Player tab ──
                if (ImGui::BeginTabItem("Player")) {
                    constexpr ImGuiTreeNodeFlags kPlayerSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
                    const uint32_t silver = hero->GetSilver();
                    const uint64_t silverRuntime = hero->GetSilverRuntimeValue();
                    if (ImGui::CollapsingHeader("Overview", kPlayerSectionFlags)) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Player: %s", hero->GetName());
                    ImGui::Text("UID:      %u", hero->GetID());
                    ImGui::Text("Position: (%d, %d)", hero->m_posMap.x, hero->m_posMap.y);
                    ImGui::Text("World:    (%d, %d)", hero->m_posWorld.x, hero->m_posWorld.y);
                    ImGui::Text("Screen:   (%d, %d)", hero->m_posScr.x, hero->m_posScr.y);
                    ImGui::Text("Status:   0x%llX", (unsigned long long)hero->m_nStatusFlag);
                    ImGui::Text("Silver:   %u", silver);
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", hero->HasTrustedSilverCache() ? "(server update)" : "(cached fallback)");
                    ImGui::Text("A30 Raw:  %llu", (unsigned long long)silverRuntime);

                    if (hero->HasSyndicate()) {
                        auto* entSet = CEntitySet::GetInstance();
                        const char* guildName = entSet ? entSet->GetSyndicateName(hero->m_idSyndicate) : nullptr;
                        const char* rankName = GetSyndicateRankName(hero->m_nSyndicateRank);
                        ImGui::Text("Guild:   ");
                        ImGui::SameLine(0, 0);
                        ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "%s", guildName ? guildName : "Unknown");
                        if (rankName[0]) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(%s)", rankName);
                        }
                    }

                    if (hero->IsDead())
                        ImGui::TextColored(ImVec4(1, 0, 0, 1), "** DEAD **");

                    CRoleMgr* mgr = Game::GetRoleMgr();
                    if (mgr && !mgr->m_deqRole.empty() && mgr->m_deqRole.size() < 10000) {
                        int playerCount = 0, monsterCount = 0, otherCount = 0;
                        for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; i++) {
                            auto& ref = mgr->m_deqRole[i];
                            if (!ref) continue;
                            CRole* e = ref.get();
                            if (e->IsPlayer())       playerCount++;
                            else if (e->IsMonster()) monsterCount++;
                            else                     otherCount++;
                        }
                        ImGui::Text("Nearby:   %d players, %d monsters, %d NPCs",
                                    playerCount, monsterCount, otherCount);
                    }

                    // ── Equipment (collapsible) ──
                    }
                    if (ImGui::CollapsingHeader("Equipment")) {
                        if (ImGui::BeginTable("##equip", 6,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_Resizable,
                                ImVec2(0, 0))) {
                            ImGui::TableSetupColumn("Slot",    ImGuiTableColumnFlags_WidthFixed, 65.0f);
                            ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Quality", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                            ImGui::TableSetupColumn("+",       ImGuiTableColumnFlags_WidthFixed, 25.0f);
                            ImGui::TableSetupColumn("Dur",     ImGuiTableColumnFlags_WidthFixed, 55.0f);
                            ImGui::TableSetupColumn("Sockets", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                            ImGui::TableHeadersRow();

                            for (int s = 0; s < EquipSlot::COUNT; s++) {
                                CItem* eq = hero->GetEquip(s);
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::Text("%s", GetEquipSlotName(s));
                                ImGui::TableNextColumn();
                                if (eq)
                                    ImGui::Text("%s", eq->GetName());
                                else
                                    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "-");
                                ImGui::TableNextColumn();
                                if (eq) {
                                    int q = eq->GetQuality();
                                    ImVec4 qc = (q >= ItemQuality::SUPER) ? ImVec4(1,0.8f,0,1) :
                                                (q >= ItemQuality::ELITE) ? ImVec4(0.6f,0.4f,1,1) :
                                                (q >= ItemQuality::UNIQUE) ? ImVec4(0.2f,0.8f,1,1) :
                                                ImVec4(1,1,1,1);
                                    ImGui::TextColored(qc, "%s", eq->GetQualityName());
                                }
                                ImGui::TableNextColumn();
                                if (eq && eq->GetPlus() > 0)
                                    ImGui::Text("+%d", eq->GetPlus());
                                ImGui::TableNextColumn();
                                if (eq)
                                    ImGui::Text("%d/%d", eq->GetDurability(), eq->GetMaxDurability());
                                ImGui::TableNextColumn();
                                if (eq) {
                                    if (eq->HasSocket1() || eq->HasSocket2()) {
                                        ImGui::Text("%s%s%s",
                                            GetGemClassName(eq->GetGem1()),
                                            eq->HasSocket2() ? ", " : "",
                                            eq->HasSocket2() ? GetGemClassName(eq->GetGem2()) : "");
                                    }
                                }
                            }
                            ImGui::EndTable();
                        }
                    }

                    // ── Inventory (collapsible) ──
                    if (ImGui::CollapsingHeader("Inventory")) {
                        ImGui::Text("Items: %zu / %d",
                                    hero->m_deqItem.size(), CHero::MAX_BAG_ITEMS);

                        if (hero->m_deqItem.empty()) {
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Inventory is empty.");
                        } else if (ImGui::BeginTable("##inv", 6,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                                ImVec2(0, 200.0f))) {
                            ImGui::TableSetupScrollFreeze(0, 1);
                            ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed, 25.0f);
                            ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Quality", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                            ImGui::TableSetupColumn("+",       ImGuiTableColumnFlags_WidthFixed, 25.0f);
                            ImGui::TableSetupColumn("Dur",     ImGuiTableColumnFlags_WidthFixed, 55.0f);
                            ImGui::TableSetupColumn("Sockets", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                            ImGui::TableHeadersRow();

                            for (size_t i = 0; i < hero->m_deqItem.size() && i < 40; i++) {
                                auto& ref = hero->m_deqItem[i];
                                if (!ref) continue;
                                CItem* item = ref.get();

                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::Text("%zu", i + 1);
                                ImGui::TableNextColumn();
                                ImGui::Text("%s", item->GetName());
                                ImGui::TableNextColumn();
                                {
                                    int q = item->GetQuality();
                                    ImVec4 qc = (q >= ItemQuality::SUPER) ? ImVec4(1,0.8f,0,1) :
                                                (q >= ItemQuality::ELITE) ? ImVec4(0.6f,0.4f,1,1) :
                                                (q >= ItemQuality::UNIQUE) ? ImVec4(0.2f,0.8f,1,1) :
                                                ImVec4(1,1,1,1);
                                    ImGui::TextColored(qc, "%s", item->GetQualityName());
                                }
                                ImGui::TableNextColumn();
                                if (item->GetPlus() > 0)
                                    ImGui::Text("+%d", item->GetPlus());
                                ImGui::TableNextColumn();
                                ImGui::Text("%d/%d", item->GetDurability(), item->GetMaxDurability());
                                ImGui::TableNextColumn();
                                if (item->HasSocket1() || item->HasSocket2()) {
                                    ImGui::Text("%s%s%s",
                                        GetGemClassName(item->GetGem1()),
                                        item->HasSocket2() ? ", " : "",
                                        item->HasSocket2() ? GetGemClassName(item->GetGem2()) : "");
                                }
                            }
                            ImGui::EndTable();
                        }
                    }

                    // ── Skills (collapsible) ──
                    if (ImGui::CollapsingHeader("Skills")) {
                        if (hero->m_vecMagic.empty()) {
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No skills learned.");
                        } else if (hero->m_vecMagic.size() < 200 &&
                                   ImGui::BeginTable("##skills", 6,
                                       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                                       ImVec2(0, 200.0f))) {
                            ImGui::TableSetupScrollFreeze(0, 1);
                            ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Lv",       ImGuiTableColumnFlags_WidthFixed, 25.0f);
                            ImGui::TableSetupColumn("MP",       ImGuiTableColumnFlags_WidthFixed, 40.0f);
                            ImGui::TableSetupColumn("Stam",     ImGuiTableColumnFlags_WidthFixed, 40.0f);
                            ImGui::TableSetupColumn("Dist",     ImGuiTableColumnFlags_WidthFixed, 35.0f);
                            ImGui::TableSetupColumn("Exp",      ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableHeadersRow();

                            for (size_t i = 0; i < hero->m_vecMagic.size(); i++) {
                                auto& ref = hero->m_vecMagic[i];
                                if (!ref) continue;
                                CMagic* magic = ref.get();

                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                if (magic->IsXpSkill()) {
                                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "[XP]");
                                    ImGui::SameLine();
                                }
                                ImGui::Text("%s", magic->GetName());
                                ImGui::TableNextColumn();
                                ImGui::Text("%u", magic->GetLevel());
                                ImGui::TableNextColumn();
                                if (magic->GetMpCost() > 0)
                                    ImGui::Text("%u", magic->GetMpCost());
                                ImGui::TableNextColumn();
                                if (magic->GetStaminaCost() > 0)
                                    ImGui::Text("%u", magic->GetStaminaCost());
                                ImGui::TableNextColumn();
                                if (magic->GetDistance() > 0)
                                    ImGui::Text("%u", magic->GetDistance());
                                ImGui::TableNextColumn();
                                if (magic->GetExpRequired() > 0) {
                                    float progress = (float)magic->GetExp() / (float)magic->GetExpRequired();
                                    if (progress > 1.0f) progress = 1.0f;
                                    char overlay[32];
                                    snprintf(overlay, sizeof(overlay), "%u/%u",
                                             magic->GetExp(), magic->GetExpRequired());
                                    ImGui::ProgressBar(progress, ImVec2(-1, 0), overlay);
                                }
                            }
                            ImGui::EndTable();
                        }
                    }
                    ImGui::EndTabItem();
                }

                // ── Map tab (minimap + travel + entity table) ──
                if (ImGui::BeginTabItem("Map")) {
                    CGameMap* map = Game::GetMap();
                    CRoleMgr* mgr = Game::GetRoleMgr();
                    bool hasMgr = mgr && !mgr->m_deqRole.empty() && mgr->m_deqRole.size() < 10000;
                    bool hasMap = map && map->m_sizeMap.iWidth > 0;

                    MapSettings& ms = GetMapSettings();
                    constexpr ImGuiTreeNodeFlags kMapSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;

                    // ── Map info header ──
                    OBJID curMapId = map ? map->GetId() : 0;
                    int heroTileX = hero->m_posMap.x;
                    int heroTileY = hero->m_posMap.y;
                    auto& gateways = GetGateways(curMapId);
                    if (ImGui::CollapsingHeader("Overview", kMapSectionFlags)) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 1.0f, 1.0f),
                        "Map: %s  (ID: %u)", GetMapName(curMapId), curMapId);
                    if (map) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                            "  %dx%d", map->m_sizeMap.iWidth, map->m_sizeMap.iHeight);
                    }

                    // Hero position with copy button
                    ImGui::Text("Hero: (%d, %d)", heroTileX, heroTileY);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Copy##pos")) {
                        char posBuf[64];
                        snprintf(posBuf, sizeof(posBuf), "{%d,%d}", heroTileX, heroTileY);
                        ImGui::SetClipboardText(posBuf);
                    }

                    // Show known gateways for current map
                    if (!gateways.empty()) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                            "  [%zu gateways]", gateways.size());
                    }

                    ImGui::Checkbox("Entities", &ms.showEntities);
                    ImGui::SameLine();
                    ImGui::Checkbox("Follow", &ms.followHero);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset View")) {
                        ms.followHero = true;
                        ms.cellSize = 1.0f;
                        g_mapCamX = (float)hero->m_posMap.x;
                        g_mapCamY = (float)hero->m_posMap.y;
                    }
                    if (hasMap) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Dump Map")) {
                            char dumpPath[MAX_PATH];
                            snprintf(dumpPath, sizeof(dumpPath), "mapdump_%u.bin", curMapId);
                            if (map->DumpToFile(dumpPath))
                                spdlog::info("[map] Dumped to {}", dumpPath);
                            else
                                spdlog::error("[map] Failed to dump map");
                        }
                    }
                    }

                    if (auto* travel = PluginManager::Get().GetPlugin<TravelPlugin>()) {
                        if (ImGui::CollapsingHeader("Travel", kMapSectionFlags)) {
                            travel->RenderUI();
                        }
                    }

                    // Snapshot map state
                    CellInfo* cells   = map ? map->m_pCellInfo : nullptr;
                    int       mapW_t  = map ? map->m_sizeMap.iWidth  : 0;
                    int       mapH_t  = map ? map->m_sizeMap.iHeight : 0;
                    bool      mapOk   = cells && mapW_t > 0 && mapH_t > 0
                                        && mapW_t < 10000 && mapH_t < 10000;

                    if (ImGui::CollapsingHeader("Minimap", kMapSectionFlags)) {
                    if (mapOk) {
                        int heroX = hero->m_posMap.x;
                        int heroY = hero->m_posMap.y;
                        float fs  = ms.cellSize;

                        // ── Compute camera center ──
                        float camTileX, camTileY;
                        if (ms.followHero) {
                            camTileX = (float)heroX;
                            camTileY = (float)heroY;
                        } else {
                            camTileX = g_mapCamX;
                            camTileY = g_mapCamY;
                        }

                        // Sync camera state so switching to free-cam starts from current view
                        if (ms.followHero) {
                            g_mapCamX = camTileX;
                            g_mapCamY = camTileY;
                        }

                        // Canvas: use all available space (entity table is collapsible)
                        float availW = ImGui::GetContentRegionAvail().x;
                        float availH = ImGui::GetContentRegionAvail().y;
                        float canvasW = availW;
                        float canvasH = availH;
                        if (canvasH < 80.0f) canvasH = 80.0f;

                        // Auto-fit fs so the entire map diamond fills the canvas
                        float mapSpan = (float)(mapW_t + mapH_t);
                        float autoFitFs = (mapSpan > 0.0f) ? fminf(canvasW, canvasH) / mapSpan : 1.0f;
                        fs = autoFitFs;

                        // Effective radius: enough to cover the entire map from any camera position
                        int maxDim = (mapW_t > mapH_t) ? mapW_t : mapH_t;
                        int effectiveRadius = maxDim + 1;

                        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
                        ImGui::InvisibleButton("##mapcanvas", ImVec2(canvasW, canvasH));
                        bool canvasHovered = ImGui::IsItemHovered();
                        bool canvasClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                        ImVec2 mousePos = ImGui::GetIO().MousePos;
                        ImDrawList* dl = ImGui::GetWindowDrawList();

                        // ── Scroll wheel zoom ──
                        if (canvasHovered) {
                            float wheel = ImGui::GetIO().MouseWheel;
                            if (wheel != 0.0f) {
                                float factor = 1.0f + wheel * 0.15f;
                                ms.cellSize *= factor;
                                if (ms.cellSize < 0.2f) ms.cellSize = 0.2f;
                                if (ms.cellSize > 20.0f) ms.cellSize = 20.0f;
                                fs = autoFitFs * ms.cellSize;
                            }
                        }

                        // Apply user zoom on top of auto-fit baseline
                        fs = autoFitFs * ms.cellSize;

                        // ── Right-click drag panning ──
                        if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                            g_mapDragging = true;
                            g_mapDragStartMouseX = mousePos.x;
                            g_mapDragStartMouseY = mousePos.y;
                            g_mapDragStartCamX = g_mapCamX;
                            g_mapDragStartCamY = g_mapCamY;
                            if (ms.followHero) ms.followHero = false;
                        }
                        if (g_mapDragging) {
                            if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                                float dx = (mousePos.x - g_mapDragStartMouseX) / fs;
                                float dy = (mousePos.y - g_mapDragStartMouseY) / fs;
                                g_mapCamX = g_mapDragStartCamX - (dx + dy) * 0.5f;
                                g_mapCamY = g_mapDragStartCamY - (dy - dx) * 0.5f;
                                camTileX = g_mapCamX;
                                camTileY = g_mapCamY;
                            } else {
                                g_mapDragging = false;
                            }
                        }

                        ImVec2 canvasEnd(canvasPos.x + canvasW, canvasPos.y + canvasH);
                        dl->PushClipRect(canvasPos, canvasEnd, true);
                        dl->AddRectFilled(canvasPos, canvasEnd, IM_COL32(20, 20, 20, 255));

                        float centerX = canvasPos.x + canvasW * 0.5f;
                        float centerY = canvasPos.y + canvasH * 0.5f;

                        // Draw isometric diamond tiles
                        DrawMapCells(dl, cells, mapW_t, mapH_t,
                                     camTileX, camTileY, effectiveRadius, fs,
                                     centerX, centerY,
                                     canvasPos.x, canvasPos.y,
                                     canvasEnd.x, canvasEnd.y);

                        // Draw entity dots on the minimap
                        if (ms.showEntities) {
                            // Roles (NPCs, players, monsters)
                            if (hasMgr && g_entityFilter != 4) {
                                GuildSettings& guild = GetGuildSettings();
                                bool deadFilter = guild.showDeadOnly && hero->HasSyndicate();
                                for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; i++) {
                                    auto& ref = mgr->m_deqRole[i];
                                    if (!ref) continue;
                                    CRole* e = ref.get();
                                    if (e->GetID() == hero->GetID()) continue;

                                    bool isPlayer = e->IsPlayer();
                                    bool isMonster = e->IsMonster();
                                    bool isNpc = !isPlayer && !isMonster;

                                    if (g_entityFilter == 1 && !isNpc) continue;
                                    if (g_entityFilter == 2 && !isPlayer) continue;
                                    if (g_entityFilter == 3 && !isMonster) continue;

                                    if (deadFilter) {
                                        if (!isPlayer || e->m_idSyndicate != hero->m_idSyndicate) continue;
                                        if (!e->IsDead() && !e->TestState(USERSTATUS_GHOST)) continue;
                                    }

                                    float edx = (float)e->m_posMap.x - camTileX;
                                    float edy = (float)e->m_posMap.y - camTileY;
                                    if (edx > effectiveRadius || edx < -effectiveRadius) continue;
                                    if (edy > effectiveRadius || edy < -effectiveRadius) continue;

                                    const bool isDead = e->IsDead() || e->TestState(USERSTATUS_GHOST);
                                    ImU32 entCol;
                                    if (isDead && (isPlayer || isMonster))
                                                        entCol = IM_COL32(140, 140, 140, 180);
                                    else if (isPlayer)  entCol = IM_COL32(80, 180, 255, 255);
                                    else if (isMonster) entCol = IM_COL32(255, 80, 80, 255);
                                    else                entCol = IM_COL32(255, 255, 100, 255);

                                    float cx = centerX + (edx - edy) * fs;
                                    float cy = centerY + (edx + edy) * fs;
                                    dl->AddCircleFilled(ImVec2(cx, cy), fs * 0.8f, entCol);
                                }
                            }

                            // Ground items
                            if (hasMap && (g_entityFilter == 0 || g_entityFilter == 4)) {
                                for (size_t i = 0; i < map->m_vecItems.size() && i < 500; i++) {
                                    auto& item = map->m_vecItems[i];
                                    if (!item) continue;

                                    float edx = (float)item->m_pos.x - camTileX;
                                    float edy = (float)item->m_pos.y - camTileY;
                                    if (edx > effectiveRadius || edx < -effectiveRadius) continue;
                                    if (edy > effectiveRadius || edy < -effectiveRadius) continue;

                                    float cx = centerX + (edx - edy) * fs;
                                    float cy = centerY + (edx + edy) * fs;
                                    dl->AddCircleFilled(ImVec2(cx, cy), fs * 0.8f,
                                        IM_COL32(220, 160, 255, 255)); // purple for items
                                }
                            }
                        }

                        // ── Draw gateway markers on minimap ──
                        for (auto& gw : gateways) {
                            float gdx = (float)gw.pos.x - camTileX;
                            float gdy = (float)gw.pos.y - camTileY;
                            if (gdx > effectiveRadius || gdx < -effectiveRadius) continue;
                            if (gdy > effectiveRadius || gdy < -effectiveRadius) continue;

                            float gcx = centerX + (gdx - gdy) * fs;
                            float gcy = centerY + (gdx + gdy) * fs;

                            ImU32 gwCol = (gw.type == GatewayType::Portal)
                                ? IM_COL32(0, 200, 255, 220)    // cyan for portals
                                : IM_COL32(255, 200, 0, 220);   // yellow for NPCs

                            // Diamond marker
                            float gs = fs * 1.5f;
                            dl->AddQuad(
                                ImVec2(gcx, gcy - gs),
                                ImVec2(gcx + gs, gcy),
                                ImVec2(gcx, gcy + gs),
                                ImVec2(gcx - gs, gcy),
                                gwCol, 2.0f);
                        }

                        // Hero dot (at hero's actual position, not necessarily center)
                        {
                            float hdx = (float)heroX - camTileX;
                            float hdy = (float)heroY - camTileY;
                            float hsx = centerX + (hdx - hdy) * fs;
                            float hsy = centerY + (hdx + hdy) * fs;
                            dl->AddCircleFilled(ImVec2(hsx, hsy), fs + 1.0f,
                                IM_COL32(255, 255, 255, 255));
                        }

                        // ── Draw path waypoints on minimap ──
                        {
                            auto& pf = Pathfinder::Get();
                            auto& waypoints = pf.GetWaypoints();
                            size_t wpIdx = pf.GetCurrentIndex();
                            if (pf.IsActive() && wpIdx < waypoints.size()) {
                                float h_dx = (float)heroX - camTileX;
                                float h_dy = (float)heroY - camTileY;
                                ImVec2 prev(centerX + (h_dx - h_dy) * fs,
                                            centerY + (h_dx + h_dy) * fs);
                                for (size_t wi = wpIdx; wi < waypoints.size(); wi++) {
                                    float wdx = (float)waypoints[wi].x - camTileX;
                                    float wdy = (float)waypoints[wi].y - camTileY;
                                    float wcx = centerX + (wdx - wdy) * fs;
                                    float wcy = centerY + (wdx + wdy) * fs;
                                    dl->AddLine(prev, ImVec2(wcx, wcy),
                                        IM_COL32(0, 255, 128, 180), 2.0f);
                                    dl->AddCircleFilled(ImVec2(wcx, wcy),
                                        fs * 0.6f, IM_COL32(0, 255, 128, 220));
                                    prev = ImVec2(wcx, wcy);
                                }
                            }
                        }

                        // ── Click-to-jump / pathfind ──
                        // Inverse isometric: screen → tile (relative to camera)
                        if (canvasClicked) {
                            float dx = (mousePos.x - centerX) / fs;
                            float dy = (mousePos.y - centerY) / fs;
                            float ftdx = (dx + dy) * 0.5f;
                            float ftdy = (dy - dx) * 0.5f;
                            int tileX = (int)roundf(camTileX + ftdx);
                            int tileY = (int)roundf(camTileY + ftdy);

                            if (!PluginManager::Get().HandleMapClick({tileX, tileY})) {
                                // Cancel any active path on manual click
                                Pathfinder::Get().Stop();

                                if (map->CanJump(heroX, heroY, tileX, tileY, CGameMap::GetHeroAltThreshold())
                                    && !IsTileOccupied(tileX, tileY)) {
                                    // Direct jump
                                    hero->Jump(tileX, tileY);
                                } else if (map->IsWalkable(tileX, tileY)) {
                                    // Pathfind: A* tile path → simplify into jumps
                                    auto tilePath = map->FindPath(heroX, heroY, tileX, tileY, 1000000);
                                    if (!tilePath.empty()) {
                                        auto waypoints = map->SimplifyPath(tilePath);
                                        if (!waypoints.empty()) {
                                            Pathfinder::Get().StartPath(
                                                waypoints,
                                                900);
                                        }
                                    }
                                }
                            }
                        }

                        // ── Hover tooltip ──
                        if (canvasHovered) {
                            float dx = (mousePos.x - centerX) / fs;
                            float dy = (mousePos.y - centerY) / fs;
                            float ftdx = (dx + dy) * 0.5f;
                            float ftdy = (dy - dx) * 0.5f;
                            int tileX = (int)roundf(camTileX + ftdx);
                            int tileY = (int)roundf(camTileY + ftdy);
                            int dist = CGameMap::TileDist(heroX, heroY, tileX, tileY);
                            bool occupied = IsTileOccupied(tileX, tileY);
                            bool canJump = map->CanJump(heroX, heroY, tileX, tileY, CGameMap::GetHeroAltThreshold())
                                           && !occupied;

                            // Highlight hovered tile
                            float htdx = (float)tileX - camTileX;
                            float htdy = (float)tileY - camTileY;
                            float hcx = centerX + (htdx - htdy) * fs;
                            float hcy = centerY + (htdx + htdy) * fs;
                            ImU32 hlCol = canJump
                                ? IM_COL32(255, 255, 255, 80)
                                : IM_COL32(255, 0, 0, 80);
                            dl->AddQuadFilled(
                                ImVec2(hcx, hcy - fs),
                                ImVec2(hcx + fs, hcy),
                                ImVec2(hcx, hcy + fs),
                                ImVec2(hcx - fs, hcy),
                                hlCol);

                            // Detailed tooltip with cell info
                            CellInfo* hoverCell = map->GetCell(tileX, tileY);
                            uint16_t mask = hoverCell ? CGameMap::GetMask(hoverCell) : 0;
                            uint16_t terrain = hoverCell ? CGameMap::GetTerrain(hoverCell) : 0;
                            int16_t alt = hoverCell ? CGameMap::GetAltitude(hoverCell) : 0;
                            bool walkable = map->IsWalkable(tileX, tileY);

                            const char* reason = "";
                            if (!walkable)
                                reason = " [blocked]";
                            else if (canJump)
                                reason = "";
                            else if (dist > CGameMap::MAX_JUMP_DIST
                                     || !map->CanReach(heroX, heroY, tileX, tileY))
                                reason = " [pathfind]";
                            else if (occupied)
                                reason = " [occupied]";

                            ImGui::BeginTooltip();
                            ImGui::Text("(%d, %d) dist=%d%s",
                                tileX, tileY, dist, reason);
                            ImGui::Text("terrain=%u mask=%u alt=%d",
                                terrain, mask, alt);
                            ImGui::EndTooltip();
                        }

                        dl->PopClipRect();

                        // ── Gateway table for current map ──
                        if (!gateways.empty() && ImGui::TreeNode("Gateways##gwlist")) {
                            for (size_t gi = 0; gi < gateways.size(); gi++) {
                                auto& gw = gateways[gi];
                                const char* typeStr = (gw.type == GatewayType::Portal)
                                    ? "Portal" : "NPC";
                                if (gw.IsIntraMap())
                                    typeStr = "Warp";
                                int gdist = CGameMap::TileDist(heroX, heroY,
                                    gw.pos.x, gw.pos.y);
                                if (gw.HasDestPos()) {
                                    ImGui::Text("[%zu] %s (%d,%d) -> (%d,%d) %s  dist=%d",
                                        gi, typeStr, gw.pos.x, gw.pos.y,
                                        gw.destPos.x, gw.destPos.y,
                                        gw.IsIntraMap() ? "" : GetMapName(gw.destMapId),
                                        gdist);
                                } else {
                                    ImGui::Text("[%zu] %s (%d,%d) -> %s  dist=%d",
                                        gi, typeStr, gw.pos.x, gw.pos.y,
                                        GetMapName(gw.destMapId), gdist);
                                }
                            }
                            ImGui::TreePop();
                        }

                    } else {
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                            "Map data not available.");
                    }

                    // ── Entity table (collapsible) ──
                    }
                    if (ImGui::CollapsingHeader("Entities##table")) {
                        ImGui::Text("Show:");
                        ImGui::SameLine();
                        ImGui::RadioButton("All", &g_entityFilter, 0);
                        ImGui::SameLine();
                        ImGui::RadioButton("NPCs", &g_entityFilter, 1);
                        ImGui::SameLine();
                        ImGui::RadioButton("Players", &g_entityFilter, 2);
                        ImGui::SameLine();
                        ImGui::RadioButton("Monsters", &g_entityFilter, 3);
                        ImGui::SameLine();
                        ImGui::RadioButton("Items", &g_entityFilter, 4);

                        bool noData = !hasMgr && !hasMap;
                        if (noData) {
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                "No entities nearby.");
                        } else if (ImGui::BeginTable("##ent", 6,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                                ImVec2(0, 200.0f))) {
                            ImGui::TableSetupScrollFreeze(0, 1);
                            ImGui::TableSetupColumn("ID",    ImGuiTableColumnFlags_WidthFixed, 80.0f);
                            ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Guild", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                            ImGui::TableSetupColumn("Type",  ImGuiTableColumnFlags_WidthFixed, 70.0f);
                            ImGui::TableSetupColumn("Pos",   ImGuiTableColumnFlags_WidthFixed, 100.0f);
                            ImGui::TableSetupColumn("Dist",  ImGuiTableColumnFlags_WidthFixed, 50.0f);
                            ImGui::TableHeadersRow();

                            // Roles (NPCs, players, monsters)
                            if (hasMgr && g_entityFilter != 4) {
                                GuildSettings& guild = GetGuildSettings();
                                bool deadFilter = guild.showDeadOnly && hero->HasSyndicate();
                                for (size_t i = 0; i < mgr->m_deqRole.size() && i < 500; i++) {
                                    auto& ref = mgr->m_deqRole[i];
                                    if (!ref) continue;
                                    CRole* e = ref.get();

                                    bool isPlayer = e->IsPlayer();
                                    bool isMonster = e->IsMonster();
                                    bool isNpc = !isPlayer && !isMonster;

                                    if (g_entityFilter == 1 && !isNpc) continue;
                                    if (g_entityFilter == 2 && !isPlayer) continue;
                                    if (g_entityFilter == 3 && !isMonster) continue;

                                    if (deadFilter) {
                                        if (!isPlayer || e->m_idSyndicate != hero->m_idSyndicate) continue;
                                        if (!e->IsDead() && !e->TestState(USERSTATUS_GHOST)) continue;
                                    }

                                    ImGui::PushID((int)i);

                                    const char* type = "NPC";
                                    ImVec4 typeColor(1.0f, 1.0f, 0.4f, 1.0f);
                                    if (isPlayer) {
                                        type = "Player";
                                        typeColor = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
                                    } else if (isMonster) {
                                        type = "Monster";
                                        typeColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                                    }

                                    float dist = hero->m_posMap.DistanceTo(e->m_posMap);

                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%u", e->GetID());
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%s", e->GetName());
                                    ImGui::TableNextColumn();
                                    if (e->HasSyndicate()) {
                                        auto* entSet = CEntitySet::GetInstance();
                                        const char* guildName = entSet ? entSet->GetSyndicateName(e->m_idSyndicate) : nullptr;
                                        const char* rankName = GetSyndicateRankName(e->m_nSyndicateRank);
                                        if (guildName && rankName[0])
                                            ImGui::Text("%s [%s]", guildName, rankName);
                                        else if (guildName)
                                            ImGui::Text("%s", guildName);
                                        else
                                            ImGui::Text("ID:%u", e->m_idSyndicate);
                                    }
                                    ImGui::TableNextColumn();
                                    ImGui::TextColored(typeColor, "%s", type);
                                    ImGui::TableNextColumn();
                                    ImGui::Text("(%d, %d)", e->m_posMap.x, e->m_posMap.y);
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%.0f", dist);
                                    ImGui::PopID();
                                }
                            }

                            // Ground items
                            if (hasMap && (g_entityFilter == 0 || g_entityFilter == 4)) {
                                for (size_t i = 0; i < map->m_vecItems.size() && i < 500; i++) {
                                    auto& item = map->m_vecItems[i];
                                    if (!item) continue;

                                    ImGui::PushID((int)(10000 + i));

                                    std::string name = FormatItemName(item->m_idType, item->GetPlus());
                                    int q = item->GetQuality();
                                    ImVec4 qc = (q >= ItemQuality::SUPER)  ? ImVec4(1,0.8f,0,1) :
                                                (q >= ItemQuality::ELITE)  ? ImVec4(0.6f,0.4f,1,1) :
                                                (q >= ItemQuality::UNIQUE) ? ImVec4(0.2f,0.8f,1,1) :
                                                (q >= ItemQuality::REFINED)? ImVec4(0.4f,1,0.4f,1) :
                                                ImVec4(1,1,1,1);
                                    float dist = hero->m_posMap.DistanceTo(item->m_pos);

                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%u", item->m_id);
                                    ImGui::TableNextColumn();
                                    ImGui::TextColored(qc, "%s", name.c_str());
                                    ImGui::TableNextColumn();
                                    // no guild for items
                                    ImGui::TableNextColumn();
                                    ImGui::TextColored(ImVec4(0.86f,0.63f,1,1), "Item");
                                    ImGui::TableNextColumn();
                                    ImGui::Text("(%d, %d)", item->m_pos.x, item->m_pos.y);
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%.0f", dist);
                                    ImGui::PopID();
                                }
                            }

                            ImGui::EndTable();
                        }
                    }
                    ImGui::EndTabItem();
                }

                // ── Packets tab ──
                if (ImGui::BeginTabItem("Packets")) {
                    PacketLog& plog = GetPacketLog();
                    constexpr ImGuiTreeNodeFlags kPacketSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;

                    if (ImGui::CollapsingHeader("Controls", kPacketSectionFlags)) {
                    ImGui::Checkbox("Logging", &plog.enabled);
                    ImGui::SameLine();
                    if (ImGui::Button("Clear"))
                        plog.Clear();
                    ImGui::SameLine();
                    ImGui::Text("(%zu packets)", plog.Count());
                    }

                    if (ImGui::CollapsingHeader("Log", kPacketSectionFlags)) {
                    if (plog.Count() == 0) {
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                            "No packets captured yet.");
                    } else {
                        if (ImGui::BeginChild("##pktscroll", ImVec2(0, 0), ImGuiChildFlags_None,
                                ImGuiWindowFlags_HorizontalScrollbar)) {
                            for (size_t i = 0; i < plog.Count(); i++) {
                                const PacketEntry& pkt = plog.Get(i);

                                // Build full hex dump string (reused for expand & copy)
                                std::string fullDump;
                                for (size_t r = 0; r < pkt.data.size(); r += 16) {
                                    char line[128] = {};
                                    int p = snprintf(line, sizeof(line), "%04X  ", (unsigned)r);
                                    for (size_t c = 0; c < 16; c++) {
                                        if (r + c < pkt.data.size())
                                            p += snprintf(line + p, sizeof(line) - p,
                                                          "%02X ", pkt.data[r + c]);
                                        else
                                            p += snprintf(line + p, sizeof(line) - p, "   ");
                                        if (c == 7)
                                            p += snprintf(line + p, sizeof(line) - p, " ");
                                    }
                                    p += snprintf(line + p, sizeof(line) - p, " ");
                                    for (size_t c = 0; c < 16 && r + c < pkt.data.size(); c++) {
                                        uint8_t b = pkt.data[r + c];
                                        line[p++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
                                    }
                                    line[p] = '\0';
                                    if (!fullDump.empty()) fullDump += '\n';
                                    fullDump += line;
                                }

                                // Header label: click to expand, right-click to copy
                                char label[128];
                                snprintf(label, sizeof(label),
                                         "[%zu] Type=0x%04X  Size=%u##pkt%zu",
                                         i, pkt.msgType, pkt.rawSize, i);

                                bool open = ImGui::TreeNode(label);

                                // Right-click copies entire hex dump to clipboard
                                if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                                    ImGui::SetClipboardText(fullDump.c_str());
                                }
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("Right-click to copy hex dump");
                                }

                                if (open) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.8f, 1.0f, 1.0f));
                                    ImGui::TextUnformatted(fullDump.c_str());
                                    ImGui::PopStyleColor();
                                    ImGui::TreePop();
                                }
                            }
                        }
                        ImGui::EndChild();
                    }
                    }
                    ImGui::EndTabItem();
                }

                // ── Misc tab ──
                if (ImGui::BeginTabItem("Misc")) {
                    constexpr ImGuiTreeNodeFlags kSectionFlags = ImGuiTreeNodeFlags_DefaultOpen;

                    if (ImGui::CollapsingHeader("Discord Webhook", kSectionFlags)) {
                        DiscordSettings& discord = GetDiscordSettings();
                        ImGui::Checkbox("Enable Discord Webhook", &discord.webhookEnabled);
                        ImGui::InputText("Webhook URL##discord", discord.webhookUrl, IM_ARRAYSIZE(discord.webhookUrl));
                        ImGui::InputText("Mention User ID##discord", discord.mentionUserId, IM_ARRAYSIZE(discord.mentionUserId));
                        if (ImGui::Button("Test Webhook")) {
                            if (discord.webhookUrl[0] != '\0') {
                                CHero* heroTest = Game::GetHero();
                                char msg[256];
                                if (heroTest)
                                    snprintf(msg, sizeof(msg), "[%s] Discord webhook test.", heroTest->GetName());
                                else
                                    snprintf(msg, sizeof(msg), "[coclassic] Discord webhook test.");
                                std::string payload = BuildDiscordWebhookPayload(msg, discord.mentionUserId);
                                SendDiscordWebhookPayloadAsync(discord.webhookUrl, std::move(payload));
                            }
                        }
                        ImGui::TextDisabled("Shared webhook used by all notification features.");
                    }

                    if (ImGui::CollapsingHeader("Whisper Notifications", kSectionFlags)) {
                        MiscSettings& misc = GetMiscSettings();
                        ImGui::Checkbox("Notify on Whisper", &misc.whisperNotifyEnabled);
                        ImGui::TextDisabled("Sends a Discord notification when another player whispers you.");
                    }

                    if (ImGui::CollapsingHeader("Loot Drop Notifications", kSectionFlags)) {
                        MiscSettings& misc = GetMiscSettings();
                        ImGui::Checkbox("Notify on Loot Drop", &misc.lootDropNotifyEnabled);
                        ImGui::TextDisabled("Sends a Discord notification when an item drops from your kill.");
                    }

                    if (ImGui::CollapsingHeader("Item Notifications", kSectionFlags)) {
                        MiscSettings& misc = GetMiscSettings();
                        ImGui::Checkbox("Enable Item Notifications", &misc.itemNotifyEnabled);
                        ImGui::TextDisabled("Sends a Discord notification when a tracked item enters your inventory.");

                        if (ImGui::SmallButton("Clear Notify Items"))
                            misc.notifyItemIds.clear();
                        ImGui::SameLine();
                        ImGui::Text("Tracked: %d", (int)misc.notifyItemIds.size());

                        static char notifyItemSearch[128] = "";
                        ImGui::InputText("Search##notifyitem", notifyItemSearch, sizeof(notifyItemSearch));

                        std::string searchLower;
                        for (const char* p = notifyItemSearch; *p; ++p)
                            searchLower.push_back((char)std::tolower((unsigned char)*p));

                        ImGui::BeginChild("##notifyitembrowser", ImVec2(0, 260.0f), ImGuiChildFlags_Borders);
                        if (ImGui::BeginTable("##notifyitemtable", 4,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                ImVec2(0, 0))) {
                            ImGui::TableSetupScrollFreeze(0, 1);
                            ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                            ImGui::TableSetupColumn("Notify", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                            ImGui::TableSetupColumn("Mention", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                            ImGui::TableHeadersRow();

                            int shown = 0;
                            for (const ItemTypeInfo* info : GetAllItemTypes()) {
                                if (!info)
                                    continue;

                                if (!searchLower.empty()) {
                                    std::string nameLower;
                                    for (char c : info->name)
                                        nameLower.push_back((char)std::tolower((unsigned char)c));
                                    if (nameLower.find(searchLower) == std::string::npos
                                        && std::to_string(info->id).find(notifyItemSearch) == std::string::npos)
                                        continue;
                                }

                                const bool isNotify = std::find(misc.notifyItemIds.begin(),
                                    misc.notifyItemIds.end(), info->id) != misc.notifyItemIds.end();
                                const bool isMention = std::find(misc.mentionItemIds.begin(),
                                    misc.mentionItemIds.end(), info->id) != misc.mentionItemIds.end();

                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::Text("%s", info->name.c_str());
                                ImGui::TableNextColumn();
                                ImGui::Text("%u", info->id);

                                ImGui::TableNextColumn();
                                {
                                    char btnId[64];
                                    snprintf(btnId, sizeof(btnId), "%s##miscnotify%u",
                                             isNotify ? "Remove" : "Add", info->id);
                                    if (ImGui::SmallButton(btnId)) {
                                        if (isNotify) {
                                            misc.notifyItemIds.erase(
                                                std::remove(misc.notifyItemIds.begin(),
                                                            misc.notifyItemIds.end(), info->id),
                                                misc.notifyItemIds.end());
                                            misc.mentionItemIds.erase(
                                                std::remove(misc.mentionItemIds.begin(),
                                                            misc.mentionItemIds.end(), info->id),
                                                misc.mentionItemIds.end());
                                        } else {
                                            misc.notifyItemIds.push_back(info->id);
                                        }
                                    }
                                }

                                ImGui::TableNextColumn();
                                if (isNotify) {
                                    char btnId[64];
                                    snprintf(btnId, sizeof(btnId), "%s##miscmention%u",
                                             isMention ? "Remove" : "Add", info->id);
                                    if (ImGui::SmallButton(btnId)) {
                                        if (isMention)
                                            misc.mentionItemIds.erase(
                                                std::remove(misc.mentionItemIds.begin(),
                                                            misc.mentionItemIds.end(), info->id),
                                                misc.mentionItemIds.end());
                                        else
                                            misc.mentionItemIds.push_back(info->id);
                                    }
                                }

                                if (searchLower.empty() && ++shown >= 250)
                                    break;
                            }

                            ImGui::EndTable();
                        }
                        ImGui::EndChild();
                    }

                    ImGui::EndTabItem();
                }

                // ── Plugin tabs ──
                PluginManager::Get().RenderAllUI();

                ImGui::EndTabBar();
            }
            } // else (hero valid)
        }
        ImGui::End();

        // ── Render ──
        ImGui::Render();
        g_pDevice->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX10_RenderDrawData(ImGui::GetDrawData());
        rtv->Release();
    }

    return OrigPresent(pSwapChain, sync, flags);
}

// =====================================================================
// Public API
// =====================================================================
void InitOverlay()
{
    uintptr_t presentAddr = FindPresentAddress();
    if (!presentAddr) {
        spdlog::error("[overlay] Failed to find Present address");
        return;
    }

    OrigPresent = (PresentFn)presentAddr;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)OrigPresent, HkPresent);
    LONG err = DetourTransactionCommit();

    spdlog::info("[overlay] Present hook @ 0x{:X}: {}", presentAddr, err == NO_ERROR ? "OK" : "FAILED");
}

void ShutdownOverlay()
{
    // Restore WndProc
    if (g_origWndProc && g_hGameWnd) {
        SetWindowLongPtrW(g_hGameWnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
        g_origWndProc = nullptr;
    }

    // Unhook Present
    if (OrigPresent) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)OrigPresent, HkPresent);
        DetourTransactionCommit();
        OrigPresent = nullptr;
    }

    // Shutdown ImGui
    if (g_initialized) {
        ImGui_ImplDX10_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_initialized = false;
    }

    spdlog::info("[overlay] Shutdown complete");
}

bool IsOverlayVisible()
{
    return g_showOverlay;
}
