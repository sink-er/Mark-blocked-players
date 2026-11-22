// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// ESC 玩家列表「主页」按钮：事件驱动实现（hook 入队 → 游戏线程落地）。
#include "card_panel.hpp"

#include "players.hpp"
#include "profile_legacy.hpp"
#include "ui_strings.hpp"

#include <fstream>

namespace mbpid
{

namespace
{

const TCHAR* kCardClassPath   = STR("/Game/UI/Menu_Tab/HUD_TabPlayerListEntry_InteractableV2.HUD_TabPlayerListEntry_InteractableV2_C");
const TCHAR* kListClassPath   = STR("/Game/UI/Menu_Tab/HUD_TabPlayerList_InteractableV2.HUD_TabPlayerList_InteractableV2_C");
const TCHAR* kOpenProfilePath = STR("/Script/FSD.FSDUserWidget:OpenProfile");
const TCHAR* kButtonClassPath = STR("/Game/UI/Art/WidgetParts/Basic_ButtonCutCorner.Basic_ButtonCutCorner_C");

// 跨帧保存 UObject 时一律用弱引用：ESC 列表会在关菜单/切图/退出时销毁卡片，
// 裸指针在 200ms 后可能已经失效（2026-11-17 退出崩溃即此）。
struct CardWeakRef
{
    FWeakObjectPtr ptr;

    CardWeakRef() = default;
    CardWeakRef(UObject* object) : ptr(object) {}

    auto get() const -> UObject* { return ptr.Get(); }
};

std::mutex g_card_mutex;
std::mutex g_card_diag_mutex;
std::unordered_set<std::string> g_card_diag_seen;
std::vector<CardWeakRef> g_card_queue;
std::unordered_map<UObject*, CardWeakRef> g_card_by_button; // 我们的按钮 → 卡片
CardWeakRef g_card_list;
bool g_card_scan_pending = false;
bool g_card_hook_card = false;
bool g_card_hook_list_construct = false;
bool g_card_hook_list_add = false;
bool g_card_hook_click = false;
bool g_card_warned_path = false;
bool g_card_warned_list_path = false;
int g_cards_created = 0;

// 可通过 data/profile_button.txt 热改，不用重新编译
// 默认值 = 游戏自己按钮（重生/静音/资料）在资产里的取值：FontSize=12、Width(minimum)=50、Height=20
std::string g_card_placement = "bottomleft"; // bottomleft | infobox | ranks | canvas
std::string g_card_action = "overlay";       // overlay | browser | both
float g_card_offset_x = 20.0f;               // 计算定位后再微调（设计单位，正数=向右）
float g_card_offset_y = 20.0f;               // 正数=向下
int g_card_font = 12;
float g_card_height = 20.0f;
float g_card_width = 50.0f;

// ---------- 诊断 ----------
auto mbp_card_diag(const std::string& text) -> void
{
    std::lock_guard<std::mutex> lock(g_card_diag_mutex);
    std::ofstream out(data_file(L"profile_diag.txt"), std::ios::binary | std::ios::app);
    if (!out.is_open()) return;
    out << text << "\n";
}

auto mbp_card_diag_reset() -> void
{
    std::lock_guard<std::mutex> lock(g_card_diag_mutex);
    g_card_diag_seen.clear();
    std::ofstream out(data_file(L"profile_diag.txt"), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;
    out << "# MBPId v0.1 profile button diag\n";
}

// 同一类失败只记一条，避免每次 PreConstruct/扫描刷盘
auto mbp_card_diag_once(const std::string& text) -> void
{
    {
        std::lock_guard<std::mutex> lock(g_card_diag_mutex);
        if (!g_card_diag_seen.insert(text).second) return;
    }
    mbp_card_diag(text);
}

// ---------- 配置 ----------
auto mbp_card_load_config() -> void
{
    std::ifstream in(data_file(L"profile_button.txt"), std::ios::binary);
    if (!in.is_open())
    {
        mbp_card_diag("config: profile_button.txt not found, using defaults");
        return;
    }

    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim_ascii(line.substr(0, eq));
        const std::string value = trim_ascii(line.substr(eq + 1));

        if (key == "placement")      g_card_placement = value;
        else if (key == "action")    g_card_action = value;
        else if (key == "offset_x")  g_card_offset_x = static_cast<float>(std::atof(value.c_str()));
        else if (key == "offset_y")  g_card_offset_y = static_cast<float>(std::atof(value.c_str()));
        else if (key == "font")      g_card_font = std::atoi(value.c_str());
        else if (key == "height")    g_card_height = static_cast<float>(std::atof(value.c_str()));
        else if (key == "width")     g_card_width = static_cast<float>(std::atof(value.c_str()));
    }

    if (g_card_placement != "bottomleft" && g_card_placement != "infobox"
        && g_card_placement != "ranks" && g_card_placement != "canvas")
        g_card_placement = "bottomleft";
    if (g_card_action != "overlay" && g_card_action != "browser" && g_card_action != "both")
        g_card_action = "overlay";
    if (g_card_font < 6 || g_card_font > 20) g_card_font = 12;
    if (g_card_height < 12.0f || g_card_height > 40.0f) g_card_height = 20.0f;
    if (g_card_width < 0.0f || g_card_width > 240.0f) g_card_width = 50.0f;

    mbp_card_diag("config: placement=" + g_card_placement
        + " action=" + g_card_action
        + " offset=" + std::to_string(g_card_offset_x) + "," + std::to_string(g_card_offset_y)
        + " font=" + std::to_string(g_card_font)
        + " height=" + std::to_string(g_card_height)
        + " width=" + std::to_string(g_card_width));
}

// ---------------- hook 回调：只入队，不改 UMG ----------------
auto mbp_card_noop_pre(UnrealScriptFunctionCallableContext&, void*) -> void {}

auto on_card_preconstruct_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try
    {
        UObject* card = ctx.Context;
        if (!card) return;
        std::lock_guard<std::mutex> lock(g_card_mutex);
        g_card_queue.emplace_back(card);
    }
    catch (...) {}
}

auto on_card_list_construct_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try
    {
        std::lock_guard<std::mutex> lock(g_card_mutex);
        if (ctx.Context) g_card_list = CardWeakRef(ctx.Context);
        g_card_scan_pending = true;
    }
    catch (...) {}
}

