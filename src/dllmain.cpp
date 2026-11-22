// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// MBPId：Deep Rock Galactic 本地 UI 增强 mod（UE4SS C++）。
//
// 这个文件只做「装配 + 游戏线程调度」，业务逻辑在各自模块里：
//   common          路径 / 字符串 / UObject 安全访问 / UMG 反射小工具
//   blocklist       Steam 屏蔽名单（实时枚举 + ignored.txt 兜底）
//   ui_strings      17 种界面语言文案（读 GameUserSettings.ini）
//   players         PlayerState 导出 players.txt + 玩家名反查 SteamID64
//   names           房内被屏蔽玩家红名（三处名字控件）
//   server_list     服务器列表 [已屏蔽玩家] 标签
//   card_panel      ESC 玩家列表「主页」按钮（事件驱动，现行）
//   profile_legacy  同一按钮的旧轮询实现（不参与调度，提供按钮/URL 小工具）
//
// 线程模型：UE4SS 的 on_update() 跑在它自己的事件循环线程，不是游戏线程。
// 所有 UObject/UMG 操作都只在 EngineTick post 回调（游戏线程）里做。
#include "blocklist.hpp"
#include "card_panel.hpp"
#include "common.hpp"
#include "names.hpp"
#include "players.hpp"
#include "server_list.hpp"
#include "ui_strings.hpp"

using namespace mbpid;

static bool g_game_thread_hook_ready = false;
static bool g_game_thread_hook_warned = false;

static auto run_game_thread_update() -> void
{
    // ProcessEvent 兜底回调可能在 ProcessEvent 内部再次进入；防止递归。
    static thread_local bool in_update = false;
    if (in_update) return;
    in_update = true;
    struct UpdateGuard
    {
        bool& flag;
        ~UpdateGuard() { flag = false; }
    } guard{ in_update };

    using clock = std::chrono::steady_clock;
    static auto last = clock::now();
    static auto last_names = clock::now() - std::chrono::seconds(4);
    static auto last_profiles = clock::now() - std::chrono::seconds(1);
    static auto perf_last = clock::now();
    static double perf_dump = 0.0, perf_scan = 0.0, perf_names = 0.0, perf_profiles = 0.0;
    static int perf_dump_n = 0, perf_scan_n = 0, perf_names_n = 0, perf_profiles_n = 0;
    auto now = clock::now();

    auto elapsed_ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    // 3 秒：玩家档案（players.txt）+ 服务器列表扫描
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= 3000)
    {
        last = now;
        auto t0 = clock::now();
        try { dump_players(); } catch (...) {}
        auto t1 = clock::now();
        perf_dump += elapsed_ms(t0, t1);
        ++perf_dump_n;

        t0 = clock::now();
        try { scan_rows(); } catch (...) {}
        t1 = clock::now();
        perf_scan += elapsed_ms(t0, t1);
        ++perf_scan_n;
    }

    // 2 秒：房内红名
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_names).count() >= 2000)
    {
        last_names = now;
        auto t0 = clock::now();
        try { update_blocked_name_colors(); } catch (...) {}
        auto t1 = clock::now();
        perf_names += elapsed_ms(t0, t1);
        ++perf_names_n;
    }

    // 200ms 只做「补装 hook + 队列落地」；稳态没有 FindAllOf，开销 ≈ 0。
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_profiles).count() >= 200)
    {
        last_profiles = now;
        auto t0 = clock::now();
        try { card_panel_pump(); } catch (...) {}
        auto t1 = clock::now();
        perf_profiles += elapsed_ms(t0, t1);
        ++perf_profiles_n;
    }

    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - perf_last).count() >= 15000)
    {
        perf_last = now;
        auto avg = [](double total, int count) { return count > 0 ? total / count : 0.0; };
        log(STR("[MBPId] perf avg dump={:.3f}ms scanRows={:.3f}ms names={:.3f}ms profiles={:.3f}ms\n"),
            avg(perf_dump, perf_dump_n), avg(perf_scan, perf_scan_n),
            avg(perf_names, perf_names_n), avg(perf_profiles, perf_profiles_n));
        perf_dump = perf_scan = perf_names = perf_profiles = 0.0;
        perf_dump_n = perf_scan_n = perf_names_n = perf_profiles_n = 0;
    }
}

static auto try_register_game_thread_hook() -> void
{
    if (g_game_thread_hook_ready) return;

    try
    {
        if (UE4SSRuntime::IsEngineTickAvailable())
        {
            Hook::RegisterEngineTickPostCallback([](UEngine*, float) { run_game_thread_update(); });
            g_game_thread_hook_ready = true;
            log(STR("[MBPId] game-thread hook: EngineTick\n"));
            return;
        }
    }
    catch (...) {}

    try
    {
        Hook::RegisterGameViewportClientTickPostCallback([](UGameViewportClient*, float) { run_game_thread_update(); });
        g_game_thread_hook_ready = true;
        log(STR("[MBPId] game-thread hook: GameViewportClientTick\n"));
        return;
    }
    catch (...) {}

    try
    {
        if (UE4SSRuntime::IsProcessEventAvailable())
        {
            Hook::RegisterProcessEventPostCallback([](UObject*, UFunction*, void*) { run_game_thread_update(); });
            g_game_thread_hook_ready = true;
            log(STR("[MBPId] game-thread hook: ProcessEvent\n"));
            return;
        }
    }
    catch (...) {}

    if (!g_game_thread_hook_warned)
    {
        g_game_thread_hook_warned = true;
        log(STR("[MBPId] no game-thread hook available; will retry\n"));
    }
}

class MBPIdMod : public CppUserModBase
{
public:
    MBPIdMod() : CppUserModBase()
    {
        ModName        = STR("MBPId");
        ModVersion     = STR("0.1");
        ModDescription = STR("Player SteamIDs + server list tag + blocked name color + profile button (live Steam blocklist)");
        ModAuthors     = STR("local");
        try { card_panel_init(); } catch (...) {}
        log(STR("[MBPId] constructed v0.1 (game-thread updates)\n"));
        log(STR("[MBPId] UI language: {} / {}\n"),
            ui_strings().blocked_tag, ui_strings().profile_button);
    }

    ~MBPIdMod() override = default;

    // 关键：UE4SS 的 on_update 在它自己的事件循环线程，不是游戏线程。
    // 旧实现从那里直接 FindAllOf / ProcessEvent，会和游戏线程销毁控件竞争，
    // 触发 pure virtual 或访问冲突。所有 UObject 工作改到 EngineTick（游戏线程）。
    auto on_unreal_init() -> void override
    {
        try_register_game_thread_hook();
    }

    // 只做「补装游戏线程 hook」；不碰任何 UObject，避免回到旧线程问题。
    auto on_update() -> void override
    {
        if (g_game_thread_hook_ready) return;

        using clock = std::chrono::steady_clock;
        static auto last_try = clock::now() - std::chrono::seconds(5);
        const auto now = clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_try).count() < 1000) return;
        last_try = now;
        try_register_game_thread_hook();
    }
};

#define MBPID_API __declspec(dllexport)
extern "C"
{
    MBPID_API RC::CppUserModBase* start_mod() { return new MBPIdMod(); }
    MBPID_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
