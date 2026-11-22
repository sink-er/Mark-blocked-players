// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// ESC/Tab 玩家列表「主页」按钮 —— 旧版（轮询）实现。
// 生产路径在 card_panel.cpp；本文件保留仅供对照/回滚，并对外提供按钮样式与 URL 工具。
#include "profile_legacy.hpp"

#include "blocklist.hpp"
#include "players.hpp"
#include "ui_strings.hpp"

#include <fstream>

namespace mbpid
{

namespace
{

// 旧实现的登记表：把原生 BTN_Gamercard 挪位置/改样式的状态。
std::unordered_map<UObject*, UObject*> g_profile_button_by_entry;
std::unordered_map<UObject*, std::string> g_profile_url_by_button;
std::unordered_set<UObject*> g_profile_known_buttons;
std::unordered_set<UObject*> g_profile_hover_shown;
bool g_profile_hover_hooked = false;
bool g_profile_construct_hooked = false;
std::unordered_map<UObject*, UObject*> g_teammate_buttons;
std::unordered_map<UObject*, uint8> g_teammate_visible;
std::mutex g_profile_mutex;
bool g_profile_click_hooked = false;

auto call_get_owning_player(UObject* widget) -> UObject*
{
    if (!widget) return nullptr;
    static UFunction* fn = find_ufunction(STR("/Script/UMG.UserWidget:GetOwningPlayer"));
    if (!fn) return nullptr;

    const int32 size = fn->GetParmsSize();
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    widget->ProcessEvent(fn, buf.data());

    FProperty* ret = fn->FindProperty(FName(STR("ReturnValue"), FNAME_Find));
    return ret ? *reinterpret_cast<UObject**>(&buf[ret->GetOffset_Internal()]) : nullptr;
}

auto set_render_scale(UObject* widget, float x, float y) -> bool
{
    if (!widget) return false;
    static UFunction* fn = find_ufunction(STR("/Script/UMG.Widget:SetRenderScale"));
    if (!fn) return false;

    const int32 size = fn->GetParmsSize();
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    FProperty* p = fn->FindProperty(FName(STR("Scale"), FNAME_Find));
    if (!p) return false;
    float* value = reinterpret_cast<float*>(p->ContainerPtrToValuePtr<void>(buf.data()));
    if (!value) return false;
    value[0] = x;
    value[1] = y;
    widget->ProcessEvent(fn, buf.data());
    return true;
}

auto call_is_hovered(UObject* widget) -> bool
{
    static UFunction* fn = find_ufunction(STR("/Script/UMG.UserWidget:IsHovered"));
    if (!fn || !widget) return false;

    const int32 size = fn->GetParmsSize();
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    widget->ProcessEvent(fn, buf.data());

    FProperty* ret = fn->FindProperty(FName(STR("ReturnValue"), FNAME_Find));
    if (!ret) return false;
    const bool* value = ret->ContainerPtrToValuePtr<bool>(buf.data());
    return value && *value;
}

auto clear_button_on_clicked(UObject* button) -> void
{
    if (!button) return;
    FProperty* prop = button->GetPropertyByNameInChain(STR("OnClicked"));
    if (!prop) return;
    auto* multicast = static_cast<FMulticastDelegateProperty*>(prop);
    multicast->ClearDelegate(button, prop->ContainerPtrToValuePtr<void>(button));
}

// 把 N 个 float 写进 struct 参数（FVector2D=2）
auto mbp_card_call_vec2(UObject* obj, const TCHAR* fn_path, const TCHAR* param, float x, float y) -> bool
{
    if (!obj) return false;
    UFunction* fn = find_ufunction(fn_path);
    if (!fn) return false;
    const int32 size = fn->GetParmsSize();
    if (size <= 0) return false;
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    FProperty* p = fn->FindProperty(FName(param, FNAME_Find));
    if (!p) return false;
    if (p->GetSize() < static_cast<int32>(2 * sizeof(float))) return false;
    void* dst = p->ContainerPtrToValuePtr<void>(buf.data());
    if (!dst) return false;
    auto* value = reinterpret_cast<float*>(dst); // FVector2D 在 UE4.27 是 float X/Y
    value[0] = x;
    value[1] = y;
    obj->ProcessEvent(fn, buf.data());
    return true;
}

auto set_size_props(UObject* sizer, float width, float height) -> void
{
    if (!sizer) return;
    set_float_prop(sizer, STR("WidthOverride"), width);
    set_float_prop(sizer, STR("HeightOverride"), height);
    set_float_prop(sizer, STR("MinDesiredWidth"), 0.0f);
    set_float_prop(sizer, STR("MinDesiredHeight"), 0.0f);
}

// 老实现里唯一用得上的一处：点「主页」后开浏览器。
auto open_steam_profile(UObject* entry) -> void
{
    if (!entry) return;

    std::string id;
    UObject* player_state = get_object_prop(entry, STR("PlayerState"));
    const std::string ps_id = get_player_state_steam_id(player_state);
    const std::string entry_name = get_entry_player_name(entry);
    const std::string lookup_id = get_steam_id_by_player_name(entry_name);

    if (is_steam_id(ps_id)) id = ps_id;
    else if (is_steam_id(lookup_id)) id = lookup_id;

    if (id.empty())
    {
        log(STR("[MBPId] profile open but no steam id\n"));
        return;
    }

    std::string url = "https://steamcommunity.com/profiles/";
    url += id;
    StringType url_w{ url.begin(), url.end() };
    log(STR("[MBPId] profile open -> {}\n"), url_w);
    launch_url(url);
}

auto on_open_profile_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try { open_steam_profile(ctx.Context); } catch (...) {}
}

auto on_profile_check_hover(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try
    {
        UObject* entry = ctx.Context;
        if (!entry) return;

        if (!call_is_hovered(entry))
        {
            g_profile_hover_shown.erase(entry);
            return;
        }
        if (g_profile_hover_shown.count(entry)) return;

        UObject* button = get_object_prop(entry, STR("BTN_Gamercard"));
        if (!button) return;
        set_widget_visibility(button, 0);
        g_profile_hover_shown.insert(entry);
        log(STR("[MBPId] profile hover show\n"));
    }
    catch (...) {}
}

auto on_profile_hover_begin(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try
    {
        UObject* entry = ctx.Context;
        if (!entry) return;
        UObject* button = get_object_prop(entry, STR("BTN_Gamercard"));
        if (!button) return;
        set_widget_visibility(button, 0);
        log(STR("[MBPId] profile hover show\n"));
    }
    catch (...) {}
}

auto ensure_profile_hover_hook(UObject* entry) -> void
{
    if (g_profile_hover_hooked || !entry) return;
    UClass* cls = entry->GetClassPrivate();
    if (!cls) return;

    StringType path = cls->GetFullName();
    path += STR(":CheckHover");
    UFunction* fn = find_ufunction(path.c_str());
    if (!fn) return;

    auto ids = UObjectGlobals::RegisterHook(fn,
        [](UnrealScriptFunctionCallableContext&, void*) {},
        on_profile_check_hover,
        nullptr);
    g_profile_hover_hooked = true;
    log(STR("[MBPId] profile hover hook ids=({}, {})\n"), ids.first, ids.second);
}

// 旧实现没走到这里：新版不再给按钮挂 OnClicked，改成 hook 里统一处理。
auto on_profile_button_click_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try
    {
        bool known = false;
        std::string url;
        {
            std::lock_guard<std::mutex> lock(g_profile_mutex);
            known = g_profile_known_buttons.count(ctx.Context) > 0;
            auto it = g_profile_url_by_button.find(ctx.Context);
            if (it != g_profile_url_by_button.end()) url = it->second;
        }

        if (known)
        {
            StringType url_w{ url.begin(), url.end() };
            log(STR("[MBPId] profile click raw mapped={} url={}\n"), !url.empty(), url_w);
        }

        if (!url.empty())
        {
            launch_url(url);
        }
    }
    catch (...) {}
}

