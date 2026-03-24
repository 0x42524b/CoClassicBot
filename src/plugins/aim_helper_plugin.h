#pragma once
#include "plugin.h"

struct AimHelperSettings
{
    bool  enabled        = false;
    bool  showPlayers    = true;
    bool  showMonsters   = false;
    bool  ignoreGuild    = false;
    int   markerSize     = 8;     // arm length in pixels
    int   markerThickness = 2;    // bar half-width in pixels
    float color[4]       = { 1.0f, 0.0f, 0.0f, 1.0f };  // RGBA for ImGui
};

AimHelperSettings& GetAimSettings();

class AimHelperPlugin : public IPlugin {
public:
    const char* GetName() const override { return "Aim Helper"; }
    void Update() override {}
    void RenderUI() override;
    void OnPostRenderEntity(CRole* entity) override;
};
