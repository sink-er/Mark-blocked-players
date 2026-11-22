// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// ESC/Tab 玩家列表「主页」按钮 —— 旧版（轮询）实现。
//
// 历史：这是 v0.1 之前的一版实现，靠 on_update 里定期 FindAllOf 驱动
// （update_profile_buttons_simple / update_profile_buttons_under_name /
//  update_profile_buttons / update_teammate_visibility ...）。
// 现状：**已不再被 on_update 调用**，生产路径是 card_panel.cpp 的事件驱动版本。
// 保留原因：仅供对照与回滚；也复用了其中的一批小工具（按钮样式拷贝、LaunchURL 等）。
//
// 与新版的关键差别：这里按 ":Construct" 找卡片 UFunction —— 而资产里卡片根本没有
// Construct，所以这套 hook 从未装上。真正可用的事实见 card_panel.hpp 的注释。
#pragma once

#include "common.hpp"

namespace mbpid
{

// ---------- 给其它模块复用的小工具 ----------
// 取本地玩家控制器。队友条目的 GetOwningPlayer() 可能返回空，
// 而 WidgetBlueprintLibrary::Create 需要有效的 OwningPlayer 才会真正创建控件。
auto mbp_local_player_controller(UObject* world_context) -> UObject*;
// UMG 蓝图工厂：按类创建一个新控件（OwningPlayer 走 GetOwningPlayer → 本地 PC 兜底）。
auto create_profile_button(UObject* entry, UClass* button_class) -> UObject*;
// 写 Basic_ButtonCutCorner 的按钮文字（先写 ButtonText，再调 SetButtonText / Update Look）。
auto set_profile_button_text(UObject* button, UClass* button_class) -> void;
// 把按钮压成和「静音」接近的小尺寸（Sizer + slot + 字号）。
auto copy_profile_button_size(UObject* button, UObject* entry) -> void;
// 用 KismetSystemLibrary:LaunchURL 打开浏览器。
auto launch_url(const std::string& url) -> void;

} // namespace mbpid
