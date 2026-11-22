// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// MBPId 公共基础：路径、字符串、日志、UObject 安全访问、UMG 反射小工具。
// 这里只放「两个以上模块都要用」的东西；业务逻辑在各自模块里。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <array>
#include <cstring>
#include <cstdlib>
#include <cstdint>

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UEngine.hpp>
#include <Unreal/UGameViewportClient.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <UE4SSRuntime.hpp>
#include <UE4SSProgram.hpp>

namespace mbpid
{
using namespace RC;
using namespace RC::Unreal;

// ---------- 日志 ----------
template <typename... Args>
inline auto log(const TCHAR* fmt, Args&&... args) -> void
{
    Output::send<LogLevel::Verbose>(fmt, std::forward<Args>(args)...);
}

// ---------- 路径 ----------
// 所有路径都从 UE4SS 的 Mods 目录推导，仓库里不写死任何本机路径。
auto mods_directory() -> const std::filesystem::path&;
// data_file(L"rows.txt") -> <mods>/MBPId/data/rows.txt
auto data_file(const wchar_t* name) -> std::wstring;
// SteamAPI 未就绪时的兜底名单：<mods>/MBPMarker/data/ignored.txt
auto blocklist_fallback_file() -> const std::wstring&;
// <Game>/FSD/Saved/Config（不含 WindowsNoEditor 这一层）
auto game_config_directory() -> std::filesystem::path;

// ---------- 字符串 ----------
auto wstr_to_utf8(const StringType& w) -> std::string;
auto fstring_to_utf8(const FString& str) -> std::string;
// UTF-8 -> UE 宽字符串。必须走真正的转换，绝不能 StringType{utf8.begin(), utf8.end()}
// —— 那样会把每个 UTF-8 字节单独当成一个 wchar，中文直接变乱码。
auto utf8_to_wstr(const std::string& utf8) -> StringType;
auto trim_ascii(std::string s) -> std::string;
auto lower_ascii(std::string s) -> std::string;
// 17 位纯数字（SteamID64）
auto is_steam_id(const std::string& id) -> bool;

// ---------- UObject 安全访问 ----------
// FindAllOf 会返回 pending kill 的对象；读属性/调虚函数会崩，先过一遍弱引用。
auto as_live_object(UObject* object) -> UObject*;
auto export_property(UObject* obj, const TCHAR* prop_name) -> std::string;
auto get_widget_text(UObject* obj, const TCHAR* prop_name) -> std::string;
auto set_widget_text(UObject* obj, const TCHAR* prop_name, const TCHAR* new_text) -> bool;
auto get_object_prop(UObject* obj, const TCHAR* prop_name) -> UObject*;
auto find_ufunction(const TCHAR* path) -> UFunction*;
auto copy_property_from(UObject* dst, UObject* src, const TCHAR* name) -> bool;
auto set_float_prop(UObject* obj, const TCHAR* name, float value) -> bool;
auto get_float_prop(UObject* obj, const TCHAR* name, float& out) -> bool;
// UPanelSlot::Padding（FMargin: Left/Top/Right/Bottom）的 Left
auto slot_padding_left(UObject* slot) -> float;

// ---------- UMG 面板工具 ----------
auto call_get_parent(UObject* widget) -> UObject*;
auto call_get_children_count(UObject* panel) -> int32;
auto call_get_child_at(UObject* panel, int32 index) -> UObject*;
auto call_insert_child_at(UObject* panel, int32 index, UObject* child) -> bool;
auto call_add_child(UObject* panel, UObject* child) -> bool;
auto call_remove_child(UObject* panel, UObject* child) -> bool;
auto is_panel_class(UObject* obj) -> bool;
auto is_child_of(UObject* parent, UObject* child) -> bool;
// 只读 Visibility 属性；识别不了 ScrollBox 的滚动裁剪（见 README「踩坑」）
auto is_widget_visible(UObject* widget) -> bool;
auto set_widget_visibility(UObject* widget, uint8 visibility) -> bool;
auto find_widget_in_tree(UObject* widget, const TCHAR* target, int depth = 0) -> UObject*;
// 该控件是否挂在服务器列表行下面（卡片逻辑要跳过这些，交给 server_list 处理）
auto is_under_server_list(UObject* entry) -> bool;

} // namespace mbpid
