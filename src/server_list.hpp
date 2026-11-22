// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// 服务器列表「已屏蔽房主」标记。
//
// 做法：遍历行控件 ITM_ServerList_Entry_C
//   · 从 Session 反射调用 USessionHandling::FSDGetHostUserID 取房主 SteamID64；
//   · 命中屏蔽名单时，在 TXT_Region 之后插入一个独立的红色 TextBlock（MBP_Tag）；
//   · 绝不改写游戏自己的 TXT_Region 文本；
//   · 未命中或行被复用给了别的 Session，就移除旧标签。
//
// 历史坑（别重复踩）：
//   · 行控件是缓存池复用的（AddOrUpdateEntry / CacheUnusedEntries），一个控件一生
//     绑定多个 Session，所以标签必须跟「当前 Session」对账；
//   · FindAllOf 会捞到大量从未绑定房间的空壳，往它们写东西界面上不会有反应；
//   · UWidget::GetParent() 对动态行控件可能返回 null。
#pragma once

#include "common.hpp"

namespace mbpid
{

// 扫描全部行控件：写 data/rows.txt 诊断 + 对账标签；首次还负责挂 SetSession hook。
auto scan_rows() -> void;

} // namespace mbpid
