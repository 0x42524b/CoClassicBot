#include "artisan_spammer_plugin.h"
#include "game.h"
#include "CHero.h"
#include "CItem.h"
#include "packets.h"
#include "log.h"
#include "imgui.h"
#include <algorithm>
#include <string>

// =====================================================================
// Varint writer (same encoding as packets.cpp)
// =====================================================================
static int WriteVarint(uint8_t* buf, uint32_t value)
{
    int n = 0;
    while (value > 0x7F) {
        buf[n++] = static_cast<uint8_t>(value & 0x7F) | 0x80;
        value >>= 7;
    }
    buf[n++] = static_cast<uint8_t>(value);
    return n;
}

// =====================================================================
// Packet type 0x03F1 — Artisan compose/upgrade
//
// Field 1 (0x08): target item instance ID
// Field 2 (0x10): material item instance ID
// Field 5 (0x28): action (19 = upgrade quality via DB, 20 = improve via Meteor/Scroll)
// =====================================================================
bool ArtisanSpammerPlugin::SendArtisanPacket(OBJID targetItemId, OBJID materialItemId, uint32_t action)
{
    uint8_t buf[32] = {};
    int off = 4; // skip header

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, targetItemId);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, materialItemId);

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, action);

    // Write header: [u16 size][u16 type]
    *reinterpret_cast<uint16_t*>(buf)     = static_cast<uint16_t>(off);
    *reinterpret_cast<uint16_t*>(buf + 2) = 0x03F1;

    return SendPacket(buf, off);
}

// =====================================================================
// Material matching helper
// =====================================================================
static bool IsMaterialMatch(CItem* item, int materialType)
{
    switch (materialType) {
        case 0: return item->IsMeteor();
        case 1: return item->IsMeteorScroll();
        case 2: return item->IsDragonBall();
        default: return false;
    }
}

// Action 19 = upgrade quality (DB), 20 = improve (Meteor/MeteorScroll)
static uint32_t GetArtisanAction(int materialType)
{
    return materialType == 2 ? 19 : 20;
}

// =====================================================================
// Update — drains the paired queue one packet per tick
// =====================================================================
void ArtisanSpammerPlugin::Update()
{
    if (!m_spamming || m_queue.empty())
        return;

    const DWORD now = GetTickCount();
    if (now - m_lastSendTick < static_cast<DWORD>(m_delayMs))
        return;

    auto [targetId, materialId] = m_queue.back();
    m_queue.pop_back();

    if (SendArtisanPacket(targetId, materialId, GetArtisanAction(m_materialType))) {
        m_sentCount++;
        spdlog::debug("[artisan] Sent {}/{} target={} mat={}",
            m_sentCount, m_totalCount, targetId, materialId);
    }

    m_lastSendTick = now;

    if (m_queue.empty()) {
        m_spamming = false;
        spdlog::info("[artisan] Done — sent {} packets", m_sentCount);
    }
}

// =====================================================================
// UI
// =====================================================================
void ArtisanSpammerPlugin::RenderUI()
{
    CHero* hero = Game::GetHero();
    if (!hero) {
        ImGui::TextDisabled("No hero");
        return;
    }

    // -- Build unique equipment type list (grouped by name) --
    struct EquipType { std::string name; int count; };
    std::vector<EquipType> equipTypes;

    for (auto& pi : hero->m_deqItem) {
        CItem* item = pi.get();
        if (!item || !item->IsEquipment()) continue;
        const char* name = item->GetName();
        auto it = std::find_if(equipTypes.begin(), equipTypes.end(),
            [name](const EquipType& e) { return e.name == name; });
        if (it != equipTypes.end())
            it->count++;
        else
            equipTypes.push_back({ name, 1 });
    }

    // -- Target item type combo --
    if (ImGui::CollapsingHeader("Target Item", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (equipTypes.empty()) {
            ImGui::TextDisabled("No equipment in inventory");
            m_selectedTarget = -1;
        } else {
            if (m_selectedTarget >= (int)equipTypes.size())
                m_selectedTarget = -1;

            const char* preview = m_selectedTarget >= 0
                ? equipTypes[m_selectedTarget].name.c_str()
                : "-- Select --";

            if (ImGui::BeginCombo("Item##target", preview)) {
                for (int i = 0; i < (int)equipTypes.size(); i++) {
                    char label[64];
                    snprintf(label, sizeof(label), "%s (x%d)##%d",
                        equipTypes[i].name.c_str(), equipTypes[i].count, i);
                    if (ImGui::Selectable(label, m_selectedTarget == i))
                        m_selectedTarget = i;
                }
                ImGui::EndCombo();
            }

            if (m_selectedTarget >= 0)
                ImGui::Text("Items: %d", equipTypes[m_selectedTarget].count);
        }
    }

    // -- Material type --
    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::RadioButton("Meteor", &m_materialType, 0);
        ImGui::SameLine();
        ImGui::RadioButton("MeteorScroll", &m_materialType, 1);
        ImGui::SameLine();
        ImGui::RadioButton("DragonBall", &m_materialType, 2);

        int matCount = 0;
        for (auto& pi : hero->m_deqItem) {
            CItem* it = pi.get();
            if (it && IsMaterialMatch(it, m_materialType))
                matCount++;
        }
        ImGui::Text("Available: %d", matCount);
    }

    // -- Delay --
    ImGui::SliderInt("Delay (ms)", &m_delayMs, 0, 1000);

    ImGui::Separator();

    // -- Spam button --
    if (m_spamming) {
        ImGui::Text("Sending %d / %d ...", m_sentCount, m_totalCount);
        if (ImGui::Button("Stop")) {
            m_spamming = false;
            m_queue.clear();
        }
    } else {
        const bool canSpam = m_selectedTarget >= 0
                          && m_selectedTarget < (int)equipTypes.size();

        if (!canSpam) ImGui::BeginDisabled();
        if (ImGui::Button("Spam All")) {
            const std::string& targetName = equipTypes[m_selectedTarget].name;

            // Collect matching equipment item IDs
            std::vector<OBJID> targetIds;
            for (auto& pi : hero->m_deqItem) {
                CItem* it = pi.get();
                if (it && it->IsEquipment() && it->GetName() == targetName)
                    targetIds.push_back(it->GetID());
            }

            // Collect matching material item IDs
            std::vector<OBJID> materialIds;
            for (auto& pi : hero->m_deqItem) {
                CItem* it = pi.get();
                if (it && IsMaterialMatch(it, m_materialType))
                    materialIds.push_back(it->GetID());
            }

            // Zip 1:1 — min(targets, materials) pairs
            const int pairCount = (std::min)((int)targetIds.size(), (int)materialIds.size());
            m_queue.clear();
            for (int i = 0; i < pairCount; i++)
                m_queue.push_back({ targetIds[i], materialIds[i] });

            if (!m_queue.empty()) {
                m_totalCount = (int)m_queue.size();
                m_sentCount = 0;
                m_lastSendTick = 0;
                m_spamming = true;
                spdlog::info("[artisan] Queued {} pairs for '{}'",
                    m_totalCount, targetName);
            }
        }
        if (!canSpam) ImGui::EndDisabled();

        if (m_sentCount > 0 && !m_spamming) {
            ImGui::SameLine();
            ImGui::Text("Last run: %d sent", m_sentCount);
        }
    }
}