auto add_profile_button(UObject* entry, UObject* actions, UObject* button) -> void
{
    if (!actions || !button) return;

    // 已经存在就保持原 slot（原 slot 的尺寸/对齐是游戏调好的），不要 remove/add。
    if (is_child_of(actions, button)) return;

    call_add_child(actions, button);
}

auto ensure_profile_click_hook(UObject* entry) -> void
{
    if (g_profile_click_hooked || !entry) return;

    UClass* cls = entry->GetClassPrivate();
    if (!cls) return;

    StringType path = cls->GetFullName();
    path += STR(":OpenProfile");
    UFunction* fn = find_ufunction(path.c_str());
    if (!fn) return;

    auto ids = UObjectGlobals::RegisterHook(fn,
        [](UnrealScriptFunctionCallableContext&, void*) {},
        on_open_profile_post,
        nullptr);
    g_profile_click_hooked = true;
    log(STR("[MBPId] profile open hook ids=({}, {})\n"), ids.first, ids.second);
}

// 旧版最小闭环：只在卡片/资料按钮都可见时，给每个条目的 BTN_Gamercard 打一次样式。
auto update_profile_buttons_simple() -> void
{
    std::vector<UObject*> entries;
    UObjectGlobals::FindAllOf(STR("HUD_TabPlayerListEntry_InteractableV2_C"), entries);
    if (entries.empty()) return;

    for (UObject* entry : entries)
    {
        ensure_profile_click_hook(entry);
        if (g_profile_click_hooked) break;
    }

    std::unordered_set<UObject*> seen_entries;
    std::unordered_set<UObject*> seen_buttons;
    static std::unordered_set<UObject*> text_ready_buttons;
    int valid_buttons = 0;

    for (UObject* entry : entries)
    {
        if (!entry) continue;

        // 复用游戏已有的 BTN_Gamercard（Profile）按钮，尺寸/字体/点击绑定都是现成的
        UObject* button = get_object_prop(entry, STR("BTN_Gamercard"));
        if (!button) continue;

        UClass* button_class = button->GetClassPrivate();
        if (!button_class) continue;

        seen_entries.insert(entry);
        {
            std::lock_guard<std::mutex> lock(g_profile_mutex);
            g_profile_known_buttons.insert(button);
        }

        if (text_ready_buttons.find(button) == text_ready_buttons.end())
        {
            set_profile_button_text(button, button_class);
            copy_profile_button_size(button, entry);
            set_widget_visibility(button, 0);
            text_ready_buttons.insert(button);
        }

        UObject* player_state = get_object_prop(entry, STR("PlayerState"));
        const std::string ps_id = get_player_state_steam_id(player_state);
        const std::string entry_name = get_entry_player_name(entry);
        const std::string lookup_id = get_steam_id_by_player_name(entry_name);
        std::string id;
        if (is_steam_id(ps_id)) id = ps_id;
        else if (is_steam_id(lookup_id)) id = lookup_id;

        {
            std::lock_guard<std::mutex> lock(g_profile_mutex);
            g_profile_button_by_entry[entry] = button;
            if (id.empty()) g_profile_url_by_button.erase(button);
            else g_profile_url_by_button[button] = "https://steamcommunity.com/profiles/" + id;
            seen_buttons.insert(button);
        }
        if (is_steam_id(id)) ++valid_buttons;
    }

    {
        std::lock_guard<std::mutex> lock(g_profile_mutex);
        for (auto it = g_profile_button_by_entry.begin(); it != g_profile_button_by_entry.end();)
        {
            if (!seen_entries.count(it->first)) it = g_profile_button_by_entry.erase(it);
            else ++it;
        }
        for (auto it = g_profile_url_by_button.begin(); it != g_profile_url_by_button.end();)
        {
            if (!seen_buttons.count(it->first)) it = g_profile_url_by_button.erase(it);
            else ++it;
        }
        for (auto it = g_profile_known_buttons.begin(); it != g_profile_known_buttons.end();)
        {
            if (!seen_buttons.count(*it)) it = g_profile_known_buttons.erase(it);
            else ++it;
        }
    }

    static int last_entries = -1;
    static int last_valid = -1;
    if ((int)entries.size() != last_entries || valid_buttons != last_valid)
    {
        last_entries = (int)entries.size();
        last_valid = valid_buttons;
        log(STR("[MBPId] profileEntries={} profileButtons={}\n"), entries.size(), valid_buttons);
    }
}

