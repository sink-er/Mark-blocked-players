// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// 玩家档案：PlayerState 导出 + 玩家名反查 SteamID64。
#include "players.hpp"

#include <fstream>

namespace mbpid
{

namespace
{

// 玩家名（小写、trim） -> SteamID64；同名不同 ID 时标记 ambiguous，红名逻辑不会用它。
struct PlayerNameId
{
    std::string id;
    bool ambiguous = false;
};

std::unordered_map<std::string, PlayerNameId> g_player_name_to_id;
std::mutex g_player_name_map_mutex;

} // namespace

auto dump_players() -> void
{
    std::vector<UObject*> states;
    UObjectGlobals::FindAllOf(STR("PlayerState"), states);

    std::string out;
    std::unordered_map<std::string, PlayerNameId> name_map;
    int with_id = 0;
    std::vector<std::string> seen;

    for (UObject* ps : states)
    {
        if (!ps) continue;

        std::string name = trim_ascii(export_property(ps, STR("PlayerNamePrivate")));
        if (name.empty()) name = trim_ascii(export_property(ps, STR("PlayerName")));

        std::string uid = trim_ascii(export_property(ps, STR("UniqueId")));
        if (uid.empty()) continue;

        bool dup = false;
        for (auto& s : seen) { if (s == uid) { dup = true; break; } }
        if (dup) continue;
        seen.push_back(uid);
        ++with_id;

        if (!name.empty())
        {
            const std::string key = lower_ascii(name);
            auto it = name_map.find(key);
            if (it == name_map.end())
            {
                name_map[key] = { uid, false };
            }
            else if (it->second.id != uid)
            {
                it->second.ambiguous = true;
            }
        }
        out += uid + "\t" + name + "\n";
    }

    if (with_id == 0) return;

    {
        std::lock_guard<std::mutex> lock(g_player_name_map_mutex);
        g_player_name_to_id.swap(name_map);
    }

    const std::wstring tmp = data_file(L"players.tmp");
    const std::wstring final_path = data_file(L"players.txt");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f.is_open()) { f.write(out.c_str(), static_cast<std::streamsize>(out.size())); f.close(); }
    }
    _wremove(final_path.c_str());
    _wrename(tmp.c_str(), final_path.c_str());

    log(STR("[MBPId] PlayerStates={} uniqueWithId={}\n"), states.size(), with_id);
}

auto get_steam_id_by_player_name(const std::string& name) -> std::string
{
    const std::string key = lower_ascii(trim_ascii(name));
    if (key.empty()) return {};
    std::lock_guard<std::mutex> lock(g_player_name_map_mutex);
    auto it = g_player_name_to_id.find(key);
    if (it == g_player_name_to_id.end() || it->second.ambiguous) return {};
    return it->second.id;
}

auto get_player_state_steam_id(UObject* player_state) -> std::string
{
    if (!player_state) return {};
    return trim_ascii(export_property(player_state, STR("UniqueId")));
}

auto get_widget_own_player_state(UObject* widget) -> UObject*
{
    widget = as_live_object(widget);
    if (!widget) return nullptr;
    FProperty* prop = widget->GetPropertyByNameInChain(STR("PlayerState"));
    if (!prop) return nullptr;

    const std::string cpp_type = fstring_to_utf8(prop->GetCPPType());
    if (cpp_type.find("Weak") != std::string::npos || cpp_type.find("Soft") != std::string::npos) return nullptr;

    UObject** value = prop->ContainerPtrToValuePtr<UObject*>(widget);
    return value ? as_live_object(*value) : nullptr;
}

auto get_entry_player_name(UObject* entry) -> std::string
{
    if (!entry) return {};
    const TCHAR* names[] = { STR("DATA_playerName"), STR("PlayerName"), STR("TXT_PlayerName") };
    for (auto* name : names)
    {
        UObject* text_block = get_object_prop(entry, name);
        if (!text_block) continue;
        const std::string text = trim_ascii(get_widget_text(text_block, STR("Text")));
        if (!text.empty()) return text;
    }
    return {};
}

} // namespace mbpid
