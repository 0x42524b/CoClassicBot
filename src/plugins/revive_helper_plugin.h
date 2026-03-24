#pragma once
#include "plugin.h"

class ReviveHelperPlugin : public IPlugin {
public:
    const char* GetName() const override { return "Revive Helper"; }
    void Update() override {}
    void RenderUI() override;
    bool OnPreRenderEntity(CRole* entity) override;
};