// 旧版：把原生 BTN_Gamercard 挪到名字/职业下方，作为常驻「主页」按钮。
auto update_profile_buttons_under_name() -> void
{
    std::vector<UObject*> entries;
    UObjectGlobals::FindAllOf(STR("HUD_TabPlayerListEntry_InteractableV2_C"), entries);
    if (entries.empty()) return;

    for (UObject* entry : entries)
    {
        ensure_profile_click_hook(entry);
        if (g_profile_click_hooked) break;
    }

    static std::unordered_set<UObject*> text_ready;
    int teammate_count = 0;
    int ready = 0;

    for (UObject* entry : entries)
    {
        if (!entry || !is_widget_visible(entry)) continue;

        UObject* mute = get_object_prop(entry, STR("BTN_mute"));
        if (!mute) continue;

        const std::string entry_name = get_entry_player_name(entry);
        if (entry_name.empty()) continue;

        // 本地玩家的卡片有可见的“重生”按钮，跳过
        UObject* respawn = get_object_prop(entry, STR("BTN_Respawn_Spacerig"));
        if (respawn && is_widget_visible(respawn)) continue;

        UObject* button = get_object_prop(entry, STR("BTN_Gamercard"));
        if (!button) continue;

        ++teammate_count;

        UObject* target = get_object_prop(entry, STR("VerticalBox_0"));
        if (!target)
        {
            UObject* name_widget = get_object_prop(entry, STR("DATA_playerName"));
            target = name_widget ? call_get_parent(name_widget) : nullptr;
        }
        if (!target) continue;

        // 移到名字/职业下方
        if (!is_child_of(target, button))
        {
            UObject* current_parent = call_get_parent(button);
            if (current_parent) call_remove_child(current_parent, button);
            if (!call_add_child(target, button)) continue;
        }

        if (!text_ready.count(button))
        {
            UClass* cls = button->GetClassPrivate();
            if (!cls) continue;
            set_profile_button_text(button, cls);
            copy_profile_button_size(button, entry);
            set_widget_visibility(button, 0);
            text_ready.insert(button);
        }

        ++ready;
    }

    static int last_teammates = -1;
    static int last_ready = -1;
    if (teammate_count != last_teammates || ready != last_ready)
    {
        last_teammates = teammate_count;
        last_ready = ready;
        log(STR("[MBPId] profileTeammates={} profileReady={}\n"), teammate_count, ready);
    }
}

