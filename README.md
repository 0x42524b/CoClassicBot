# CoClassic

A C++ DLL injection bot and overlay for Classic Conquer 2.0 (ImConquer.exe, 64-bit). Uses [Microsoft Detours](https://github.com/microsoft/Detours) for function hooking and [Dear ImGui](https://github.com/ocornut/imgui) for the in-game overlay UI.

![img.png](ImConquer_CIxoWIfUTQ.png)
![img_1.png](ImConquer_xBNuEcy9cU.png)
## Prerequisites

- **Windows 10/11** (x64)
- **Visual Studio 2022** (v143 toolset) or **JetBrains Rider** with MSVC
- **CMake 3.20+** (if using the CMake build)
- C++20 support

All dependencies are vendored — no package manager needed.

## Building

### Visual Studio / Rider

1. Open `coclassic.sln`
2. Select **Release | x64**
3. Build the solution

### CMake

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Output

| Artifact | Path |
|----------|------|
| Main DLL | `build/bin/Release/coclassic.dll` |
| Injector | `build/bin/Release/injector.exe` |
| Tests | `build/bin/Release/map_tests.exe` |

> **Note:** If the linker fails with LNK1104 ("file in use"), the DLL is still injected in a running game process. Uninject or close the game before rebuilding.

## Usage

1. Build the solution (see above)
2. Launch `injector.exe` — it will find or start the game process and inject the DLL, OR use a DLL injector and inject after logging in.
3. Press **Insert** to toggle the overlay

The injector optionally supports a SOCKS5 relay setup and packet logging (see `injector/main.cpp` for details).

### Overlay Tabs

| Tab | Description |
|-----|-------------|
| **Player** | Hero stats, inventory, equipment |
| **Map** | Map info, tools, and the Travel section |
| **Plugins** | Per-plugin settings and controls |
| **Packets** | Live packet logger |

## Features

### Plugin System

Plugins are C++ classes implementing the `IPlugin` interface. They are compiled directly into the DLL — no dynamic loading.

| Plugin                       | Description |
|------------------------------|-------------|
| **Auto Hunt** (not included) | Hunting automation with zone selection, combat, loot, and storage |
| **Mining**                   | Mine-travel automation with warehouse storage and mule trading |
| **Mule**                     | Market trade helper that accepts trades from whitelisted players |
| **Travel**                   | Cross-map travel via portals, NPCs, and VIP teleport gateways |
| **Follow**                   | Follow a target player with mob dodging and pathfinding |
| **Aim Helper**               | Draws cross markers at entity jump destinations |
| **Revive Helper**            | Guild dead filter — suppresses rendering of non-dead entities |
| **Artisan Spammer**          | Skill spamming automation |
| **Skill Trainer**            | Automated skill training with casting, sitting, and potions |

### Core Systems

- **Overlay** — Hooks `IDXGISwapChain::Present` for ImGui rendering on the game's D3D10.1 device
- **Entity Hooks** — `RenderEntityVisual` hook dispatches per-entity callbacks to plugins
- **Pathfinder** — Singleton jump-by-waypoint path executor with stuck detection
- **Gateway Graph** — Dijkstra pathfinding through inter-map portals and gateways
- **Config** — Per-character INI persistence with autosave
- **Packet Logger** — Hooks `CNetClient::SendMsg` to log outbound packets
- **Discord Webhooks** — Whisper notification forwarding

## Project Structure

```
coclassic/
├── src/                    # Main DLL source
│   ├── dllmain.cpp         # Entry point and initialization
│   ├── overlay.cpp/h       # ImGui overlay and Present hook
│   ├── hooks.cpp/h         # Entity render hook dispatcher
│   ├── game.h              # Game accessors and offsets
│   ├── pathfinder.cpp/h    # Path execution service
│   ├── gateway.cpp/h       # Inter-map gateway graph
│   ├── config.cpp/h        # INI settings persistence
│   ├── packets.cpp/h       # Packet logger
│   ├── C*.h/cpp            # Game struct overlays (CRole, CHero, CGameMap, etc.)
│   └── plugins/            # Plugin implementations
│       ├── plugin.h         # IPlugin interface
│       ├── plugin_mgr.cpp/h # Plugin manager singleton
│       └── *_plugin.cpp/h   # Individual plugins
├── injector/               # Standalone injector executable
│   └── main.cpp
├── tests/                  # Unit tests
│   └── map_tests.cpp
├── vendor/                 # Vendored dependencies
│   ├── Detours/            # Microsoft Detours
│   ├── imgui/              # Dear ImGui (D3D10 + Win32 backends)
│   ├── json/               # nlohmann/json
│   └── spdlog/             # spdlog logging
├── coclassic.sln           # Visual Studio solution
├── coclassic.vcxproj       # Main DLL project
└── CMakeLists.txt          # CMake build configuration
```

## Contributing

### Adding a Plugin

1. Create `src/plugins/my_plugin.h` and `src/plugins/my_plugin.cpp`
2. Implement the `IPlugin` interface (`GetName`, `Update`, `RenderUI`, and optionally `OnPreRenderEntity`/`OnPostRenderEntity`)
3. Add a `std::make_unique<MyPlugin>()` call in `PluginManager::Init()`
4. Add the source files to both `coclassic.vcxproj` and `CMakeLists.txt`

### Adding a Game Struct

1. Create a dedicated header (e.g., `src/CMyStruct.h`) with `#pragma pack(push, 1)`
2. Define named fields at the correct offsets, using padding arrays for gaps
3. Add `static_assert(offsetof(...))` checks for every field
4. Never use raw pointer arithmetic — always define proper struct overlays

### Conventions

- Use official game class names: `CRole`, `CHero`, `CMapObj`, `CGameMap`, `CRoleMgr`
- Members use `m_` prefix (`m_id`, `m_posMap`, `m_deqRole`)
- Use standard library types (`std::string`, `std::vector`, `std::shared_ptr`) directly in struct overlays — the game uses the same MSVC ABI
- `Ref<T>` = `std::shared_ptr<T>` for game objects

### Reverse Engineering

This project works with a Themida-packed binary analyzed through a Scylla-dumped copy in Ghidra. When discovering new offsets or struct layouts:

1. Verify findings through Cheat Engine before committing to code
2. Rename functions and data labels in Ghidra when identified
3. Note any Themida-protected (VM'd) functions — static analysis won't work on those

### Build Notes

- The game uses **D3D10.1**, not D3D11. The overlay hooks the shared `IDXGISwapChain::Present` from DXGI and uses the ImGui D3D10 backend.
- Rider may report `"isSuccess": true` even when the linker fails. Use CLI MSBuild for reliable diagnostics:
  ```
  MSBuild.exe coclassic.sln -t:Rebuild -p:Configuration=Release -p:Platform=x64
  ```

## Dependencies

| Library | Purpose | Location |
|---------|---------|----------|
| [Microsoft Detours](https://github.com/microsoft/Detours) | Function hooking | `vendor/Detours/` |
| [Dear ImGui](https://github.com/ocornut/imgui) | Overlay UI (D3D10 + Win32) | `vendor/imgui/` |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing | `vendor/json/` |
| [spdlog](https://github.com/gabime/spdlog) | Logging | `vendor/spdlog/` |

System libraries: `d3d11.lib`, `dxgi.lib`, `d3dcompiler.lib`, `winhttp.lib`, `ws2_32.lib`

## Limitations
The following functionalities were removed from the source code:
- HWID Spoofing (Login packet AntiCheat) removed
- Auto-Hunt plugin removed, but can be easily built using the existing APIs