auto on_card_list_add_player_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try
    {
        std::lock_guard<std::mutex> lock(g_card_mutex);
        if (ctx.Context) g_card_list = CardWeakRef(ctx.Context);
        g_card_scan_pending = true;
    }
    catch (...) {}
}

// ---------------- 反射小工具 ----------------
auto mbp_card_call_bool(UObject* obj, const TCHAR* fn_path, const TCHAR* param, bool value) -> bool
{
    if (!obj) return false;
    UFunction* fn = find_ufunction(fn_path);
    if (!fn) return false;
    const int32 size = fn->GetParmsSize();
    if (size <= 0) return false;
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    FProperty* p = fn->FindProperty(FName(param, FNAME_Find));
    if (!p) return false;
    const int32 offset = p->GetOffset_Internal();
    if (p->GetSize() < static_cast<int32>(sizeof(bool))
        || offset < 0 || offset + static_cast<int32>(sizeof(bool)) > size) return false;
    *reinterpret_cast<bool*>(&buf[static_cast<size_t>(offset)]) = value;
    obj->ProcessEvent(fn, buf.data());
    return true;
}

// 把 N 个 float 写进 struct 参数（FVector2D=2、FAnchors=4）
auto mbp_card_call_floats(UObject* obj, const TCHAR* fn_path, const TCHAR* param,
    const float* values, int count) -> bool
{
    if (!obj || count <= 0) return false;
    UFunction* fn = find_ufunction(fn_path);
    if (!fn) return false;
    const int32 size = fn->GetParmsSize();
    if (size <= 0) return false;
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    FProperty* p = fn->FindProperty(FName(param, FNAME_Find));
    if (!p) return false;
    if (p->GetSize() < static_cast<int32>(static_cast<size_t>(count) * sizeof(float))) return false;
    void* dst = p->ContainerPtrToValuePtr<void>(buf.data());
    if (!dst) return false;
    auto* target = reinterpret_cast<float*>(dst);
    for (int i = 0; i < count; ++i) target[i] = values[i];
    obj->ProcessEvent(fn, buf.data());
    return true;
}

// 直接调用 USizeBox 的 Set*（这些会刷新 Slate，单纯写 UPROPERTY 不一定生效）
auto mbp_card_sizebox_call(UObject* sizer, const TCHAR* func, const TCHAR* param, float value) -> bool
{
    if (!sizer) return false;
    StringType path = STR("/Script/UMG.SizeBox:");
    path += func;
    UFunction* fn = find_ufunction(path.c_str());
    if (!fn) return false;
    const int32 size = fn->GetParmsSize();
    if (size <= 0) return false;
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    FProperty* p = fn->FindProperty(FName(param, FNAME_Find));
    if (!p) return false;
    *reinterpret_cast<float*>(&buf[p->GetOffset_Internal()]) = value;
    sizer->ProcessEvent(fn, buf.data());
    return true;
}

auto mbp_card_set_font(UObject* button, UClass* button_class, int font_size) -> void;