// 旧版：Gamercard 按钮只处理一次，不做 WidgetTree 遍历 / 每轮 SetVisibility。
auto update_profile_buttons() -> void
{
    std::vector<UObject*> entries;
    UObjectGlobals::FindAllOf(STR("HUD_TabPlayerListEntry_InteractableV2_C"), entries);
    if (entries.empty()) return;

    for (UObject* entry : entries)
    {
        ensure_profile_click_hook(entry);
        if (g_profile_click_hooked) break;
    }

    static std::unordered_set<UObject*> ready_buttons;
    int visible_entries = 0;
    int ready = 0;

    for (UObject* entry : entries)
    {
        if (!entry) continue;
        if (!is_widget_visible(entry)) continue;

        UObject* button = get_object_prop(entry, STR("BTN_Gamercard"));
        if (!button) continue;

        ++visible_entries;
        if (ready_buttons.count(button))
        {
            ++ready;
            continue;
        }

        UClass* cls = button->GetClassPrivate();
        if (!cls) continue;

        set_profile_button_text(button, cls);
        copy_profile_button_size(button, entry);
        set_widget_visibility(button, 0);
        ready_buttons.insert(button);
        ++ready;
    }

    static int last_visible = -1;
    static int last_ready = -1;
    if (visible_entries != last_visible || ready != last_ready)
    {
        last_visible = visible_entries;
        last_ready = ready;
        log(STR("[MBPId] profileVisible={} profileReady={}\n"), visible_entries, ready);
    }
}

