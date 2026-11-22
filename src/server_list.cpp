// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// 服务器列表标记：房主 SteamID 解析 + 只在命中时插入自有红色标签。
#include "server_list.hpp"

#include "blocklist.hpp"
#include "ui_strings.hpp"

#include <fstream>

namespace mbpid
{

namespace
{

bool g_hooked = false;

// ---------- 房主 SteamID ----------
// 行控件的 Session 是 FBlueprintSessionResult；反射调用
// USessionHandling::FSDGetHostUserID(Session) -> FString，避免自己解析 UniqueNetId。
auto get_host_id(UObject* row) -> std::string
{
    row = as_live_object(row);
    if (!row) return {};

    static UFunction* fn = nullptr;
    static UObject* static_self = nullptr;
    static bool logged = false;

    // 类/函数可能比本 mod 的首轮扫描晚一点就绪，没找到就下一轮再试。
    if (!fn) fn = find_ufunction(STR("/Script/FSD.SessionHandling:FSDGetHostUserID"));
    if (!static_self) static_self = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/FSD.Default__SessionHandling"));

    if (!fn || !static_self)
    {
        if (!logged)
        {
            log(STR("[MBPId] FSDGetHostUserID unavailable (fn={} self={})\n"),
                fn != nullptr, static_self != nullptr);
            logged = true;
        }
        return {};
    }

    FProperty* result_prop = fn->FindProperty(FName(STR("Result"), FNAME_Find));
    FProperty* ret_prop = fn->FindProperty(FName(STR("ReturnValue"), FNAME_Find));
    FProperty* session_prop = row->GetPropertyByNameInChain(STR("Session"));
    if (!result_prop || !ret_prop || !session_prop)
    {
        if (!logged)
        {
            log(STR("[MBPId] FSDGetHostUserID params missing (Result={} Return={} Session={})\n"),
                result_prop != nullptr, ret_prop != nullptr, session_prop != nullptr);
            logged = true;
        }
        return {};
    }

    const int32 size = fn->GetParmsSize();
    const int32 struct_size = result_prop->GetSize();
    if (size <= 0 || struct_size <= 0 || size < struct_size) return {};
    if (session_prop->GetSize() != struct_size) return {};

    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    void* dst = result_prop->ContainerPtrToValuePtr<void>(buf.data());
    void* src = session_prop->ContainerPtrToValuePtr<void>(row);
    if (!dst || !src) return {};

    // 注意：FBlueprintSessionResult::OnlineResult 在 UE4.27 头里没有 UPROPERTY 标记，
    // 反射式的 CopyCompleteValue 可能只走反射、把真正的 OnlineResult 丢掉；这里按字节
    // 复制整块 C++ 结构，只读使用。调用后不要 DestroyValue(dst)，否则会重复释放
    // 源行里 TSharedPtr/FString 的内部对象。
    std::memcpy(dst, src, static_cast<size_t>(struct_size));
    static_self->ProcessEvent(fn, buf.data());

    void* ret_ptr = ret_prop->ContainerPtrToValuePtr<void>(buf.data());
    std::string host_id = ret_ptr ? trim_ascii(fstring_to_utf8(*static_cast<FString*>(ret_ptr))) : std::string{};

    if (ret_ptr) ret_prop->DestroyValue(ret_ptr);

    return host_id;
}

// ---------- 自有标签（MBP_Tag）管理 ----------
// 父容器里是否已经有我们的标签（名字可能被自动加后缀，用前缀判断）
auto has_our_tag(UObject* parent) -> bool
{
    const int32 count = call_get_children_count(parent);
    for (int32 i = 0; i < count; ++i)
    {
        UObject* child = call_get_child_at(parent, i);
        if (!child) continue;
        StringType n = child->GetName();
        if (n.find(STR("MBP_Tag")) == 0) return true;
    }
    return false;
}

auto count_our_tags(UObject* parent) -> int32
{
    int32 n = 0;
    const int32 count = call_get_children_count(parent);
    for (int32 i = 0; i < count; ++i)
    {
        UObject* child = call_get_child_at(parent, i);
        if (!child) continue;
        if (child->GetName().find(STR("MBP_Tag")) == 0) ++n;
    }
    return n;
}

// 移除容器里所有我们创建的标签（阶段清理，避免刷新后堆积）
auto remove_all_our_tags(UObject* parent) -> int32
{
    int32 removed = 0;
    for (int32 i = call_get_children_count(parent) - 1; i >= 0; --i)
    {
        UObject* child = call_get_child_at(parent, i);
        if (!child) continue;
        StringType n = child->GetName();
        if (n.find(STR("MBP_Tag")) == 0)
        {
            if (call_remove_child(parent, child)) ++removed;
        }
    }
    return removed;
}

// 把控件颜色改成红色（FSlateColor 内部第一个成员是 FLinearColor）
auto force_red(UObject* widget) -> bool
{
    if (!widget) return false;
    FProperty* prop = widget->GetPropertyByNameInChain(STR("ColorAndOpacity"));
    if (!prop) return false;
    float* color = reinterpret_cast<float*>(prop->ContainerPtrToValuePtr<void>(widget));
    if (!color) return false;
    color[0] = 1.0f;  // R
    color[1] = 0.15f; // G
    color[2] = 0.15f; // B
    color[3] = 1.0f;  // A
    return true;
}

// ---------- 行控件打标（只插入自有控件，不改游戏文本） ----------
// 只有房主 SteamID 命中屏蔽名单时才加标签；未命中或会话已换则移除旧标签。
auto tag_row(UObject* row, const std::string* known_host_id = nullptr) -> bool
{
    row = as_live_object(row);
    if (!row) return false;

    UObject* region_widget = get_object_prop(row, STR("TXT_Region"));
    if (!region_widget) return false;

    // 旧版调试钩子曾把 " [TEST]" 直接拼进 TXT_Region 的 Text；若还有残留，先去掉，
    // 避免黄色 [TEST] 在旧控件上复现。只处理结尾，绝不改动地区标识本身。
    {
        std::string region_text = get_widget_text(region_widget, STR("Text"));
        const std::string marker = " [TEST]";
        if (region_text.size() >= marker.size() &&
            region_text.compare(region_text.size() - marker.size(), marker.size(), marker) == 0)
        {
            std::string cleaned = region_text.substr(0, region_text.size() - marker.size());
            StringType w{cleaned.begin(), cleaned.end()};
            set_widget_text(region_widget, STR("Text"), w.c_str());
            log(STR("[MBPId] cleaned legacy [TEST] from TXT_Region\n"));
        }
    }

    UObject* parent = call_get_parent(region_widget);
    if (!parent) return false;

    refresh_blocklist_if_needed();
    const std::string host_id = known_host_id ? *known_host_id : get_host_id(row);
    if (!is_blocked(host_id))
    {
        remove_all_our_tags(parent);
        return false;
    }

    const int32 tags_before = count_our_tags(parent);
    remove_all_our_tags(parent);

    UClass* tb_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.TextBlock"));
    if (!tb_class) return false;

    UObject* tag = UObjectGlobals::NewObject<UObject>(parent, tb_class, FName(STR("MBP_Tag"), FNAME_Add));
    if (!tag) return false;

    set_widget_text(tag, STR("Text"), ui_strings().blocked_tag.c_str());
    copy_property_from(tag, region_widget, STR("Font"));
    copy_property_from(tag, region_widget, STR("ColorAndOpacity"));
    force_red(tag);

    if (!call_add_child(parent, tag)) return false;
    if (tags_before == 0)
    {
        StringType host_w{host_id.begin(), host_id.end()};
        log(STR("[MBPId] HIT host={} -> tagged row\n"), host_w);
    }

    // 插到 region 之后：把它后面的子控件挪走再挪回（InsertChildAt 不是蓝图函数）
    int32 region_index = -1;
    const int32 count = call_get_children_count(parent);
    for (int32 i = 0; i < count; ++i)
    {
        if (call_get_child_at(parent, i) == region_widget) { region_index = i; break; }
    }
    if (region_index < 0 || count > 10) return true;   // 条件不满足就接受末尾位置

    std::vector<UObject*> tail;
    for (int32 i = count - 1; i > region_index; --i)
    {
        UObject* child = call_get_child_at(parent, i);
        if (!child || child == tag) continue;
        tail.push_back(child);
    }
    for (auto* c : tail) call_remove_child(parent, c);
    for (auto it = tail.rbegin(); it != tail.rend(); ++it) call_add_child(parent, *it);

    return true;
}

auto on_set_session_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try { tag_row(ctx.Context); } catch (...) {}
}

// 在第一次看到行控件时挂 hook（post 阶段：行内文字已经填好）
auto ensure_hook() -> void
{
    if (g_hooked) return;

    std::vector<UObject*> rows;
    UObjectGlobals::FindAllOf(STR("ITM_ServerList_Entry_C"), rows);
    if (rows.empty()) return;

    UClass* cls = rows[0]->GetClassPrivate();
    if (!cls)
    {
        log(STR("[MBPId] row class is null\n"));
        return;
    }

    // UE4SS 没有 UClass::FindFunctionByName，用「完整对象路径」注册
    StringType func_path = cls->GetFullName();
    func_path += STR(":SetSession");
    log(STR("[MBPId] trying hook path: {}\n"), func_path);

    auto ids = UObjectGlobals::RegisterHook(func_path,
        [](UnrealScriptFunctionCallableContext&, void*) {},
        on_set_session_post,
        nullptr);
    g_hooked = true;
    log(STR("[MBPId] RegisterHook ids=({}, {})\n"), ids.first, ids.second);
}

} // namespace

