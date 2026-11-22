// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// Steam 屏蔽名单（SteamID64 集合）。
//
// 优先实时读 Steamworks：游戏进程已经初始化 SteamAPI，steam_api64.dll 也导出
// v017 的 friends 接口，直接枚举 k_EFriendFlagIgnored(0x200) / k_EFriendFlagBlocked(0x1)。
// SteamAPI 未就绪时回退 <mods>/MBPMarker/data/ignored.txt（旧导出脚本的产物）。
//
// 刷新策略：每 2 秒最多一次；一旦成功读过实时列表，之后偶发的接口失败
// （Steam 正忙/退出中）保留上一次结果，不回退到旧文件——否则刚屏蔽的 ID 会短暂消失。
#pragma once

#include "common.hpp"

namespace mbpid
{

// 按需刷新（上限 2 秒一次）；扫描前调用即可。
auto refresh_blocklist_if_needed() -> void;
// 查询单个 ID 是否在名单里。空字符串恒为 false。
auto is_blocked(const std::string& steam_id) -> bool;
auto blocked_count() -> std::size_t;

} // namespace mbpid