// 旧版：按条目 Construct 新建一个同款按钮（不是复用 BTN_Gamercard）。
auto setup_teammate_profile_button(UObject* entry) -> void
{
    if (!entry || g_teammate_buttons.count(entry)) return;
    if (is_under_server_list(entry)) return;

    UObject* mute = get_object_prop(entry, STR("BTN_mute"));
    if (!mute) return;

    UObject* target = get_object_prop(entry, STR("VerticalBox_0"));
    if (!target)
    {
        UObject* name_widget = get_object_prop(entry, STR("DATA_playerName"));
        target = name_widget ? call_get_parent(name_widget) : nullptr;
    }
    if (!target) return;

    UClass* cls = mute->GetClassPrivate();
    if (!cls) return;

    UObject* button = create_profile_button(entry, cls);
    if (!button) return;

    set_profile_button_text(button, cls);
    copy_profile_button_size(button, entry);
    set_widget_visibility(button, 0);

    if (!call_add_child(target, button)) return;

    // 新按钮点击 -> 该条目的 OpenProfile（随后我们的 post hook 打开浏览器）
    FProperty* prop = button->GetPropertyByNameInChain(STR("OnClicked"));
    if (prop)
    {
        auto* multicast = static_cast<FMulticastDelegateProperty*>(prop);
        FScriptDelegate delegate;
        delegate.BindUFunction(entry, FName(STR("OpenProfile"), FNAME_Find));
        multicast->AddDelegate(delegate, button, prop->ContainerPtrToValuePtr<void>(button));
    }

    g_teammate_buttons[entry] = button;
    log(STR("[MBPId] teammate profile button created\n"));
}

auto on_entry_construct(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try
    {
        if (ctx.Context) setup_teammate_profile_button(ctx.Context);
    }
    catch (...) {}
}

// 旧版按 ":Construct" 找 hook —— 资产里卡片没有 Construct，因此这里永远失败（见头注释）。
auto ensure_profile_construct_hook() -> void
{
    if (g_profile_construct_hooked) return;

    std::vector<UObject*> entries;
    UObjectGlobals::FindAllOf(STR("HUD_TabPlayerListEntry_InteractableV2_C"), entries);
    if (entries.empty()) return;

    UClass* cls = entries[0]->GetClassPrivate();
    if (!cls) return;

    StringType path = cls->GetFullName();
    path += STR(":Construct");
    UFunction* fn = find_ufunction(path.c_str());
    if (!fn) return;

    auto ids = UObjectGlobals::RegisterHook(fn,
        [](UnrealScriptFunctionCallableContext&, void*) {},
        on_entry_construct,
        nullptr);
    g_profile_construct_hooked = true;
    log(STR("[MBPId] profile construct hook ids=({}, {})\n"), ids.first, ids.second);

    for (UObject* entry : entries) setup_teammate_profile_button(entry);
}

auto update_teammate_visibility() -> void
{
    for (auto& [entry, button] : g_teammate_buttons)
    {
        UObject* respawn = get_object_prop(entry, STR("BTN_Respawn_Spacerig"));
        const bool is_self = respawn && is_widget_visible(respawn);
        const uint8 desired = is_self ? 1 : 0;
        auto it = g_teammate_visible.find(entry);
        if (it == g_teammate_visible.end() || it->second != desired)
        {
            set_widget_visibility(button, desired);
            g_teammate_visible[entry] = desired;
        }
    }
}

} // namespace