// 让新按钮长得和卡片上游戏自己的按钮（重生/静音/资料）一样。
// 关键：Basic_ButtonCutCorner_C 的外观由 BP 变量决定——CDO 是 FontSize=16、
// Width(minimum)=200、Height=33（所以默认创建出来又宽又大）；
// 游戏实例用的是 FontSize=12、Width(minimum)=50、Height=20。
// 这里是「拷变量 → SetFontSize → Update Look」，让 BP 自己把 Sizer/文字刷成小按钮。
auto mbp_card_apply_look(UObject* button, UObject* reference, UClass* button_cls) -> void
{
    if (!button || !button_cls) return;

    bool copied = false;
    if (reference)
    {
        copied |= copy_property_from(button, reference, STR("Width (minimum)"));
        copied |= copy_property_from(button, reference, STR("Height"));
    }
    mbp_card_diag(copied ? "look: copied Width(minimum)/Height from reference button"
                         : "look: reference button props not copied");

    if (g_card_width >= 0.0f) set_float_prop(button, STR("Width (minimum)"), g_card_width);
    if (g_card_height > 0.0f) set_float_prop(button, STR("Height"), g_card_height);
    mbp_card_set_font(button, button_cls, g_card_font);

    StringType path = button_cls->GetPathName();
    path += STR(":Update Look");
    if (UFunction* fn = find_ufunction(path.c_str()))
    {
        std::vector<uint8_t> buf(static_cast<size_t>(fn->GetParmsSize()), 0);
        button->ProcessEvent(fn, buf.data());
    }
    else
    {
        mbp_card_diag_once("look: Update Look not found");
    }

    // 兜底：若 BP 没把尺寸写进 Sizer（还是 200/33 那种），直接调 SizeBox setter 刷新
    UObject* sizer = get_object_prop(button, STR("Sizer"));
    if (!sizer) sizer = find_widget_in_tree(button, STR("Sizer"));
    if (!sizer)
    {
        mbp_card_diag_once("look: Sizer not found");
        return;
    }

    const float want_w = g_card_width >= 0.0f ? g_card_width : 50.0f;
    const float want_h = g_card_height > 0.0f ? g_card_height : 20.0f;

    float min_w = -1.0f, height_override = -1.0f;
    const bool has_min_w = get_float_prop(sizer, STR("MinDesiredWidth"), min_w);
    const bool has_h = get_float_prop(sizer, STR("HeightOverride"), height_override);

    if (!has_min_w || min_w > want_w + 1.0f)
        mbp_card_sizebox_call(sizer, STR("SetMinDesiredWidth"), STR("InMinDesiredWidth"), want_w);
    if (!has_h || height_override > want_h + 1.0f)
        mbp_card_sizebox_call(sizer, STR("SetHeightOverride"), STR("InHeightOverride"), want_h);
    mbp_card_sizebox_call(sizer, STR("SetMinDesiredHeight"), STR("InMinDesiredHeight"), 0.0f);

    float final_min_w = -1.0f, final_h = -1.0f;
    get_float_prop(sizer, STR("MinDesiredWidth"), final_min_w);
    get_float_prop(sizer, STR("HeightOverride"), final_h);
    mbp_card_diag("look: sizer MinDesiredWidth=" + std::to_string(final_min_w)
        + " HeightOverride=" + std::to_string(final_h));
}

auto mbp_card_set_font(UObject* button, UClass* button_class, int font_size) -> void
{
    if (!button || !button_class) return;
    StringType path = button_class->GetPathName();
    path += STR(":SetFontSize");
    UFunction* fn = find_ufunction(path.c_str());
    if (!fn)
    {
        mbp_card_diag("font: SetFontSize not found");
        return;
    }
    const int32 size = fn->GetParmsSize();
    if (size <= 0) return;
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    FProperty* p = fn->FindProperty(FName(STR("FontSize"), FNAME_Find));
    if (!p) p = fn->FindProperty(FName(STR("InFontSize"), FNAME_Find));
    if (!p) return;
    *reinterpret_cast<int32*>(&buf[p->GetOffset_Internal()]) = font_size;
    button->ProcessEvent(fn, buf.data());
}

auto mbp_card_pad_slot(UObject* button) -> void
{
    UObject* slot = get_object_prop(button, STR("Slot"));
    if (!slot) return;
    UClass* slot_cls = slot->GetClassPrivate();
    if (!slot_cls) return;

    StringType path = STR("/Script/UMG.");
    path += slot_cls->GetName();
    path += STR(":SetPadding");
    UFunction* fn = find_ufunction(path.c_str());
    if (!fn) return;

    const int32 size = fn->GetParmsSize();
    if (size <= 0) return;
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    FProperty* p = fn->FindProperty(FName(STR("InPadding"), FNAME_Find));
    if (!p) p = fn->FindProperty(FName(STR("Padding"), FNAME_Find));
    if (!p) return;
    void* dst = p->ContainerPtrToValuePtr<void>(buf.data());
    if (!dst) return;
    auto* margin = reinterpret_cast<float*>(dst); // FMargin: Left/Top/Right/Bottom
    margin[0] = 0.0f;
    margin[1] = 2.0f;
    margin[2] = 0.0f;
    margin[3] = 0.0f;
    slot->ProcessEvent(fn, buf.data());
}

