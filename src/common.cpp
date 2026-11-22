// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// MBPId 公共实现：路径推导、字符串转换、UObject 安全读取、UMG 反射调用。
// 业务无关，任何模块都可以调用；这里不持有模块自己的状态。
#include "common.hpp"

namespace mbpid
{

// ---------- 路径 ----------
auto mods_directory() -> const std::filesystem::path&
{
    static const std::filesystem::path dir = [] {
        std::filesystem::path path{UE4SSProgram::get_program().get_mods_directory()};
        if (path.empty())
        {
            wchar_t module_path[32768]{};
            if (GetModuleFileNameW(nullptr, module_path, static_cast<DWORD>(std::size(module_path))))
            {
                path = std::filesystem::path(module_path).parent_path() / L"ue4ss" / L"Mods";
            }
        }
        return path;
    }();
    return dir;
}

auto data_file(const wchar_t* name) -> std::wstring
{
    static const std::filesystem::path dir = mods_directory() / L"MBPId" / L"data";
    return (dir / name).wstring();
}

auto blocklist_fallback_file() -> const std::wstring&
{
    static const std::wstring path = (mods_directory() / L"MBPMarker" / L"data" / L"ignored.txt").wstring();
    return path;
}

auto game_config_directory() -> std::filesystem::path
{
    std::filesystem::path exe_dir{UE4SSProgram::get_program().get_game_executable_directory()};
    if (exe_dir.empty())
    {
        wchar_t module_path[32768]{};
        if (GetModuleFileNameW(nullptr, module_path, 32768))
        {
            exe_dir = std::filesystem::path(module_path).parent_path();
        }
    }
    // Some callers can hand us a path with a trailing separator.
    if (!exe_dir.empty() && exe_dir.filename().empty()) exe_dir = exe_dir.parent_path();
    return exe_dir.parent_path().parent_path() / L"Saved" / L"Config";
}

// ---------- 字符串 ----------
auto wstr_to_utf8(const StringType& w) -> std::string
{
    std::string out;
    for (auto ch : w)
    {
        unsigned int c = static_cast<unsigned int>(static_cast<unsigned short>(ch));
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

auto fstring_to_utf8(const FString& str) -> std::string
{
    return wstr_to_utf8(str.GetCharTArray().Num() ? StringType(str.GetCharTArray().GetData(), str.GetCharTArray().Num()) : StringType{});
}

auto utf8_to_wstr(const std::string& utf8) -> StringType
{
    StringType out{};
    out.reserve(utf8.size());
    size_t i = 0;
    const size_t n = utf8.size();
    while (i < n)
    {
        const unsigned char c0 = static_cast<unsigned char>(utf8[i]);
        unsigned int cp = 0;
        size_t extra = 0;
        if (c0 < 0x80) { cp = c0; extra = 0; }
        else if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; extra = 1; }
        else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; extra = 2; }
        else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; extra = 3; }
        else { ++i; continue; } // 非法首字节：丢弃

        if (i + extra >= n && extra != 0) break;
        bool bad = false;
        for (size_t k = 1; k <= extra; ++k)
        {
            const unsigned char ck = static_cast<unsigned char>(utf8[i + k]);
            if ((ck & 0xC0) != 0x80) { bad = true; break; }
            cp = (cp << 6) | (ck & 0x3F);
        }
        i += extra + 1;
        if (bad) continue;

        if (cp <= 0xFFFF)
        {
            out.push_back(static_cast<TCHAR>(cp));
        }
        else
        {
            // UTF-16 代理对
            cp -= 0x10000;
            out.push_back(static_cast<TCHAR>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<TCHAR>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return out;
}

auto trim_ascii(std::string s) -> std::string
{
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
    };
    size_t begin = 0;
    while (begin < s.size() && is_space(s[begin])) ++begin;
    size_t end = s.size();
    while (end > begin && is_space(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

auto lower_ascii(std::string s) -> std::string
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

auto is_steam_id(const std::string& id) -> bool
{
    if (id.size() != 17) return false;
    for (char c : id)
    {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// ---------- UObject 安全访问 ----------
auto as_live_object(UObject* object) -> UObject*
{
    if (!object) return nullptr;
    FWeakObjectPtr weak(object);
    return weak.Get();
}

auto export_property(UObject* obj, const TCHAR* prop_name) -> std::string
{
    obj = as_live_object(obj);
    if (!obj) return {};
    auto* prop = obj->GetPropertyByNameInChain(prop_name);
    if (!prop) return {};
    FString text{};
    void* value_ptr = prop->ContainerPtrToValuePtr<void>(obj);
    if (!value_ptr) return {};
    prop->ExportTextItem(text, value_ptr, value_ptr, obj, 0);
    return fstring_to_utf8(text);
}

auto get_widget_text(UObject* obj, const TCHAR* prop_name) -> std::string
{
    obj = as_live_object(obj);
    if (!obj) return {};
    auto* prop = obj->GetPropertyByNameInChain(prop_name);
    if (!prop) return {};
    auto* value = prop->ContainerPtrToValuePtr<FText>(obj);
    if (!value) return {};
    return wstr_to_utf8(value->ToString());
}

auto set_widget_text(UObject* obj, const TCHAR* prop_name, const TCHAR* new_text) -> bool
{
    if (!obj) return false;
    auto* prop = obj->GetPropertyByNameInChain(prop_name);
    if (!prop) return false;
    void* dst = prop->ContainerPtrToValuePtr<void>(obj);
    if (!dst) return false;
    FText value(new_text);
    prop->CopyCompleteValue(dst, &value);
    return true;
}

auto get_object_prop(UObject* obj, const TCHAR* prop_name) -> UObject*
{
    obj = as_live_object(obj);
    if (!obj) return nullptr;
    auto* prop = obj->GetPropertyByNameInChain(prop_name);
    if (!prop) return nullptr;
    UObject** p = prop->ContainerPtrToValuePtr<UObject*>(obj);
    return p ? as_live_object(*p) : nullptr;
}

auto find_ufunction(const TCHAR* path) -> UFunction*
{
    return UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
}

auto copy_property_from(UObject* dst, UObject* src, const TCHAR* name) -> bool
{
    if (!dst || !src) return false;
    FProperty* dp = dst->GetPropertyByNameInChain(name);
    FProperty* sp = src->GetPropertyByNameInChain(name);
    if (!dp || !sp) return false;
    dp->CopyCompleteValue(dp->ContainerPtrToValuePtr<void>(dst), sp->ContainerPtrToValuePtr<void>(src));
    return true;
}

auto set_float_prop(UObject* obj, const TCHAR* name, float value) -> bool
{
    if (!obj) return false;
    FProperty* prop = obj->GetPropertyByNameInChain(name);
    if (!prop) return false;
    float* ptr = reinterpret_cast<float*>(prop->ContainerPtrToValuePtr<void>(obj));
    if (!ptr) return false;
    *ptr = value;
    return true;
}

auto get_float_prop(UObject* obj, const TCHAR* name, float& out) -> bool
{
    if (!obj) return false;
    FProperty* prop = obj->GetPropertyByNameInChain(name);
    if (!prop) return false;
    float* ptr = reinterpret_cast<float*>(prop->ContainerPtrToValuePtr<void>(obj));
    if (!ptr) return false;
    out = *ptr;
    return true;
}

auto slot_padding_left(UObject* slot) -> float
{
    if (!slot) return 0.0f;
    FProperty* pad = slot->GetPropertyByNameInChain(STR("Padding"));
    if (!pad) return 0.0f;
    const float* v = reinterpret_cast<const float*>(pad->ContainerPtrToValuePtr<void>(slot));
    return v ? v[0] : 0.0f;
}

// ---------- UMG 面板工具 ----------
auto call_get_parent(UObject* widget) -> UObject*
{
    static UFunction* fn = find_ufunction(STR("/Script/UMG.Widget:GetParent"));
    if (!fn || !widget) return nullptr;

    const int32 size = fn->GetParmsSize();
    std::vector<uint8_t> buf(size, 0);
    widget->ProcessEvent(fn, buf.data());

    FProperty* ret = fn->FindProperty(FName(STR("ReturnValue"), FNAME_Find));
    if (!ret) return nullptr;
    return *reinterpret_cast<UObject**>(&buf[ret->GetOffset_Internal()]);
}

auto call_get_children_count(UObject* panel) -> int32
{
    static UFunction* fn = find_ufunction(STR("/Script/UMG.PanelWidget:GetChildrenCount"));
    if (!fn || !panel) return 0;
    const int32 size = fn->GetParmsSize();
    std::vector<uint8_t> buf(size, 0);
    panel->ProcessEvent(fn, buf.data());
    FProperty* ret = fn->FindProperty(FName(STR("ReturnValue"), FNAME_Find));
    if (!ret) return 0;
    return *reinterpret_cast<int32*>(&buf[ret->GetOffset_Internal()]);
}

auto call_get_child_at(UObject* panel, int32 index) -> UObject*
{
    static UFunction* fn = find_ufunction(STR("/Script/UMG.PanelWidget:GetChildAt"));
    if (!fn || !panel) return nullptr;
    const int32 size = fn->GetParmsSize();
    std::vector<uint8_t> buf(size, 0);
    FProperty* pIndex = fn->FindProperty(FName(STR("Index"), FNAME_Find));
    if (!pIndex) return nullptr;
    *reinterpret_cast<int32*>(&buf[pIndex->GetOffset_Internal()]) = index;
    panel->ProcessEvent(fn, buf.data());
    FProperty* ret = fn->FindProperty(FName(STR("ReturnValue"), FNAME_Find));
    if (!ret) return nullptr;
    return *reinterpret_cast<UObject**>(&buf[ret->GetOffset_Internal()]);
}

auto call_insert_child_at(UObject* panel, int32 index, UObject* child) -> bool
{
    static UFunction* fn = find_ufunction(STR("/Script/UMG.PanelWidget:InsertChildAt"));
    if (!fn || !panel || !child) return false;
    const int32 size = fn->GetParmsSize();
    std::vector<uint8_t> buf(size, 0);
    FProperty* pIndex = fn->FindProperty(FName(STR("Index"), FNAME_Find));
    FProperty* pContent = fn->FindProperty(FName(STR("Content"), FNAME_Find));
    if (!pIndex || !pContent) return false;
    *reinterpret_cast<int32*>(&buf[pIndex->GetOffset_Internal()]) = index;
    *reinterpret_cast<UObject**>(&buf[pContent->GetOffset_Internal()]) = child;
    panel->ProcessEvent(fn, buf.data());
    return true;
}

auto call_add_child(UObject* panel, UObject* child) -> bool
{
    static UFunction* fn = find_ufunction(STR("/Script/UMG.PanelWidget:AddChild"));
    if (!fn || !panel || !child) return false;
    const int32 size = fn->GetParmsSize();
    std::vector<uint8_t> buf(size, 0);
    FProperty* content = fn->FindProperty(FName(STR("Content"), FNAME_Find));
    if (!content) return false;
    *reinterpret_cast<UObject**>(&buf[content->GetOffset_Internal()]) = child;
    panel->ProcessEvent(fn, buf.data());
    return true;
}

auto call_remove_child(UObject* panel, UObject* child) -> bool
{
    static UFunction* fn = find_ufunction(STR("/Script/UMG.PanelWidget:RemoveChild"));
    if (!fn || !panel || !child) return false;
    const int32 size = fn->GetParmsSize();
    std::vector<uint8_t> buf(size, 0);
    FProperty* content = fn->FindProperty(FName(STR("Content"), FNAME_Find));
    if (!content) return false;
    *reinterpret_cast<UObject**>(&buf[content->GetOffset_Internal()]) = child;
    panel->ProcessEvent(fn, buf.data());
    return true;
}

auto is_panel_class(UObject* obj) -> bool
{
    static UClass* panel_class = nullptr;
    if (!panel_class) panel_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.PanelWidget"));
    return obj && panel_class && obj->IsA(panel_class);
}

auto is_child_of(UObject* parent, UObject* child) -> bool
{
    if (!parent || !child) return false;
    const int32 count = call_get_children_count(parent);
    for (int32 i = 0; i < count; ++i)
    {
        if (call_get_child_at(parent, i) == child) return true;
    }
    return false;
}

auto is_widget_visible(UObject* widget) -> bool
{
    if (!widget) return false;
    FProperty* prop = widget->GetPropertyByNameInChain(STR("Visibility"));
    if (!prop) return true;
    const uint8* value = reinterpret_cast<const uint8*>(prop->ContainerPtrToValuePtr<void>(widget));
    if (!value) return true;
    return *value == 0 || *value >= 3; // Visible / HitTestInvisible / SelfHitTestInvisible
}

auto set_widget_visibility(UObject* widget, uint8 visibility) -> bool
{
    if (!widget) return false;

    // 先直接写 Visibility 属性，保证即使 UFunction 不可用也能生效
    FProperty* prop = widget->GetPropertyByNameInChain(STR("Visibility"));
    if (prop)
    {
        uint8* value = reinterpret_cast<uint8*>(prop->ContainerPtrToValuePtr<void>(widget));
        if (value) *value = visibility;
    }

    static UFunction* fn = find_ufunction(STR("/Script/UMG.Widget:SetVisibility"));
    if (!fn) return prop != nullptr;

    const int32 size = fn->GetParmsSize();
    std::vector<uint8_t> buf(size, 0);
    FProperty* p = fn->FindProperty(FName(STR("InVisibility"), FNAME_Find));
    if (!p) return prop != nullptr;
    *reinterpret_cast<uint8*>(&buf[p->GetOffset_Internal()]) = visibility;
    widget->ProcessEvent(fn, buf.data());
    return true;
}

auto find_widget_in_tree(UObject* widget, const TCHAR* target, int depth) -> UObject*
{
    if (!widget || depth > 16) return nullptr;
    if (widget->GetName() == target) return widget;

    UObject* tree = get_object_prop(widget, STR("WidgetTree"));
    if (tree)
    {
        UObject* root = get_object_prop(tree, STR("RootWidget"));
        if (root && root != widget)
        {
            UObject* found = find_widget_in_tree(root, target, depth + 1);
            if (found) return found;
        }
    }

    if (is_panel_class(widget))
    {
        const int32 count = call_get_children_count(widget);
        for (int32 i = 0; i < count; ++i)
        {
            UObject* child = call_get_child_at(widget, i);
            if (!child || child == widget) continue;
            UObject* found = find_widget_in_tree(child, target, depth + 1);
            if (found) return found;
        }
    }
    return nullptr;
}

auto is_under_server_list(UObject* entry) -> bool
{
    if (!entry) return false;
    const StringType path = entry->GetPathName();
    return path.find(STR("ServerList")) != StringType::npos
        || path.find(STR("ITM_ServerList")) != StringType::npos;
}

} // namespace mbpid