auto mbp_local_player_controller(UObject* world_context) -> UObject*
{
    static UFunction* fn = nullptr;
    static UObject* static_self = nullptr;
    if (!fn) fn = find_ufunction(STR("/Script/Engine.GameplayStatics:GetPlayerController"));
    if (!static_self) static_self = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
    if (!fn || !static_self) return nullptr;

    const int32 size = fn->GetParmsSize();
    if (size <= 0) return nullptr;
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);

    FProperty* world_prop = fn->FindProperty(FName(STR("WorldContextObject"), FNAME_Find));
    FProperty* index_prop = fn->FindProperty(FName(STR("PlayerIndex"), FNAME_Find));
    FProperty* ret_prop   = fn->FindProperty(FName(STR("ReturnValue"), FNAME_Find));
    if (!world_prop || !index_prop || !ret_prop) return nullptr;

    *reinterpret_cast<UObject**>(&buf[world_prop->GetOffset_Internal()]) = world_context;
    *reinterpret_cast<int32*>(&buf[index_prop->GetOffset_Internal()]) = 0;

    static_self->ProcessEvent(fn, buf.data());
    return *reinterpret_cast<UObject**>(&buf[ret_prop->GetOffset_Internal()]);
}

auto create_profile_button(UObject* entry, UClass* button_class) -> UObject*
{
    if (!entry || !button_class) return nullptr;

    static UFunction* fn = nullptr;
    static UObject* static_self = nullptr;
    static bool logged = false;
    if (!fn) fn = find_ufunction(STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
    if (!static_self) static_self = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
    if (!fn || !static_self)
    {
        if (!logged)
        {
            log(STR("[MBPId] profile Create unavailable (fn={} self={})\n"),
                fn != nullptr, static_self != nullptr);
            logged = true;
        }
        return nullptr;
    }

    const int32 size = fn->GetParmsSize();
    if (size <= 0) return nullptr;

    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    FProperty* world_prop = fn->FindProperty(FName(STR("WorldContextObject"), FNAME_Find));
    FProperty* type_prop = fn->FindProperty(FName(STR("WidgetType"), FNAME_Find));
    FProperty* owner_prop = fn->FindProperty(FName(STR("OwningPlayer"), FNAME_Find));
    FProperty* ret_prop = fn->FindProperty(FName(STR("ReturnValue"), FNAME_Find));
    if (!world_prop || !type_prop || !owner_prop || !ret_prop)
    {
        if (!logged)
        {
            log(STR("[MBPId] profile Create params missing\n"));
            logged = true;
        }
        return nullptr;
    }

    *reinterpret_cast<UObject**>(&buf[world_prop->GetOffset_Internal()]) = entry;
    *reinterpret_cast<UClass**>(&buf[type_prop->GetOffset_Internal()]) = button_class;
    UObject* owner_player = call_get_owning_player(entry);
    if (!owner_player) owner_player = mbp_local_player_controller(entry);
    *reinterpret_cast<UObject**>(&buf[owner_prop->GetOffset_Internal()]) = owner_player;

    static_self->ProcessEvent(fn, buf.data());
    return *reinterpret_cast<UObject**>(&buf[ret_prop->GetOffset_Internal()]);
}

auto set_profile_button_text(UObject* button, UClass* button_class) -> void
{
    if (!button || !button_class) return;

    const std::wstring& label = ui_strings().profile_button;
    set_widget_text(button, STR("ButtonText"), label.c_str());

    StringType path = button_class->GetFullName();
    path += STR(":SetButtonText");
    UFunction* fn = find_ufunction(path.c_str());
    if (fn)
    {
        const int32 size = fn->GetParmsSize();
        std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
        FProperty* p = nullptr;
        const TCHAR* names[] = { STR("ButtonText"), STR("InText"), STR("Text") };
        for (auto* n : names)
        {
            p = fn->FindProperty(FName(n, FNAME_Find));
            if (p) break;
        }
        if (p)
        {
            FText text(label.c_str());
            void* dst = p->ContainerPtrToValuePtr<void>(buf.data());
            if (dst)
            {
                p->CopyCompleteValue(dst, &text);
                button->ProcessEvent(fn, buf.data());
                p->DestroyValue(dst);
                return;
            }
        }
    }

    // 兜底：让 Basic_ButtonCutCorner 按 ButtonText 刷新显示
    StringType update_path = button_class->GetFullName();
    update_path += STR(":Update Look");
    UFunction* update_fn = find_ufunction(update_path.c_str());
    if (update_fn)
    {
        std::vector<uint8_t> buf(static_cast<size_t>(update_fn->GetParmsSize()), 0);
        button->ProcessEvent(update_fn, buf.data());
    }
}

auto copy_profile_button_size(UObject* button, UObject* entry) -> void
{
    if (!button) return;

    // Sizer 在 WidgetTree 里，不是 UPROPERTY；只在可见条目上做一次。
    UObject* dst_sizer = find_widget_in_tree(button, STR("Sizer"));
    if (!dst_sizer)
    {
        // 兜底：视觉缩放（布局仍偏大，但至少能看出按钮）
        set_render_scale(button, 0.6f, 0.6f);
        return;
    }

    // 固定成和“静音”接近的小尺寸
    set_size_props(dst_sizer, 48.0f, 24.0f);
    set_float_prop(dst_sizer, STR("MaxDesiredWidth"), 48.0f);
    set_float_prop(dst_sizer, STR("MaxDesiredHeight"), 24.0f);

    UObject* dst_scale = find_widget_in_tree(button, STR("ScaleBox_53"));
    if (dst_scale)
    {
        set_float_prop(dst_scale, STR("UserSpecifiedScale"), 1.0f);
    }

    // slot 改为 Auto，避免被 HorizontalBox 拉满
    UObject* target_slot = get_object_prop(button, STR("Slot"));
    if (target_slot)
    {
        FProperty* size_prop = target_slot->GetPropertyByNameInChain(STR("Size"));
        if (size_prop)
        {
            void* value = size_prop->ContainerPtrToValuePtr<void>(target_slot);
            if (value)
            {
                *reinterpret_cast<float*>(value) = 0.0f;
                *(reinterpret_cast<uint8*>(value) + sizeof(float)) = 0; // ESlateSizeRule::Auto
            }
        }

        UObject* respawn = get_object_prop(entry, STR("BTN_Respawn_Spacerig"));
        UObject* source_slot = respawn ? get_object_prop(respawn, STR("Slot")) : nullptr;
        if (source_slot)
        {
            copy_property_from(target_slot, source_slot, STR("Padding"));
        }
    }

    // 字体也压小
    UClass* cls = button->GetClassPrivate();
    if (!cls) return;
    StringType path = cls->GetFullName();
    path += STR(":SetFontSize");
    UFunction* fn = find_ufunction(path.c_str());
    if (!fn) return;

    const int32 size = fn->GetParmsSize();
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    FProperty* p = fn->FindProperty(FName(STR("FontSize"), FNAME_Find));
    if (!p) p = fn->FindProperty(FName(STR("InFontSize"), FNAME_Find));
    if (!p) return;
    *reinterpret_cast<int32*>(&buf[p->GetOffset_Internal()]) = 11;
    button->ProcessEvent(fn, buf.data());
}

auto launch_url(const std::string& url) -> void
{
    if (url.empty()) return;

    static UFunction* fn = nullptr;
    static UObject* static_self = nullptr;
    static bool logged = false;
    if (!fn) fn = find_ufunction(STR("/Script/Engine.KismetSystemLibrary:LaunchURL"));
    if (!static_self) static_self = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
    if (!fn || !static_self)
    {
        if (!logged)
        {
            log(STR("[MBPId] LaunchURL unavailable (fn={} self={})\n"),
                fn != nullptr, static_self != nullptr);
            logged = true;
        }
        return;
    }

    const int32 size = fn->GetParmsSize();
    if (size <= 0) return;

    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    FProperty* p = fn->FindProperty(FName(STR("URL"), FNAME_Find));
    if (!p) return;

    StringType w{ url.begin(), url.end() };
    FString f(w.c_str());
    void* dst = p->ContainerPtrToValuePtr<void>(buf.data());
    if (!dst) return;
    p->CopyCompleteValue(dst, &f);
    static_self->ProcessEvent(fn, buf.data());
    p->DestroyValue(dst);
}

} // namespace mbpid
