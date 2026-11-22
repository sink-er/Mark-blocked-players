// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// 房内被屏蔽玩家红名。
//
// 每 2 秒扫描三处名字控件，命中屏蔽名单就把名字改成红色，未命中则恢复原色：
//   · HUD_TeamDisplay_SmallItem.PlayerName      游戏内队友列表
//   · HUD_PlayerName_Label.DATA_PlayerName      本地玩家信息
//   · ITEM_SpaceRig_TeamMember.TXT_PlayerName   空间站与选人队友列表
// 诊断写 data/names.txt。
//
// SteamID 来源优先级：控件自己的 PlayerState 属性 → 名字反查（同名不同 ID 不参与）。
// 存在 data/test_all_names.txt 时把所有找到的名字都涂红（只用于检查样式）。
#pragma once

#include "common.hpp"

namespace mbpid
{

// 2 秒一拍：刷新名单 + 重涂三处名字控件 + 写 data/names.txt。
auto update_blocked_name_colors() -> void;

} // namespace mbpid
