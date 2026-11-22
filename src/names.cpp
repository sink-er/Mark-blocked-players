// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// 房内被屏蔽玩家红名：三处名字控件涂红 / 恢复。
#include "names.hpp"

#include "blocklist.hpp"
#include "players.hpp"

#include <fstream>

namespace mbpid
{

namespace
{

struct NameLabelTarget
{
    const TCHAR* class_name;
    const TCHAR* text_prop;
};

// 已确认的三处名字控件（游戏内队友列表 / 本地玩家信息 / 空间站与选人队友列表）
const NameLabelTarget kNameLabelTargets[] = {
    { STR("HUD_PlayerName_Label_C"),      STR("DATA_PlayerName") },
    { STR("HUD_TeamDisplay_SmallItem_C"), STR("PlayerName") },
    { STR("ITEM_SpaceRig_TeamMember_C"),  STR("TXT_PlayerName") },
};

// TextBlock 指针 -> 改色前的 RGBA；只在命中时记录，未命中时恢复。
std::unordered_map<UObject*, std::array<float, 4>> g_name_original_colors;

// 存在 test_all_names.txt 时，把所有找到的名字都涂红（只用于检查样式）。
auto test_all_names_enabled() -> bool
{
    std::ifstream f(data_file(L"test_all_names.txt"));
    return f.good();
}

auto read_text_block_color(UObject* text_block, std::array<float, 4>& out) -> bool
{
    text_block = as_live_object(text_block);
    if (!text_block) return false;
    FProperty* prop = text_block->GetPropertyByNameInChain(STR("ColorAndOpacity"));
    if (!prop) return false;
    const float* color = reinterpret_cast<const float*>(prop->ContainerPtrToValuePtr<void>(text_block));
    if (!color) return false;
    out = { color[0], color[1], color[2], color[3] };
    return true;
}

auto write_text_block_color(UObject* text_block, float r, float g, float b, float a) -> bool
{
    text_block = as_live_object(text_block);
    if (!text_block) return false;
    FProperty* prop = text_block->GetPropertyByNameInChain(STR("ColorAndOpacity"));
    if (!prop) return false;
    void* value = prop->ContainerPtrToValuePtr<void>(text_block);
    if (!value) return false;

    float* color = reinterpret_cast<float*>(value);
    color[0] = r; color[1] = g; color[2] = b; color[3] = a;

    // 游戏可能已经用 SetColorAndOpacity 设过静态 Slate 颜色，单纯改 UPROPERTY
    // 不一定刷新显示；这里再调用一次 UTextBlock::SetColorAndOpacity。
    static UFunction* fn = find_ufunction(STR("/Script/UMG.TextBlock:SetColorAndOpacity"));
    if (fn)
    {
        const int32 size = fn->GetParmsSize();
        if (size > 0)
        {
            std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
            FProperty* in = fn->FindProperty(FName(STR("InColorAndOpacity"), FNAME_Find));
            if (in)
            {
                void* dst = in->ContainerPtrToValuePtr<void>(buf.data());
                if (dst)
                {
                    in->CopyCompleteValue(dst, value);
                    text_block->ProcessEvent(fn, buf.data());
                    in->DestroyValue(dst);
                }
            }
        }
    }
    return true;
}

// 返回是否命中；out_ps_id / out_lookup_id / out_text 供 names.txt 诊断。
auto apply_name_color(UObject* widget, const TCHAR* text_prop, bool test_all,
                      std::string& out_ps_id, std::string& out_lookup_id, std::string& out_text) -> bool
{
    widget = as_live_object(widget);
    if (!widget) return false;

    UObject* text_block = get_object_prop(widget, text_prop);
    if (!text_block) return false;

    // FindAllOf 会把 pending kill 的控件也返回；此时 Text 的 FTextHistory 可能已析构，
    // 再调 ToString() 就会落到 _purecall（2026-11-20 02:35 崩溃）。
    text_block = as_live_object(text_block);
    if (!text_block) return false;

    out_text = trim_ascii(get_widget_text(text_block, STR("Text")));

    // 精确路径：控件自己的 PlayerState 属性（SetPlayerState 写入的队友状态）。
    out_ps_id = get_player_state_steam_id(get_widget_own_player_state(widget));
    // 兜底：名字反查；同名不同 ID 时 get_steam_id_by_player_name 返回空，不会误判。
    out_lookup_id = get_steam_id_by_player_name(out_text);

    const std::string id = !out_ps_id.empty() ? out_ps_id : out_lookup_id;
    const bool blocked = test_all || is_blocked(id);
    if (blocked)
    {
        if (g_name_original_colors.find(text_block) == g_name_original_colors.end())
        {
            std::array<float, 4> original{};
            if (read_text_block_color(text_block, original))
            {
                g_name_original_colors[text_block] = original;
            }
        }
        write_text_block_color(text_block, 1.0f, 0.15f, 0.15f, 1.0f);
    }
    else
    {
        auto it = g_name_original_colors.find(text_block);
        if (it != g_name_original_colors.end())
        {
            const auto original = it->second;
            g_name_original_colors.erase(it);
            write_text_block_color(text_block, original[0], original[1], original[2], original[3]);
        }
    }

    return blocked;
}

} // namespace

auto update_blocked_name_colors() -> void
{
    refresh_blocklist_if_needed();

    const bool test_all = test_all_names_enabled();
    std::string log_text;
    int labels = 0;
    int blocked_names = 0;

    for (const auto& target : kNameLabelTargets)
    {
        std::vector<UObject*> widgets;
        UObjectGlobals::FindAllOf(target.class_name, widgets);
        for (UObject* widget : widgets)
        {
            widget = as_live_object(widget);
            if (!widget) continue;
            UObject* text_block = get_object_prop(widget, target.text_prop);
            if (!text_block) continue;

            std::string ps_id;
            std::string lookup_id;
            std::string text;
            const bool blocked = apply_name_color(widget, target.text_prop, test_all, ps_id, lookup_id, text);
            ++labels;
            if (blocked) ++blocked_names;

            log_text += wstr_to_utf8(StringType(target.class_name));
            log_text += "\t";
            log_text += ps_id.empty() ? "-" : ps_id;
            log_text += "\t";
            log_text += lookup_id.empty() ? "-" : lookup_id;
            log_text += "\t";
            log_text += text;
            log_text += "\t";
            log_text += blocked ? (test_all ? "TEST_ALL" : "BLOCKED") : "-";
            log_text += "\n";
        }
    }

    static std::string last_log;
    if (!log_text.empty() && log_text != last_log)
    {
        last_log = log_text;
        std::ofstream out(data_file(L"names.txt"), std::ios::binary | std::ios::trunc);
        if (out.is_open())
        {
            out.write(log_text.c_str(), static_cast<std::streamsize>(log_text.size()));
            out.close();
        }
    }

    static int last_labels = -1;
    static int last_blocked = -1;
    if (labels != last_labels || blocked_names != last_blocked)
    {
        last_labels = labels;
        last_blocked = blocked_names;
        log(STR("[MBPId] nameLabels={} blockedNames={} blocklist={} testAll={}\n"),
            labels, blocked_names, blocked_count(), test_all);
    }
}

} // namespace mbpid