// ---------- 画布定位 ----------
// bottom_left = true：卡片画布（CanvasPanel_2）左下角 → 角色头像正下方
// bottom_left = false：子控件画布（CanvasPanel_1）名字行下方
// 注意：卡片主体被 Button_Hover(UButton) 包着，插在它内容里的按钮收不到点击，
// 所以默认放在 Button_Hover 的兄弟层（CanvasPanel_2）。
auto mbp_card_add_to_canvas(UObject* canvas, UObject* button, bool bottom_left) -> bool
{
    if (!canvas || !button) return false;
    UFunction* fn = find_ufunction(STR("/Script/UMG.CanvasPanel:AddChildToCanvas"));
    if (!fn)
    {
        mbp_card_diag("canvas: AddChildToCanvas not found");
        return false;
    }
    const int32 size = fn->GetParmsSize();
    if (size <= 0) return false;
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    FProperty* content = fn->FindProperty(FName(STR("Content"), FNAME_Find));
    FProperty* ret = fn->FindProperty(FName(STR("ReturnValue"), FNAME_Find));
    if (!content || !ret) return false;

    *reinterpret_cast<UObject**>(&buf[content->GetOffset_Internal()]) = button;
    canvas->ProcessEvent(fn, buf.data());
    UObject* slot = *reinterpret_cast<UObject**>(&buf[ret->GetOffset_Internal()]);
    if (!slot)
    {
        mbp_card_diag("canvas: AddChildToCanvas returned null slot");
        return false;
    }

    mbp_card_call_bool(slot, STR("/Script/UMG.CanvasPanelSlot:SetAutoSize"), STR("InbAutoSize"), true);

    if (bottom_left)
    {
        // 初始占位（真正位置由 mbp_card_apply_position 用设计单位算好后覆盖）：
        // 万一定位一直失败，至少落在头像附近而不是卡片最左边。
        const float placeholder_x = 40.0f + g_card_offset_x;
        const float anchors[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        const float align[2]   = { 0.5f, 1.0f };
        const float pos[2]     = { placeholder_x, g_card_offset_y };
        mbp_card_call_floats(slot, STR("/Script/UMG.CanvasPanelSlot:SetAnchors"), STR("InAnchors"), anchors, 4);
        mbp_card_call_floats(slot, STR("/Script/UMG.CanvasPanelSlot:SetAlignment"), STR("InAlignment"), align, 2);
        mbp_card_call_floats(slot, STR("/Script/UMG.CanvasPanelSlot:SetPosition"), STR("InPosition"), pos, 2);
    }
    else
    {
        const float anchors[4] = { 0.0f, 0.5f, 0.0f, 0.5f };  // 左侧居中
        const float align[2]   = { 0.0f, 0.5f };
        const float pos[2]     = { g_card_offset_x, g_card_offset_y };
        mbp_card_call_floats(slot, STR("/Script/UMG.CanvasPanelSlot:SetAnchors"), STR("InAnchors"), anchors, 4);
        mbp_card_call_floats(slot, STR("/Script/UMG.CanvasPanelSlot:SetAlignment"), STR("InAlignment"), align, 2);
        mbp_card_call_floats(slot, STR("/Script/UMG.CanvasPanelSlot:SetPosition"), STR("InPosition"), pos, 2);
    }

    // 画在卡片内容之上
    if (UFunction* z = find_ufunction(STR("/Script/UMG.CanvasPanelSlot:SetZOrder")))
    {
        std::vector<uint8_t> zbuf(static_cast<size_t>(z->GetParmsSize()), 0);
        if (FProperty* zp = z->FindProperty(FName(STR("InZOrder"), FNAME_Find)))
        {
            *reinterpret_cast<int32*>(&zbuf[zp->GetOffset_Internal()]) = 40;
            slot->ProcessEvent(z, zbuf.data());
        }
    }
    return true;
}

// 位置队列：卡片刚建好时还没布局（尺寸全 0），等下一拍再算
struct PendingPos
{
    CardWeakRef card;
    CardWeakRef button;
    int tries;
};

std::vector<PendingPos> g_card_pending_pos;

// UWidget::GetDesiredSize() -> FVector2D（设计单位，简单可靠，不用解析 FGeometry）
auto mbp_card_desired_size(UObject* widget, float& w, float& h) -> bool
{
    if (!widget) return false;
    UFunction* fn = find_ufunction(STR("/Script/UMG.Widget:GetDesiredSize"));
    if (!fn) return false;
    const int32 size = fn->GetParmsSize();
    if (size <= 0) return false;

    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    widget->ProcessEvent(fn, buf.data());

    FProperty* ret = fn->FindProperty(FName(STR("ReturnValue"), FNAME_Find));
    if (!ret) return false;
    const float* v = reinterpret_cast<const float*>(ret->ContainerPtrToValuePtr<void>(buf.data()));
    if (!v) return false;

    w = v[0];
    h = v[1];
    return true;
}

// 读 UCanvasPanelSlot::LayoutData(FAnchorData: Offsets(FMargin) / Anchors / Alignment) 的 Left
auto mbp_card_slot_left(UObject* slot) -> float
{
    if (!slot) return 0.0f;
    FProperty* layout = slot->GetPropertyByNameInChain(STR("LayoutData"));
    if (!layout) return 0.0f;
    const float* v = reinterpret_cast<const float*>(layout->ContainerPtrToValuePtr<void>(slot));
    return v ? v[0] : 0.0f;
}

// 定位：水平对齐六边形头像中心，垂直贴卡片底边那一行（和「重生」同一行）。
// 全部用设计单位：名字行 main 的画布槽 Left + 头像槽 Left padding + 头像宽度/2。
// 画布槽偏移本来就是设计单位，所以跟分辨率/DPI 无关，不需要猜像素。
auto mbp_card_apply_position(UObject* card, UObject* button) -> bool
{
    if (!card || !button) return false;

    UObject* nested = get_object_prop(card, STR("Entry"));
    if (!nested) return false;

    UObject* icon = get_object_prop(nested, STR("ITM_CharacterIcon"));
    if (!icon) return false;

    float icon_w = 0.0f, icon_h = 0.0f;
    if (!mbp_card_desired_size(icon, icon_w, icon_h)) return false;
    if (icon_w < 8.0f || icon_w > 400.0f) return false; // 还没布局完，下一拍再试

    UObject* main_row = find_widget_in_tree(nested, STR("main"));
    const float main_left = main_row ? mbp_card_slot_left(get_object_prop(main_row, STR("Slot"))) : 0.0f;
    const float pad_left = slot_padding_left(get_object_prop(icon, STR("Slot")));

    const float center_x = main_left + pad_left + icon_w * 0.5f + g_card_offset_x;
    if (center_x < -4.0f || center_x > 420.0f)
    {
        mbp_card_diag_once("pos: implausible centerX=" + std::to_string(center_x));
        return false;
    }

    UObject* slot = get_object_prop(button, STR("Slot"));
    if (!slot) return false;

    const float anchors[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    const float align[2]   = { 0.5f, 1.0f };
    const float pos[2]     = { center_x, g_card_offset_y }; // offset_y 正数 = 向下
    mbp_card_call_floats(slot, STR("/Script/UMG.CanvasPanelSlot:SetAnchors"), STR("InAnchors"), anchors, 4);
    mbp_card_call_floats(slot, STR("/Script/UMG.CanvasPanelSlot:SetAlignment"), STR("InAlignment"), align, 2);
    mbp_card_call_floats(slot, STR("/Script/UMG.CanvasPanelSlot:SetPosition"), STR("InPosition"), pos, 2);

    // 定位成功才显示（避免先闪在错误位置）
    set_widget_visibility(button, 0);

    mbp_card_diag("pos: centerX=" + std::to_string(center_x)
        + " iconW=" + std::to_string(icon_w)
        + " mainLeft=" + std::to_string(main_left)
        + " padLeft=" + std::to_string(pad_left)
        + " offsetY=" + std::to_string(g_card_offset_y));
    return true;
}

auto mbp_card_process_positions() -> void
{
    std::vector<PendingPos> pending;
    {
        std::lock_guard<std::mutex> lock(g_card_mutex);
        if (g_card_pending_pos.empty()) return;
        pending.swap(g_card_pending_pos);
    }

    for (auto& item : pending)
    {
        UObject* card = item.card.get();
        UObject* button = item.button.get();
        if (!card || !button)
        {
            // 卡片/按钮已在关菜单、切图或退出时销毁；直接丢弃，绝不碰失效指针。
            continue;
        }

        bool done = false;
        try { done = mbp_card_apply_position(card, button); } catch (...) {}
        if (done) continue;

        if (item.tries < 25)
        {
            ++item.tries;
            std::lock_guard<std::mutex> lock(g_card_mutex);
            g_card_pending_pos.push_back(item);
        }
        else
        {
            mbp_card_diag_once("pos: gave up waiting for geometry");
            // 兜底：仍然显示（至少能用），放在卡片左下
            set_widget_visibility(button, 0);
        }
    }
}

// ---------- 点击处理 ----------
auto mbp_card_open_url_for(UObject* card) -> void
{
    if (!card) return;

    std::string id;
    UObject* player_state = get_object_prop(card, STR("PlayerState"));
    const std::string ps_id = get_player_state_steam_id(player_state);
    if (is_steam_id(ps_id)) id = ps_id;

    if (id.empty())
    {
        UObject* nested = get_object_prop(card, STR("Entry"));
        UObject* name_label = nested ? get_object_prop(nested, STR("DATA_playerName")) : nullptr;
        const std::string name = name_label ? trim_ascii(get_widget_text(name_label, STR("Text"))) : std::string();
        const std::string lookup = get_steam_id_by_player_name(name);
        if (is_steam_id(lookup)) id = lookup;
        mbp_card_diag("click: name='" + name + "' -> " + (id.empty() ? std::string("-") : id));
    }

    if (id.empty())
    {
        mbp_card_diag("click: no steam id for this card");
        return;
    }

    const std::string url = "https://steamcommunity.com/profiles/" + id;
    StringType url_w{ url.begin(), url.end() };
    log(STR("[MBPId] profile open -> {}\n"), url_w);
    launch_url(url);
}

// 走游戏自己的资料逻辑：Steam 平台下就是开 Steam 叠加界面里的玩家资料，不用登录网页。
// 先直接调 FSDUserWidget::OpenProfile（游戏「资料」按钮最终走的就是它），
// 拿不到再退回卡片 BTN_Gamercard 的点击处理（等价于玩家真按那个按钮）。
auto mbp_card_game_profile(UObject* card) -> bool
{
    if (!card) return false;

    auto call_fn = [card](const TCHAR* fn_path, const char* tag) -> bool {
        UFunction* fn = find_ufunction(fn_path);
        if (!fn) return false;
        const int32 size = fn->GetParmsSize();
        std::vector<uint8_t> buf(static_cast<size_t>(size > 0 ? size : 1), 0);
        card->ProcessEvent(fn, buf.data());
        mbp_card_diag(std::string("click: ") + tag + " called (parmsSize=" + std::to_string(size) + ")");
        return true;
    };

    // 主路径：卡片上「资料」按钮(BTN_Gamercard)的点击处理 —— 实测这条能开游戏内叠加界面
    UClass* card_cls = card->GetClassPrivate();
    if (card_cls)
    {
        StringType path = card_cls->GetPathName();
        path += STR(":");
        path += STR("BndEvt__BTN_Gamercard_K2Node_ComponentBoundEvent_7_OnClicked__DelegateSignature");
        if (call_fn(path.c_str(), "gamercard handler")) return true;
    }

    // 兜底：FSDUserWidget::OpenProfile（本作里这个路径其实解析不到，留着以防版本变化）
    if (call_fn(kOpenProfilePath, "OpenProfile")) return true;

    mbp_card_diag("click: game profile function unavailable");
    return false;
}

// 点击入口统一处理（两个 hook 都可能触发，用 400ms 去抖保证只开一次浏览器）
UObject* g_card_last_clicked = nullptr;
std::chrono::steady_clock::time_point g_card_last_click_time{};

auto mbp_card_handle_click(UObject* button) -> void
{
    if (!button) return;

    static const std::string our_text = wstr_to_utf8(ui_strings().profile_button);
    const std::string text = trim_ascii(get_widget_text(button, STR("ButtonText")));
    if (text != our_text) return;

    const auto now = std::chrono::steady_clock::now();
    if (button == g_card_last_clicked
        && std::chrono::duration_cast<std::chrono::milliseconds>(now - g_card_last_click_time).count() < 400)
    {
        return;
    }
    g_card_last_clicked = button;
    g_card_last_click_time = now;

    UObject* card = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_card_mutex);
        auto it = g_card_by_button.find(button);
        if (it != g_card_by_button.end()) card = it->second.get();
    }
    mbp_card_diag(std::string("click: our button, card=") + (card ? "found" : "missing"));
    if (!card) return;

    if (g_card_action != "browser")
    {
        // 默认：游戏内叠加界面（Steam 资料页），不用登录网页
        if (mbp_card_game_profile(card) && g_card_action == "overlay") return;
    }
    if (g_card_action != "overlay")
    {
        mbp_card_open_url_for(card);
    }
}

