#pragma once
#include "base.h"

class CRole;

class IPlugin {
public:
    virtual ~IPlugin() = default;
    virtual const char* GetName() const = 0;
    virtual void Update() = 0;
    virtual void RenderUI() = 0;
    virtual bool OnMapClick(const Position& tile) { return false; }

    // Per-entity render callbacks (optional overrides)
    virtual bool OnPreRenderEntity(CRole* entity) { return true; }
    virtual void OnPostRenderEntity(CRole* entity) {}

    bool m_enabled = true;
};
