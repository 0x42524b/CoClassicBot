#pragma once
#include "plugin.h"
#include "base.h"
#include <vector>
#include <memory>

class PluginManager {
public:
    void Init();
    void UpdateAll();
    void RenderAllUI();
    void Shutdown();
    static PluginManager& Get();

    bool PreRenderEntity(CRole* entity);
    void PostRenderEntity(CRole* entity);
    bool HandleMapClick(const Position& tile);

    template <typename T>
    T* GetPlugin()
    {
        for (auto& plugin : m_plugins) {
            if (auto* typed = dynamic_cast<T*>(plugin.get()))
                return typed;
        }
        return nullptr;
    }

    const std::vector<std::unique_ptr<IPlugin>>& GetPlugins() const { return m_plugins; }

private:
    std::vector<std::unique_ptr<IPlugin>> m_plugins;
    int m_selectedPlugin = -2;  // -2 = Hunting composite view
};