// Basic_ButtonCutCorner_C:Click（BP 函数）
auto on_card_button_click_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try { mbp_card_handle_click(ctx.Context); } catch (...) {}
}

// 内部 Button_0 的点击事件（最直接：UIButton 的 OnClicked 一定走这里）
auto on_card_button_bndevt_click_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try { mbp_card_handle_click(ctx.Context); } catch (...) {}
}

// ---------- 卡片打理 ----------
auto mbp_card_pick_container(UObject* card, UObject* nested, std::string& where, bool& bottom_left) -> UObject*
{
    bottom_left = false;

    if (g_card_placement == "bottomleft")
    {
        where = "bottomleft";
        bottom_left = true;
        // 卡片画布 = Button_Hover 的父控件（按父链取，避免按名字撞车）
        UObject* hover = call_get_parent(nested);
        UObject* canvas = hover ? call_get_parent(hover) : nullptr;
        if (!canvas) canvas = find_widget_in_tree(card, STR("CanvasPanel_2"));
        return canvas;
    }
    if (g_card_placement == "ranks")
    {
        where = "ranks";
        return find_widget_in_tree(nested, STR("Ranks"));
    }
    if (g_card_placement == "canvas")
    {
        where = "canvas";
        return find_widget_in_tree(nested, STR("CanvasPanel_1"));
    }
    where = "infobox";
    return get_object_prop(nested, STR("InfoBox"));
}