auto scan_rows() -> void
{
    ensure_hook();
    refresh_blocklist_if_needed();

    std::vector<UObject*> rows;
    UObjectGlobals::FindAllOf(STR("ITM_ServerList_Entry_C"), rows);
    if (rows.empty()) return;

    std::string log_text;
    int tagged = 0;
    int host_ids = 0;

    for (UObject* row : rows)
    {
        if (!row) continue;

        UObject* name_widget   = get_object_prop(row, STR("TXT_ServerName"));
        UObject* region_widget = get_object_prop(row, STR("TXT_Region"));

        std::string name   = get_widget_text(name_widget, STR("Text"));
        std::string region = get_widget_text(region_widget, STR("Text"));
        std::string session = export_property(row, STR("Session"));

        std::string host_id;
        bool blocked = false;
        if (region_widget)
        {
            host_id = get_host_id(row);
            blocked = is_blocked(host_id);
            if (!host_id.empty()) ++host_ids;
        }

        log_text += name;
        log_text += "\t";
        log_text += region;
        log_text += "\t";
        log_text += host_id.empty() ? "-" : host_id;
        log_text += "\t";
        log_text += blocked ? "BLOCKED" : "-";
        log_text += "\t";

        // 诊断：父容器信息（每行都记，便于定位）
        {
            UObject* parent = region_widget ? call_get_parent(region_widget) : nullptr;
            std::string pcls = "null";
            int32 count = 0;
            int32 found = -1;
            if (parent)
            {
                if (parent->GetClassPrivate()) pcls = wstr_to_utf8(parent->GetClassPrivate()->GetFullName());
                count = call_get_children_count(parent);
                for (int32 i = 0; i < count; ++i)
                {
                    if (call_get_child_at(parent, i) == region_widget) { found = i; break; }
                }
            }
            log_text += pcls + " count=" + std::to_string(count) + " idx=" + std::to_string(found);
        }

        log_text += "\t";
        if (region_widget && region_widget->GetClassPrivate())
        {
            log_text += wstr_to_utf8(region_widget->GetClassPrivate()->GetFullName());
        }
        log_text += "\t";
        log_text += session;
        log_text += "\n";

        // hook 还没触发时（例如列表已经在屏幕上）也在这里兜底
        if (tag_row(row, &host_id)) ++tagged;
    }

    std::ofstream out(data_file(L"rows.txt"), std::ios::binary | std::ios::trunc);
    if (out.is_open())
    {
        out.write(log_text.c_str(), static_cast<std::streamsize>(log_text.size()));
        out.close();
    }

    log(STR("[MBPId] rows={} hostIds={} blockedRows={} hooked={} blockedTotal={}\n"),
        rows.size(), host_ids, tagged, g_hooked, blocked_count());
}

} // namespace mbpid
