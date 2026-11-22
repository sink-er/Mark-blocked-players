// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// 玩家档案（原有功能）：遍历 PlayerState 导出 UniqueId/名字 → data/players.txt，
// 并维护「玩家名 → SteamID64」映射，供名字反查使用。
//
// 同时提供「名字反查」与「PlayerState → SteamID」两个查询；
// 房内红名（redname）和卡片按钮都用它拿 SteamID64。
//
// 注意：同名不同 ID 时映射里标记 ambiguous，查询返回空，避免误判。
#pragma once

#include "common.hpp"

namespace mbpid
{

// 3 秒一拍：扫描 PlayerState → data/players.txt + 名字映射。没有带 ID 的玩家时不写盘。
auto dump_players() -> void;

// 小写 trim 后反查 SteamID64；找不到或有歧义返回空。
auto get_steam_id_by_player_name(const std::string& name) -> std::string;
// 直接读 PlayerState.UniqueId（返回 trim 过的文本）。
auto get_player_state_steam_id(UObject* player_state) -> std::string;
// 控件自己保存的 PlayerState（SetPlayerState 写入的那个成员）。
// 只处理强引用 ObjectProperty，避免把 TWeakObjectPtr 当裸指针读。
auto get_widget_own_player_state(UObject* widget) -> UObject*;
// 卡片/条目内的玩家名：依次试 DATA_playerName / PlayerName / TXT_PlayerName。
auto get_entry_player_name(UObject* entry) -> std::string;

} // namespace mbpid