// 容器里是否已经有我们的按钮：容器（InfoBox/Ranks/画布）原本不含 Basic_ButtonCutCorner，
// 除了卡片自己那几个（Gamercard/Kick/Mute…），所以「不在已知清单里的 Basic_ButtonCutCorner」
// 就是我们已经加过的那一个。用它代替「已处理控件集合」，避免控件指针回收后地址复用导致漏加。
auto mbp_card_find_our_button(UObject* container, UObject* card) -> UObject*
{
    if (!container || !card) return nullptr;

    const TCHAR* known_names[] = {
        STR("BTN_Gamercard"), STR("BTN_Kick"), STR("BTN_mute"),
        STR("BTN_unmute"), STR("BTN_Respawn_Spacerig")
    };

    const int32 count = call_get_children_count(container);
    for (int32 i = 0; i < count; ++i)
    {
        UObject* child = call_get_child_at(container, i);
        if (!child) continue;

        bool known = false;
        for (auto* name : known_names)
        {
            if (get_object_prop(card, name) == child) { known = true; break; }
        }
        if (known) continue;

        UClass* child_cls = child->GetClassPrivate();
        if (!child_cls) continue;
        if (wstr_to_utf8(child_cls->GetName()).find("Basic_ButtonCutCorner") != std::string::npos) return child;
    }
    return nullptr;
}

