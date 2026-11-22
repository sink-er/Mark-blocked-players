// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// Steam 屏蔽名单：实时枚举 + ignored.txt 兜底。
#include "blocklist.hpp"

#include <fstream>

namespace mbpid
{

namespace
{

std::unordered_set<std::string> g_blocked_ids;
std::mutex g_blocklist_mutex;
std::mutex g_blocklist_refresh_mutex;
bool g_blocklist_logged = false;

// steam_api64.dll 导出的 v017 friends / v021 user 接口。
using SteamFriendsFn        = void* (__cdecl*)();
using SteamUserFn           = void* (__cdecl*)();
using SteamIsRunningFn      = bool  (__cdecl*)();
using SteamGetSteamIDFn     = unsigned long long (__cdecl*)(void*);
using SteamFriendCountFn    = int   (__cdecl*)(void*, int);
using SteamFriendByIndexFn  = unsigned long long (__cdecl*)(void*, int, int);

constexpr int kFriendFlagBlocked = 0x1;
constexpr int kFriendFlagIgnored = 0x200;

// 返回 true 表示「成功读到实时列表」（哪怕是空的）；false 表示接口还没就绪。
auto load_live_steam_blocklist(std::unordered_set<std::string>& ids) -> bool
{
    static HMODULE steam_module = nullptr;
    static SteamFriendsFn get_friends = nullptr;
    static SteamUserFn get_user = nullptr;
    static SteamIsRunningFn is_running = nullptr;
    static SteamGetSteamIDFn get_steam_id = nullptr;
    static SteamFriendCountFn get_count = nullptr;
    static SteamFriendByIndexFn get_by_index = nullptr;
    static bool resolved = false;
    static auto last_resolve = std::chrono::steady_clock::now() - std::chrono::seconds(5);

    const auto now = std::chrono::steady_clock::now();
    if (!resolved)
    {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_resolve).count() < 5) return false;
        last_resolve = now;

        steam_module = GetModuleHandleW(L"steam_api64.dll");
        if (!steam_module) return false;

        get_friends  = reinterpret_cast<SteamFriendsFn>(GetProcAddress(steam_module, "SteamAPI_SteamFriends_v017"));
        get_user     = reinterpret_cast<SteamUserFn>(GetProcAddress(steam_module, "SteamAPI_SteamUser_v021"));
        is_running   = reinterpret_cast<SteamIsRunningFn>(GetProcAddress(steam_module, "SteamAPI_IsSteamRunning"));
        get_steam_id = reinterpret_cast<SteamGetSteamIDFn>(GetProcAddress(steam_module, "SteamAPI_ISteamUser_GetSteamID"));
        get_count    = reinterpret_cast<SteamFriendCountFn>(GetProcAddress(steam_module, "SteamAPI_ISteamFriends_GetFriendCount"));
        get_by_index = reinterpret_cast<SteamFriendByIndexFn>(GetProcAddress(steam_module, "SteamAPI_ISteamFriends_GetFriendByIndex"));
        resolved = get_friends && get_count && get_by_index;
        if (!resolved) return false;
    }

    if (is_running && !is_running()) return false;

    // 确认 SteamAPI 已经初始化（本机 SteamID 非 0），避免在引擎初始化前调用接口。
    if (get_user && get_steam_id)
    {
        void* user = get_user();
        if (!user || get_steam_id(user) == 0) return false;
    }

    void* friends = get_friends();
    if (!friends) return false;

    bool any = false;
    for (const int flags : { kFriendFlagIgnored, kFriendFlagBlocked })
    {
        const int count = get_count(friends, flags);
        if (count < 0) continue;
        any = true;
        for (int i = 0; i < count; ++i)
        {
            const unsigned long long id = get_by_index(friends, i, flags);
            if (id != 0) ids.insert(std::to_string(id));
        }
    }
    return any;
}

// ignored.txt：每行 `<steamid64>[\t名字]`；# 开头是注释。
auto load_blocklist_file(std::unordered_set<std::string>& ids) -> bool
{
    const std::wstring fallback_path = blocklist_fallback_file();
    std::ifstream in(fallback_path, std::ios::binary);
    if (!in.is_open())
    {
        std::lock_guard<std::mutex> lock(g_blocklist_mutex);
        if (!g_blocklist_logged)
        {
            log(STR("[MBPId] blocklist not found: {}\n"), fallback_path.c_str());
            g_blocklist_logged = true;
        }
        return false;
    }

    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#') continue;

        const size_t tab = line.find('\t');
        const std::string id = trim_ascii(tab == std::string::npos ? line : line.substr(0, tab));
        if (id.size() < 10) continue;

        bool all_digits = true;
        for (char c : id)
        {
            if (c < '0' || c > '9') { all_digits = false; break; }
        }
        if (all_digits) ids.insert(id);
    }
    return true;
}

auto load_blocklist() -> void
{
    static bool live_seen = false;

    std::unordered_set<std::string> ids;
    const bool live = load_live_steam_blocklist(ids);
    if (live) live_seen = true;

    // 已经成功读过 Steam 列表后，偶尔一次接口失败（Steam 正忙/退出中）不要回退到旧文件，
    // 否则刚屏蔽的两个 ID 会短暂消失；保留上一次实时结果即可。
    if (!live && live_seen) return;

    if (!live)
    {
        if (!load_blocklist_file(ids))
        {
            std::lock_guard<std::mutex> lock(g_blocklist_mutex);
            g_blocked_ids.clear();
            return;
        }
    }

    std::lock_guard<std::mutex> lock(g_blocklist_mutex);
    const bool changed = ids != g_blocked_ids;
    g_blocked_ids.swap(ids);
    if (changed || !g_blocklist_logged)
    {
        log(STR("[MBPId] blocklist loaded: {} ids (source={})\n"),
            g_blocked_ids.size(), live ? STR("steam") : STR("file"));
        g_blocklist_logged = true;
    }
}

} // namespace

auto refresh_blocklist_if_needed() -> void
{
    std::lock_guard<std::mutex> refresh_lock(g_blocklist_refresh_mutex);
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(3);
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count() < 2) return;
    last = now;
    load_blocklist();
}

auto is_blocked(const std::string& steam_id) -> bool
{
    if (steam_id.empty()) return false;
    std::lock_guard<std::mutex> lock(g_blocklist_mutex);
    return g_blocked_ids.find(steam_id) != g_blocked_ids.end();
}

auto blocked_count() -> std::size_t
{
    std::lock_guard<std::mutex> lock(g_blocklist_mutex);
    return g_blocked_ids.size();
}

} // namespace mbpid
