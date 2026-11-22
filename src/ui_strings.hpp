// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// 游戏语言 / 界面文案。
//
// 不依赖 UE4 的 FInternationalization 私有布局，直接读游戏自己的
// Saved/Config/WindowsNoEditor/GameUserSettings.ini 里的 [Internationalization]。
// 语言切换后重启游戏即可生效。文案表见 docs/TRANSLATIONS.md（17 种语言，两边一起改）。
#pragma once

#include "common.hpp"

namespace mbpid
{

struct UiStrings
{
    // 服务器列表行内标签，例如 [已屏蔽玩家]
    std::wstring blocked_tag;
    // ESC 玩家卡片的「主页」按钮
    std::wstring profile_button;
};

// 规范化后的语言码，例如 en / zh-hans / pt-br
auto ui_language() -> const std::string&;
// 当前语言的文案（进程内只解析一次）
auto ui_strings() -> const UiStrings&;

} // namespace mbpid
