// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// ESC/Tab 玩家列表「主页」按钮 —— 事件驱动实现（现行）。
//
// 目标控件：HUD_TabPlayerListEntry_InteractableV2_C（带“静音”的队友卡片）。
// 在每张卡片上加一个同款 Basic_ButtonCutCorner_C，点击后打开该玩家的资料。
//
// 资产级事实（由 Content/UI/Menu_Tab 的 UAssetGUI JSON 离线确认，不要再猜）：
//   · 卡片 HUD_TabPlayerListEntry_InteractableV2_C 没有 Construct；只有
//     PreConstruct / SetPlayer / OnPlayerSet / CheckHover / Update Mute Button / BndEvt__*。
//     → 按 ":Construct" 找 UFunction 必然失败（旧实现即栽在这里，见 profile_legacy）。
//   · VerticalBox_0 / CanvasPanel_2 / DATA_playerName 都不是卡片类的 UPROPERTY；
//     名字与职业在子控件 HUD_TabPlayerListEntryV2_C 里（卡片属性 Entry）。
//   · 卡片 UPROPERTY：BTN_Gamercard/BTN_Kick/BTN_mute/BTN_unmute/BTN_Respawn_Spacerig/
//     Button_Hover/Entry/HorizontalBox_Actions/MuteSwitcher/PlayerState/PlayerController。
//   · 子控件 UPROPERTY：DATA_playerName/DATA_className/InfoBox/VerticalBox_AmmoHealth/
//     PutStuffHere_Rightside/ITM_*；Ranks 与 CanvasPanel_1 不是属性，只能按名字走 WidgetTree。
//   · ESC 列表 HUD_TabPlayerList_InteractableV2_C 有 Construct 与 AddPlayer，卡片挂在
//     PlayersPanel(HorizontalBox)。
//   · OpenProfile 继承自 native 类 FSDUserWidget：/Script/FSD.FSDUserWidget:OpenProfile。
//   · Basic_ButtonCutCorner_C.Sizer 是 UPROPERTY(SizeBox)：MinDesiredWidth=200、
//     WidthOverride=200、bOverride_MinDesiredWidth=true、bOverride_HeightOverride=true。
//     → 把 MinDesiredWidth 置 0 就自适应宽度；bool 覆盖位不必碰。
//
// 线程模型：hook 里只入队；游戏线程每 200ms 落一次队列；稳态没有 FindAllOf。
// 跨帧保存 UObject 一律用弱引用：ESC 列表会在关菜单/切图/退出时销毁卡片。
//
// 配置：data/profile_button.txt（热改，不用重新编译）
//   placement = bottomleft | infobox | ranks | canvas
//   action    = overlay | browser | both
//   offset_x / offset_y / font / height / width
#pragma once

#include "common.hpp"

namespace mbpid
{

// 载入 profile_button.txt 并写初始化诊断（构造时调一次）。
auto card_panel_init() -> void;
// 游戏线程 200ms 一拍：补装 hook + 落地队列 + 处理定位队列。
auto card_panel_pump() -> void;

} // namespace mbpid
