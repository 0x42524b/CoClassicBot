cmake_minimum_required(VERSION 3.20)
project(coclassic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Vendor sources (compiled directly, warnings suppressed) ───────
set(DETOURS_SOURCES
    vendor/Detours/src/detours.cpp
    vendor/Detours/src/modules.cpp
    vendor/Detours/src/disasm.cpp
    vendor/Detours/src/image.cpp
    vendor/Detours/src/creatwth.cpp
    vendor/Detours/src/disolx86.cpp
    vendor/Detours/src/disolx64.cpp
    vendor/Detours/src/disolarm.cpp
    vendor/Detours/src/disolarm64.cpp
    vendor/Detours/src/disolia64.cpp
)
set(IMGUI_SOURCES
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_demo.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_dx10.cpp
    vendor/imgui/backends/imgui_impl_dx11.cpp
    vendor/imgui/backends/imgui_impl_win32.cpp
)
set_source_files_properties(${DETOURS_SOURCES} ${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0")

# ── Main DLL ────────────────────────────────────────────────────────
add_library(coclassic SHARED
    src/log.cpp
    src/dllmain.cpp
    src/hwid_spoof.cpp
    src/overlay.cpp
    src/CStatTable.cpp
    src/CRole.cpp
    src/CHero.cpp
    src/CEntityInfo.cpp
    src/CEntitySet.cpp
    src/CGameMap.cpp
    src/config.cpp
    src/inventory_utils.cpp
    src/npc_utils.cpp
    src/revive_utils.cpp
    src/discord.cpp
    src/gateway.cpp
    src/gfx.cpp
    src/hooks.cpp
    src/itemtype.cpp
    src/packets.cpp
    src/pathfinder.cpp
    src/plugins/plugin_mgr.cpp

    src/plugins/mining_plugin.cpp
    src/plugins/mule_plugin.cpp
    src/plugins/travel_plugin.cpp
    src/plugins/follow_plugin.cpp
    src/plugins/aim_helper_plugin.cpp
    src/plugins/revive_helper_plugin.cpp
    src/plugins/artisan_spammer_plugin.cpp
    src/plugins/skill_trainer_plugin.cpp
    ${DETOURS_SOURCES}
    ${IMGUI_SOURCES}
)
target_include_directories(coclassic PRIVATE
    src
    src/plugins
    vendor/Detours/src
    vendor/imgui
    vendor/imgui/backends
    vendor/json/include
    vendor/spdlog/include
)
target_link_libraries(coclassic PRIVATE
    d3d11.lib
    dxgi.lib
    d3dcompiler.lib
    winhttp.lib
)
set_target_properties(coclassic PROPERTIES
    OUTPUT_NAME "coclassic"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(injector
    injector/main.cpp
)
target_include_directories(injector PRIVATE
    vendor/json/include
)
target_link_libraries(injector PRIVATE
    ws2_32
)
set_target_properties(injector PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── Map tests ──────────────────────────────────────────────────────
add_executable(map_tests
    tests/map_tests.cpp
    src/CGameMap.cpp
)
target_include_directories(map_tests PRIVATE src)
set_target_properties(map_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