// 每张卡片：加一个「主页」按钮（幂等）
auto mbp_card_setup(UObject* card) -> void
{
    if (!card) return;
    if (is_under_server_list(card)) return;

    UClass* card_cls = card->GetClassPrivate();
    if (!card_cls)
    {
        mbp_card_diag_once("skip: no class");
        return;
    }
    const std::string cls_name = wstr_to_utf8(card_cls->GetName());
    if (cls_name.find("HUD_TabPlayerListEntry") == std::string::npos)
    {
        mbp_card_diag_once("skip: class=" + cls_name);
        return;
    }

    UObject* nested = get_object_prop(card, STR("Entry"));
    if (!nested)
    {
        mbp_card_diag_once("fail: Entry prop missing");
        return;
    }

    std::string where;
    bool bottom_left = false;
    UObject* container = mbp_card_pick_container(card, nested, where, bottom_left);
    if (!container)
    {
        mbp_card_diag_once("fail: container " + where + " not found");
        return;
    }

    // 已经有按钮：只刷新「按钮→卡片」映射（卡片会被复用）
    if (UObject* existing = mbp_card_find_our_button(container, card))
    {
        std::lock_guard<std::mutex> lock(g_card_mutex);
        g_card_by_button[existing] = CardWeakRef(card);
        return;
    }

    UObject* ref = get_object_prop(card, STR("BTN_Respawn_Spacerig"));
    if (!ref) ref = get_object_prop(card, STR("BTN_mute"));
    if (!ref) ref = get_object_prop(card, STR("BTN_Gamercard"));
    if (!ref)
    {
        mbp_card_diag_once("fail: no reference button");
        return;
    }
    UClass* button_cls = ref->GetClassPrivate();
    if (!button_cls)
    {
        mbp_card_diag_once("fail: no button class");
        return;
    }

    UObject* button = create_profile_button(card, button_cls);
    if (!button)
    {
        mbp_card_diag_once("fail: WidgetBlueprintLibrary::Create returned null");
        return;
    }

    set_profile_button_text(button, button_cls);
    mbp_card_apply_look(button, ref, button_cls);

    const bool placed = (where == "canvas" || where == "bottomleft")
        ? mbp_card_add_to_canvas(container, button, bottom_left)
        : call_add_child(container, button);
    if (!placed)
    {
        mbp_card_diag("fail: place into " + where);
        return;
    }

    set_widget_visibility(button, bottom_left ? 1 : 0); // bottomleft 先 Collapsed，等几何算好再显示
    if (where != "canvas" && where != "bottomleft") mbp_card_pad_slot(button);

    {
        std::lock_guard<std::mutex> lock(g_card_mutex);
        g_card_by_button[button] = CardWeakRef(card);
        if (bottom_left) g_card_pending_pos.push_back(PendingPos{ CardWeakRef(card), CardWeakRef(button), 0 });
    }

    ++g_cards_created;
    const int32 children = call_get_children_count(container);
    UClass* container_cls = container->GetClassPrivate();
    mbp_card_diag("created: placement=" + where
        + " container=" + (container_cls ? wstr_to_utf8(container_cls->GetName()) : std::string("?"))
        + " children=" + std::to_string(children)
        + " total=" + std::to_string(g_cards_created));
    log(STR("[MBPId] profile button created (#{}, containerChildren={})\n"),
        g_cards_created, children);
}

// ---------- hook 安装（先 StaticFindObject 判空，绝不把 null 传给 RegisterHook） ----------
auto mbp_card_hook_one(UClass* cls, const TCHAR* func_name,
    const UnrealScriptFunctionCallable& post, std::pair<int, int>& ids) -> bool
{
    if (!cls) return false;

    StringType path = cls->GetPathName();
    path += STR(":");
    path += func_name;

    UFunction* fn = find_ufunction(path.c_str());
    if (!fn)
    {
        mbp_card_diag_once("hook: function not found -> " + wstr_to_utf8(path));
        return false;
    }

    try
    {
        ids = UObjectGlobals::RegisterHook(fn, mbp_card_noop_pre, post, nullptr);
    }
    catch (const std::exception& e)
    {
        mbp_card_diag_once(std::string("hook: RegisterHook threw: ") + e.what() + " -> " + wstr_to_utf8(path));
        return false;
    }
    catch (...)
    {
        mbp_card_diag_once("hook: RegisterHook threw -> " + wstr_to_utf8(path));
        return false;
    }

    mbp_card_diag("hook: ok ids=(" + std::to_string(ids.first) + "," + std::to_string(ids.second) + ") -> " + wstr_to_utf8(path));
    return true;
}

auto mbp_card_install_hooks() -> void
{
    // 不再 hook OpenProfile：现在点击是直接调用游戏自己的资料/OpenProfile 逻辑，
    // 若再挂这个 hook，叠加界面打开的同时还会多弹一个浏览器页。

    if (!g_card_hook_card)
    {
        UClass* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, kCardClassPath);
        if (cls)
        {
            std::pair<int, int> ids{0, 0};
            if (mbp_card_hook_one(cls, STR("PreConstruct"), on_card_preconstruct_post, ids))
            {
                g_card_hook_card = true;

                // 兜底：装 hook 时 ESC 列表可能已经开着了，做一次性扫描
                std::vector<UObject*> existing;
                UObjectGlobals::FindAllOf(STR("HUD_TabPlayerListEntry_InteractableV2_C"), existing);
                {
                    std::lock_guard<std::mutex> lock(g_card_mutex);
                    for (UObject* c : existing) g_card_queue.emplace_back(c);
                }
                mbp_card_diag("hook: card PreConstruct installed, existing cards=" + std::to_string(existing.size()));
            }
        }
        else if (!g_card_warned_path)
        {
            g_card_warned_path = true;
            mbp_card_diag(std::string("hook: card class not loaded yet -> ") + wstr_to_utf8(kCardClassPath));
        }
    }

    if (!g_card_hook_click)
    {
        UClass* btn_cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, kButtonClassPath);
        if (btn_cls)
        {
            std::pair<int, int> ids{0, 0};
            bool any = false;
            if (mbp_card_hook_one(btn_cls, STR("Click"), on_card_button_click_post, ids)) any = true;
            if (mbp_card_hook_one(btn_cls,
                    STR("BndEvt__Button_0_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature"),
                    on_card_button_bndevt_click_post, ids))
            {
                any = true;
            }
            if (any) g_card_hook_click = true;
        }
        else
        {
            mbp_card_diag_once(std::string("hook: button class not loaded yet -> ") + wstr_to_utf8(kButtonClassPath));
        }
    }

    UClass* list_cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, kListClassPath);
    if (list_cls)
    {
        std::pair<int, int> ids{0, 0};
        if (!g_card_hook_list_construct
            && mbp_card_hook_one(list_cls, STR("Construct"), on_card_list_construct_post, ids))
        {
            g_card_hook_list_construct = true;
        }

        std::pair<int, int> ids2{0, 0};
        if (!g_card_hook_list_add
            && mbp_card_hook_one(list_cls, STR("AddPlayer"), on_card_list_add_player_post, ids2))
        {
            g_card_hook_list_add = true;
        }
    }
    else if (!g_card_warned_list_path)
    {
        g_card_warned_list_path = true;
        mbp_card_diag(std::string("hook: list class not loaded yet -> ") + wstr_to_utf8(kListClassPath));
    }
}

} // namespace

auto card_panel_init() -> void
{
    mbp_card_diag_reset();
    mbp_card_load_config();
    mbp_card_diag(std::string("card class  : ") + wstr_to_utf8(kCardClassPath));
    mbp_card_diag(std::string("list class  : ") + wstr_to_utf8(kListClassPath));
    mbp_card_diag(std::string("openprofile : ") + wstr_to_utf8(kOpenProfilePath));
}

auto card_panel_pump() -> void
{
    const bool all_ready = g_card_hook_card && g_card_hook_list_construct && g_card_hook_click;
    if (!all_ready)
    {
        static auto last_try = std::chrono::steady_clock::now() - std::chrono::seconds(5);
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_try).count() >= 1000)
        {
            last_try = now;
            try { mbp_card_install_hooks(); } catch (...) {}
        }
    }

    // 位置计算每拍都试一次（几何没准备好就等下一拍，200ms）
    try { mbp_card_process_positions(); } catch (...) {}

    std::vector<CardWeakRef> cards;
    bool scan = false;
    UObject* list = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_card_mutex);
        if (g_card_queue.empty() && !g_card_scan_pending) return;
        cards.swap(g_card_queue);
        scan = g_card_scan_pending;
        g_card_scan_pending = false;
        list = g_card_list.get();
    }

    for (auto& ref : cards)
    {
        UObject* card = ref.get();
        if (!card) continue;
        try { mbp_card_setup(card); } catch (...) {}
    }

    if (scan && list)
    {
        UObject* panel = get_object_prop(list, STR("PlayersPanel"));
        if (!panel)
        {
            mbp_card_diag("scan: list has no PlayersPanel");
            return;
        }
        const int32 count = call_get_children_count(panel);
        for (int32 i = 0; i < count; ++i)
        {
            UObject* child = call_get_child_at(panel, i);
            try { mbp_card_setup(child); } catch (...) {}
        }
    }
}

} // namespace mbpid
