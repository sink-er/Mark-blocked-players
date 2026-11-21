// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// MBPId：玩家 SteamID 导出 + 服务器列表「已屏蔽房主」标记 + 房内红名
//
// 功能一：遍历 PlayerState，导出 UniqueId/名字 → MBPId/data/players.txt
// 功能二：遍历服务器列表行 ITM_ServerList_Entry_C
//     - 从 Session 反射调用 FSDGetHostUserID 取房主 SteamID64
//     - 直接读 Steamworks 的 Ignored/Blocked 列表（不可用时回退 ignored.txt）
//     - 仅在命中时，在 TXT_Region 之后插入独立的红色 [已屏蔽玩家] 标签
//     - 绝不改写游戏自己的 TXT_Region 文本
// 功能三：每 2 秒扫描房内名字控件，命中屏蔽名单的玩家名字改成红色
//     - HUD_TeamDisplay_SmallItem.PlayerName
//     - HUD_PlayerName_Label.DATA_PlayerName
//     - ITEM_SpaceRig_TeamMember.TXT_PlayerName
//     - 诊断写 MBPId/data/names.txt
//
// 所有 UObject/UMG 操作都在 UEngine::Tick 回调（游戏线程）执行；
// UE4SS 的 on_update 仅用于补装游戏线程 hook，见文件末尾。
// UI 文案随 GameUserSettings.ini 的 Language 自动切换，见 docs/TRANSLATIONS.md。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>
#include <array>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <filesystem>

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

using namespace RC;
using namespace RC::Unreal;

// 所有路径都从 UE4SS 的 Mods 目录推导，仓库里不写死任何本机路径。
static auto mods_directory() -> const std::filesystem::path&
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

static auto data_file(const wchar_t* name) -> std::wstring
{
    static const std::filesystem::path dir = mods_directory() / L"MBPId" / L"data";
    return (dir / name).wstring();
}

static auto blocklist_file() -> const std::wstring&
{
    static const std::wstring path = (mods_directory() / L"MBPMarker" / L"data" / L"ignored.txt").wstring();
    return path;
}

// ---------- 字符串工具 ----------
static auto wstr_to_utf8(const StringType& w) -> std::string
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

static auto fstring_to_utf8(const FString& str) -> std::string
{
    return wstr_to_utf8(str.GetCharTArray().Num() ? StringType(str.GetCharTArray().GetData(), str.GetCharTArray().Num()) : StringType{});
}

// 任何来自 FindAllOf / 属性读取的 UObject 先过一遍弱引用：
// pending kill 的对象仍在对象数组里，FindAllOf 会返回它；
// 直接读属性或调用虚函数会触发 pure virtual / AV。
static auto as_live_object(UObject* object) -> UObject*
{
    if (!object) return nullptr;
    FWeakObjectPtr weak(object);
    return weak.Get();
}

// ---------- 通用属性工具 ----------
static auto export_property(UObject* obj, const TCHAR* prop_name) -> std::string
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

// 读 FText 属性
static auto get_widget_text(UObject* obj, const TCHAR* prop_name) -> std::string
{
    obj = as_live_object(obj);
    if (!obj) return {};
    auto* prop = obj->GetPropertyByNameInChain(prop_name);
    if (!prop) return {};
    auto* value = prop->ContainerPtrToValuePtr<FText>(obj);
    if (!value) return {};
    return wstr_to_utf8(value->ToString());
}

// 写 FText 属性
static auto set_widget_text(UObject* obj, const TCHAR* prop_name, const TCHAR* new_text) -> bool
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

// 读对象属性（BindWidget 控件）
static auto get_object_prop(UObject* obj, const TCHAR* prop_name) -> UObject*
{
    obj = as_live_object(obj);
    if (!obj) return nullptr;
    auto* prop = obj->GetPropertyByNameInChain(prop_name);
    if (!prop) return nullptr;
    UObject** p = prop->ContainerPtrToValuePtr<UObject*>(obj);
    return p ? as_live_object(*p) : nullptr;
}

// ---------- UMG 反射调用小工具 ----------
static auto find_ufunction(const TCHAR* path) -> UFunction*
{
    return UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
}

// ---------- 屏蔽名单（SteamID64 集合） ----------
static std::unordered_set<std::string> g_blocked_ids;
static std::mutex g_blocklist_mutex;
static std::mutex g_blocklist_refresh_mutex;
static bool g_blocklist_logged = false;

static auto trim_ascii(std::string s) -> std::string
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

static auto lower_ascii(std::string s) -> std::string
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// ---------- 游戏语言 / 界面文案 ----------
// 不依赖 UE4 的 FInternationalization 私有布局，直接读游戏自己的
// Saved/Config/WindowsNoEditor/GameUserSettings.ini 里的 [Internationalization]。
// 语言切换后重启游戏即可生效。
static auto game_config_directory() -> std::filesystem::path
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

static auto read_game_language() -> std::string
{
    const std::filesystem::path root = game_config_directory();
    const wchar_t* subdirs[] = { L"WindowsNoEditor", L"Windows" };

    for (const wchar_t* subdir : subdirs)
    {
        std::ifstream in(root / subdir / L"GameUserSettings.ini", std::ios::binary);
        if (!in.is_open()) continue;

        bool internationalization = false;
        std::string line;
        while (std::getline(in, line))
        {
            if (line.size() >= 3
                && static_cast<unsigned char>(line[0]) == 0xEF
                && static_cast<unsigned char>(line[1]) == 0xBB
                && static_cast<unsigned char>(line[2]) == 0xBF)
            {
                line.erase(0, 3);
            }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "[Internationalization]")
            {
                internationalization = true;
                continue;
            }
            if (!line.empty() && line[0] == '[')
            {
                internationalization = false;
                continue;
            }
            if (!internationalization) continue;

            if (line.rfind("Language=", 0) == 0 || line.rfind("Locale=", 0) == 0)
            {
                const std::string value = trim_ascii(line.substr(line.find('=') + 1));
                if (!value.empty()) return value;
            }
        }
    }
    return "en";
}

static auto normalize_language(std::string code) -> std::string
{
    code = lower_ascii(trim_ascii(code));
    for (char& c : code)
    {
        if (c == '_') c = '-';
    }

    if (code.rfind("zh-cn", 0) == 0 || code.rfind("zh-hans", 0) == 0) return "zh-hans";
    if (code.rfind("zh-tw", 0) == 0 || code.rfind("zh-hant", 0) == 0) return "zh-hant";
    if (code.rfind("es-419", 0) == 0) return "es-419";
    if (code.rfind("pt", 0) == 0) return "pt-br";
    if (code.rfind("es", 0) == 0) return "es";

    const auto dash = code.find('-');
    return dash == std::string::npos ? code : code.substr(0, dash);
}

static auto ui_language() -> const std::string&
{
    static const std::string language = normalize_language(read_game_language());
    return language;
}

struct UiStrings
{
    std::wstring blocked_tag;
    std::wstring profile_button;
};

static auto ui_strings() -> const UiStrings&
{
    static const UiStrings strings = [] {
        const std::string& lang = ui_language();
        // Deep Rock Galactic 的 17 种界面语言；未知语言回退英文。
        if (lang == "en") return UiStrings{L"[Blocked Player]", L"Profile"};
        if (lang == "fr") return UiStrings{L"[Joueur bloqu\u00E9]", L"Profil"};
        if (lang == "de") return UiStrings{L"[Blockierter Spieler]", L"Profil"};
        if (lang == "es") return UiStrings{L"[Jugador bloqueado]", L"Perfil"};
        if (lang == "es-419") return UiStrings{L"[Jugador bloqueado]", L"Perfil"};
        if (lang == "pl") return UiStrings{L"[Zablokowany gracz]", L"Profil"};
        if (lang == "pt-br") return UiStrings{L"[Jogador bloqueado]", L"Perfil"};
        if (lang == "ru") return UiStrings{L"[\u0417\u0430\u0431\u043B\u043E\u043A\u0438\u0440\u043E\u0432\u0430\u043D\u043D\u044B\u0439 \u0438\u0433\u0440\u043E\u043A]", L"\u041F\u0440\u043E\u0444\u0438\u043B\u044C"};
        if (lang == "zh-hans") return UiStrings{L"[\u5DF2\u5C4F\u853D\u73A9\u5BB6]", L"\u4E3B\u9875"};
        if (lang == "zh-hant") return UiStrings{L"[\u5DF2\u5C01\u9396\u73A9\u5BB6]", L"\u500B\u4EBA\u6A94\u6848"};
        if (lang == "uk") return UiStrings{L"[\u0417\u0430\u0431\u043B\u043E\u043A\u043E\u0432\u0430\u043D\u0438\u0439 \u0433\u0440\u0430\u0432\u0435\u0446\u044C]", L"\u041F\u0440\u043E\u0444\u0456\u043B\u044C"};
        if (lang == "it") return UiStrings{L"[Giocatore bloccato]", L"Profilo"};
        if (lang == "ja") return UiStrings{L"[\u30D6\u30ED\u30C3\u30AF\u3057\u305F\u30D7\u30EC\u30A4\u30E4\u30FC]", L"\u30D7\u30ED\u30D5\u30A3\u30FC\u30EB"};
        if (lang == "ko") return UiStrings{L"[\uCC28\uB2E8\uD55C \uD50C\uB808\uC774\uC5B4]", L"\uD504\uB85C\uD544"};
        if (lang == "tr") return UiStrings{L"[Engellenen Oyuncu]", L"Profil"};
        if (lang == "hu") return UiStrings{L"[Letiltott j\u00E1t\u00E9kos]", L"Profil"};
        if (lang == "ro") return UiStrings{L"[Juc\u0103tor blocat]", L"Profil"};
        return UiStrings{L"[Blocked Player]", L"Profile"};
    }();
    return strings;
}


// v017 的 friends 接口。直接读 k_EFriendFlagIgnored(0x200) / k_EFriendFlagBlocked(0x1)，
// 不再依赖 PowerShell 导出；ignored.txt 只做 SteamAPI 未就绪时的兜底。
// 实时 Steam 屏蔽名单：游戏进程已经初始化 SteamAPI，steam_api64.dll 也导出
// v017 的 friends 接口。直接读 k_EFriendFlagIgnored(0x200) / k_EFriendFlagBlocked(0x1)，
// 不再依赖 PowerShell 导出；ignored.txt 只做 SteamAPI 未就绪时的兜底。
using SteamFriendsFn = void* (__cdecl*)();
using SteamUserFn = void* (__cdecl*)();
using SteamIsRunningFn = bool (__cdecl*)();
using SteamGetSteamIDFn = unsigned long long (__cdecl*)(void*);
using SteamFriendCountFn = int (__cdecl*)(void*, int);
using SteamFriendByIndexFn = unsigned long long (__cdecl*)(void*, int, int);

static auto load_live_steam_blocklist(std::unordered_set<std::string>& ids) -> bool
{
    static HMODULE steam_module = nullptr;
    static SteamFriendsFn get_friends = nullptr;
    static SteamUserFn get_user = nullptr;
    static SteamIsRunningFn is_running = nullptr;
    static SteamGetSteamIDFn get_steam_id = nullptr;
    static SteamFriendCountFn get_count = nullptr;
    static SteamFriendByIndexFn get_by_index = nullptr;
    static bool resolved = false;
    static auto last_resolve = std::chrono::steady_clock::now() - std::chrono::seconds(5);

    const auto now = std::chrono::steady_clock::now();
    if (!resolved)
    {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_resolve).count() < 5) return false;
        last_resolve = now;

        steam_module = GetModuleHandleW(L"steam_api64.dll");
        if (!steam_module) return false;

        get_friends = reinterpret_cast<SteamFriendsFn>(GetProcAddress(steam_module, "SteamAPI_SteamFriends_v017"));
        get_user = reinterpret_cast<SteamUserFn>(GetProcAddress(steam_module, "SteamAPI_SteamUser_v021"));
        is_running = reinterpret_cast<SteamIsRunningFn>(GetProcAddress(steam_module, "SteamAPI_IsSteamRunning"));
        get_steam_id = reinterpret_cast<SteamGetSteamIDFn>(GetProcAddress(steam_module, "SteamAPI_ISteamUser_GetSteamID"));
        get_count = reinterpret_cast<SteamFriendCountFn>(GetProcAddress(steam_module, "SteamAPI_ISteamFriends_GetFriendCount"));
        get_by_index = reinterpret_cast<SteamFriendByIndexFn>(GetProcAddress(steam_module, "SteamAPI_ISteamFriends_GetFriendByIndex"));
        resolved = get_friends && get_count && get_by_index;
        if (!resolved) return false;
    }

    if (is_running && !is_running()) return false;

    // 确认 SteamAPI 已经初始化（本机 SteamID 非 0），避免在引擎初始化前调用接口。
    if (get_user && get_steam_id)
    {
        void* user = get_user();
        if (!user || get_steam_id(user) == 0) return false;
    }

    void* friends = get_friends();
    if (!friends) return false;

    bool any = false;
    for (const int flags : { 0x200, 0x1 }) // Ignored / Blocked
    {
        const int count = get_count(friends, flags);
        if (count < 0) continue;
        any = true;
        for (int i = 0; i < count; ++i)
        {
            const unsigned long long id = get_by_index(friends, i, flags);
            if (id != 0) ids.insert(std::to_string(id));
        }
    }
    return any;
}

static auto load_blocklist() -> void
{
    static bool live_seen = false;

    std::unordered_set<std::string> ids;
    const bool live = load_live_steam_blocklist(ids);
    if (live) live_seen = true;

    // 已经成功读过 Steam 列表后，偶尔一次接口失败（Steam 正忙/退出中）不要回退到旧文件，
    // 否则刚屏蔽的两个 ID 会短暂消失；保留上一次实时结果即可。
    if (!live && live_seen) return;

    if (!live)
    {
        const std::wstring fallback_path = blocklist_file();
        std::ifstream in(fallback_path, std::ios::binary);
        if (!in.is_open())
        {
            std::lock_guard<std::mutex> lock(g_blocklist_mutex);
            if (!g_blocklist_logged)
            {
                Output::send<LogLevel::Verbose>(STR("[MBPId] blocklist not found: {}\n"), fallback_path.c_str());
                g_blocklist_logged = true;
            }
            g_blocked_ids.clear();
            return;
        }

        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#') continue;

            const size_t tab = line.find('\t');
            const std::string id = trim_ascii(tab == std::string::npos ? line : line.substr(0, tab));
            if (id.size() < 10) continue;

            bool all_digits = true;
            for (char c : id)
            {
                if (c < '0' || c > '9') { all_digits = false; break; }
            }
            if (all_digits) ids.insert(id);
        }
    }

    std::lock_guard<std::mutex> lock(g_blocklist_mutex);
    const bool changed = ids != g_blocked_ids;
    g_blocked_ids.swap(ids);
    if (changed || !g_blocklist_logged)
    {
        Output::send<LogLevel::Verbose>(STR("[MBPId] blocklist loaded: {} ids (source={})\n"),
            g_blocked_ids.size(), live ? STR("steam") : STR("file"));
        g_blocklist_logged = true;
    }
}

// 每 2 秒最多刷新一次（Steam 实时；失败时回退 ignored.txt）
static auto refresh_blocklist_if_needed() -> void
{
    std::lock_guard<std::mutex> refresh_lock(g_blocklist_refresh_mutex);
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(3);
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count() < 2) return;
    last = now;
    load_blocklist();
}

static auto is_blocked(const std::string& steam_id) -> bool
{
    if (steam_id.empty()) return false;
    std::lock_guard<std::mutex> lock(g_blocklist_mutex);
    return g_blocked_ids.find(steam_id) != g_blocked_ids.end();
}

static auto blocked_count() -> size_t
{
    std::lock_guard<std::mutex> lock(g_blocklist_mutex);
    return g_blocked_ids.size();
}

// ---------- 房主 SteamID ----------
// 行控件的 Session 是 FBlueprintSessionResult；反射调用
// USessionHandling::FSDGetHostUserID(Session) -> FString，避免自己解析 UniqueNetId。
static auto get_host_id(UObject* row) -> std::string
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
            Output::send<LogLevel::Verbose>(STR("[MBPId] FSDGetHostUserID unavailable (fn={} self={})\n"),
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
            Output::send<LogLevel::Verbose>(STR("[MBPId] FSDGetHostUserID params missing (Result={} Return={} Session={})\n"),
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

// UWidget::GetParent() -> UPanelWidget*
static auto call_get_parent(UObject* widget) -> UObject*
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

// UPanelWidget::GetChildrenCount() -> int32
static auto call_get_children_count(UObject* panel) -> int32
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

// UPanelWidget::GetChildAt(int32) -> UWidget*
static auto call_get_child_at(UObject* panel, int32 index) -> UObject*
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

// UPanelWidget::InsertChildAt(int32, UWidget*) -> UPanelSlot*
static auto call_insert_child_at(UObject* panel, int32 index, UObject* child) -> bool
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

// UPanelWidget::AddChild(UWidget*) -> UPanelSlot*（InsertChildAt 失败时兜底）
static auto call_add_child(UObject* panel, UObject* child) -> bool
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
// 把 src 的某个属性值整体复制给 dst（用于继承字体/颜色样式）
static auto copy_property_from(UObject* dst, UObject* src, const TCHAR* name) -> bool
{
    if (!dst || !src) return false;
    FProperty* dp = dst->GetPropertyByNameInChain(name);
    FProperty* sp = src->GetPropertyByNameInChain(name);
    if (!dp || !sp) return false;
    dp->CopyCompleteValue(dp->ContainerPtrToValuePtr<void>(dst), sp->ContainerPtrToValuePtr<void>(src));
    return true;
}

// 把控件颜色改成红色（FSlateColor 内部第一个成员是 FLinearColor）
static auto force_red(UObject* widget) -> bool
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

// UPanelWidget::RemoveChild(UWidget*) -> bool
static auto call_remove_child(UObject* panel, UObject* child) -> bool
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

// 父容器里是否已经有我们的标签（名字可能被自动加后缀，用前缀判断）
static auto has_our_tag(UObject* parent) -> bool
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

static auto count_our_tags(UObject* parent) -> int32
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
static auto remove_all_our_tags(UObject* parent) -> int32
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

// ---------- 行控件打标（只插入自有控件，不改游戏文本） ----------
// 只有房主 SteamID 命中 ignored.txt 时才加标签；未命中或会话已换则移除旧标签。
static auto tag_row(UObject* row, const std::string* known_host_id = nullptr) -> bool
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
            Output::send<LogLevel::Verbose>(STR("[MBPId] cleaned legacy [TEST] from TXT_Region\n"));
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
        Output::send<LogLevel::Verbose>(STR("[MBPId] HIT host={} -> tagged row\n"), host_w);
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
static bool g_hooked = false;

static auto on_set_session_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try { tag_row(ctx.Context); } catch (...) {}
}

// 在第一次看到行控件时挂 hook（post 阶段：行内文字已经填好）
static auto ensure_hook() -> void
{
    if (g_hooked) return;

    std::vector<UObject*> rows;
    UObjectGlobals::FindAllOf(STR("ITM_ServerList_Entry_C"), rows);
    if (rows.empty()) return;

    UClass* cls = rows[0]->GetClassPrivate();
    if (!cls)
    {
        Output::send<LogLevel::Verbose>(STR("[MBPId] row class is null\n"));
        return;
    }

    // UE4SS 没有 UClass::FindFunctionByName，用「完整对象路径」注册
    StringType func_path = cls->GetFullName();
    func_path += STR(":SetSession");
    Output::send<LogLevel::Verbose>(STR("[MBPId] trying hook path: {}\n"), func_path);

    auto ids = UObjectGlobals::RegisterHook(func_path,
        [](UnrealScriptFunctionCallableContext&, void*) {},
        on_set_session_post,
        nullptr);
    g_hooked = true;
    Output::send<LogLevel::Verbose>(STR("[MBPId] RegisterHook ids=({}, {})\n"), ids.first, ids.second);
}

// ---------- 服务器列表扫描 ----------
static auto scan_rows() -> void
{
    ensure_hook();
    refresh_blocklist_if_needed();

    std::vector<UObject*> rows;
    UObjectGlobals::FindAllOf(STR("ITM_ServerList_Entry_C"), rows);
    if (rows.empty()) return;

    std::string log;
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

        log += name;
        log += "\t";
        log += region;
        log += "\t";
        log += host_id.empty() ? "-" : host_id;
        log += "\t";
        log += blocked ? "BLOCKED" : "-";
        log += "\t";

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
            log += pcls + " count=" + std::to_string(count) + " idx=" + std::to_string(found);
        }

        log += "\t";
        if (region_widget && region_widget->GetClassPrivate())
        {
            log += wstr_to_utf8(region_widget->GetClassPrivate()->GetFullName());
        }
        log += "\t";
        log += session;
        log += "\n";

        // hook 还没触发时（例如列表已经在屏幕上）也在这里兜底
        if (tag_row(row, &host_id)) ++tagged;
    }

    std::ofstream out(data_file(L"rows.txt"), std::ios::binary | std::ios::trunc);
    if (out.is_open())
    {
        out.write(log.c_str(), static_cast<std::streamsize>(log.size()));
        out.close();
    }

    Output::send<LogLevel::Verbose>(STR("[MBPId] rows={} hostIds={} blockedRows={} hooked={} blockedTotal={}\n"),
        rows.size(), host_ids, tagged, g_hooked, blocked_count());
}

// ---------- 玩家扫描（原有功能） ----------
// 玩家名（小写、trim） -> SteamID64；同名不同 ID 时标记 ambiguous，红名逻辑不会用它。
struct PlayerNameId
{
    std::string id;
    bool ambiguous = false;
};

static std::unordered_map<std::string, PlayerNameId> g_player_name_to_id;
static std::mutex g_player_name_map_mutex;

static auto dump_players() -> void
{
    std::vector<UObject*> states;
    UObjectGlobals::FindAllOf(STR("PlayerState"), states);

    std::string out;
    std::unordered_map<std::string, PlayerNameId> name_map;
    int with_id = 0;
    std::vector<std::string> seen;

    for (UObject* ps : states)
    {
        if (!ps) continue;

        std::string name = trim_ascii(export_property(ps, STR("PlayerNamePrivate")));
        if (name.empty()) name = trim_ascii(export_property(ps, STR("PlayerName")));

        std::string uid = trim_ascii(export_property(ps, STR("UniqueId")));
        if (uid.empty()) continue;

        bool dup = false;
        for (auto& s : seen) { if (s == uid) { dup = true; break; } }
        if (dup) continue;
        seen.push_back(uid);
        ++with_id;

        if (!name.empty())
        {
            const std::string key = lower_ascii(name);
            auto it = name_map.find(key);
            if (it == name_map.end())
            {
                name_map[key] = { uid, false };
            }
            else if (it->second.id != uid)
            {
                it->second.ambiguous = true;
            }
        }
        out += uid + "\t" + name + "\n";
    }

    if (with_id == 0) return;

    {
        std::lock_guard<std::mutex> lock(g_player_name_map_mutex);
        g_player_name_to_id.swap(name_map);
    }

    const std::wstring tmp = data_file(L"players.tmp");
    const std::wstring final_path = data_file(L"players.txt");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f.is_open()) { f.write(out.c_str(), static_cast<std::streamsize>(out.size())); f.close(); }
    }
    _wremove(final_path.c_str());
    _wrename(tmp.c_str(), final_path.c_str());

    Output::send<LogLevel::Verbose>(STR("[MBPId] PlayerStates={} uniqueWithId={}\n"), states.size(), with_id);
}

// ---------- 房内玩家名红色标记 ----------
struct NameLabelTarget
{
    const TCHAR* class_name;
    const TCHAR* text_prop;
};

// 已确认的三处名字控件（游戏内队友列表 / 本地玩家信息 / 空间站与选人队友列表）
static const NameLabelTarget kNameLabelTargets[] = {
    { STR("HUD_PlayerName_Label_C"),      STR("DATA_PlayerName") },
    { STR("HUD_TeamDisplay_SmallItem_C"), STR("PlayerName") },
    { STR("ITEM_SpaceRig_TeamMember_C"),  STR("TXT_PlayerName") },
};

// TextBlock 指针 -> 改色前的 RGBA；只在命中时记录，未命中时恢复。
static std::unordered_map<UObject*, std::array<float, 4>> g_name_original_colors;

// 存在 test_all_names.txt 时，把所有找到的名字都涂红（只用于检查样式）。
static auto test_all_names_enabled() -> bool
{
    std::ifstream f(data_file(L"test_all_names.txt"));
    return f.good();
}

static auto get_steam_id_by_player_name(const std::string& name) -> std::string
{
    const std::string key = lower_ascii(trim_ascii(name));
    if (key.empty()) return {};
    std::lock_guard<std::mutex> lock(g_player_name_map_mutex);
    auto it = g_player_name_to_id.find(key);
    if (it == g_player_name_to_id.end() || it->second.ambiguous) return {};
    return it->second.id;
}

static auto get_player_state_steam_id(UObject* player_state) -> std::string
{
    if (!player_state) return {};
    return trim_ascii(export_property(player_state, STR("UniqueId")));
}

// 控件自己保存的 PlayerState（SetPlayerState 函数写入的那个成员）。
// 只处理强引用 ObjectProperty，避免把 TWeakObjectPtr 当裸指针读。
static auto get_widget_own_player_state(UObject* widget) -> UObject*
{
    widget = as_live_object(widget);
    if (!widget) return nullptr;
    FProperty* prop = widget->GetPropertyByNameInChain(STR("PlayerState"));
    if (!prop) return nullptr;

    const std::string cpp_type = fstring_to_utf8(prop->GetCPPType());
    if (cpp_type.find("Weak") != std::string::npos || cpp_type.find("Soft") != std::string::npos) return nullptr;

    UObject** value = prop->ContainerPtrToValuePtr<UObject*>(widget);
    return value ? as_live_object(*value) : nullptr;
}

static auto read_text_block_color(UObject* text_block, std::array<float, 4>& out) -> bool
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

static auto write_text_block_color(UObject* text_block, float r, float g, float b, float a) -> bool
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
static auto apply_name_color(UObject* widget, const TCHAR* text_prop, bool test_all,
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

static auto update_blocked_name_colors() -> void
{
    refresh_blocklist_if_needed();

    const bool test_all = test_all_names_enabled();
    std::string log;
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

            log += wstr_to_utf8(StringType(target.class_name));
            log += "\t";
            log += ps_id.empty() ? "-" : ps_id;
            log += "\t";
            log += lookup_id.empty() ? "-" : lookup_id;
            log += "\t";
            log += text;
            log += "\t";
            log += blocked ? (test_all ? "TEST_ALL" : "BLOCKED") : "-";
            log += "\n";
        }
    }

    static std::string last_log;
    if (!log.empty() && log != last_log)
    {
        last_log = log;
        std::ofstream out(data_file(L"names.txt"), std::ios::binary | std::ios::trunc);
        if (out.is_open())
        {
            out.write(log.c_str(), static_cast<std::streamsize>(log.size()));
            out.close();
        }
    }

    static int last_labels = -1;
    static int last_blocked = -1;
    if (labels != last_labels || blocked_names != last_blocked)
    {
        last_labels = labels;
        last_blocked = blocked_names;
        Output::send<LogLevel::Verbose>(STR("[MBPId] nameLabels={} blockedNames={} blocklist={} testAll={}\n"),
            labels, blocked_names, blocked_count(), test_all);
    }
}

// ---------- ESC/Tab 玩家列表「主页」按钮 ----------
// 目标控件：HUD_TabPlayerListEntry_InteractableV2_C（带“静音”的队友卡片）。
// 在 HorizontalBox_Actions 里、MuteSwitcher 后面加一个同款 Basic_ButtonCutCorner_C。
static std::unordered_map<UObject*, UObject*> g_profile_button_by_entry;
static std::unordered_map<UObject*, std::string> g_profile_url_by_button;
static std::unordered_set<UObject*> g_profile_known_buttons;
static std::unordered_set<UObject*> g_profile_hover_shown;
static bool g_profile_hover_hooked = false;
static bool g_profile_construct_hooked = false;
static std::unordered_map<UObject*, UObject*> g_teammate_buttons;
static std::unordered_map<UObject*, uint8> g_teammate_visible;
static std::mutex g_profile_mutex;
static bool g_profile_click_hooked = false;

static auto is_steam_id(const std::string& id) -> bool
{
    if (id.size() != 17) return false;
    for (char c : id)
    {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

static auto call_get_owning_player(UObject* widget) -> UObject*
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

// 取本地玩家控制器。队友条目的 GetOwningPlayer() 可能返回空，
// 而 WidgetBlueprintLibrary::Create 需要有效的 OwningPlayer 才会真正创建控件。
static auto mbp_local_player_controller(UObject* world_context) -> UObject*
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

static auto create_profile_button(UObject* entry, UClass* button_class) -> UObject*
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
            Output::send<LogLevel::Verbose>(STR("[MBPId] profile Create unavailable (fn={} self={})\n"),
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
            Output::send<LogLevel::Verbose>(STR("[MBPId] profile Create params missing\n"));
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

static auto set_render_scale(UObject* widget, float x, float y) -> bool
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

static auto set_widget_visibility(UObject* widget, uint8 visibility) -> bool
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
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    FProperty* p = fn->FindProperty(FName(STR("InVisibility"), FNAME_Find));
    if (!p) return prop != nullptr;
    *reinterpret_cast<uint8*>(&buf[p->GetOffset_Internal()]) = visibility;
    widget->ProcessEvent(fn, buf.data());
    return true;
}

static auto clear_button_on_clicked(UObject* button) -> void
{
    if (!button) return;
    FProperty* prop = button->GetPropertyByNameInChain(STR("OnClicked"));
    if (!prop) return;
    auto* multicast = static_cast<FMulticastDelegateProperty*>(prop);
    multicast->ClearDelegate(button, prop->ContainerPtrToValuePtr<void>(button));
}

static auto get_entry_player_name(UObject* entry) -> std::string
{
    if (!entry) return {};
    const TCHAR* names[] = { STR("DATA_playerName"), STR("PlayerName"), STR("TXT_PlayerName") };
    for (auto* name : names)
    {
        UObject* text_block = get_object_prop(entry, name);
        if (!text_block) continue;
        const std::string text = trim_ascii(get_widget_text(text_block, STR("Text")));
        if (!text.empty()) return text;
    }
    return {};
}

static auto set_float_prop(UObject* obj, const TCHAR* name, float value) -> bool
{
    if (!obj) return false;
    FProperty* prop = obj->GetPropertyByNameInChain(name);
    if (!prop) return false;
    float* ptr = reinterpret_cast<float*>(prop->ContainerPtrToValuePtr<void>(obj));
    if (!ptr) return false;
    *ptr = value;
    return true;
}

static auto is_panel_class(UObject* obj) -> bool
{
    static UClass* panel_class = nullptr;
    if (!panel_class) panel_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.PanelWidget"));
    return obj && panel_class && obj->IsA(panel_class);
}

static auto find_widget_in_tree(UObject* widget, const TCHAR* target, int depth = 0) -> UObject*
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

static auto set_size_props(UObject* sizer, float width, float height) -> void
{
    if (!sizer) return;
    set_float_prop(sizer, STR("WidthOverride"), width);
    set_float_prop(sizer, STR("HeightOverride"), height);
    set_float_prop(sizer, STR("MinDesiredWidth"), 0.0f);
    set_float_prop(sizer, STR("MinDesiredHeight"), 0.0f);
}

static auto copy_profile_button_size(UObject* button, UObject* entry) -> void
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

static auto call_is_hovered(UObject* widget) -> bool
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

static auto on_profile_check_hover(UnrealScriptFunctionCallableContext& ctx, void*) -> void
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
        Output::send<LogLevel::Verbose>(STR("[MBPId] profile hover show\n"));
    }
    catch (...) {}
}

static auto on_profile_hover_begin(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try
    {
        UObject* entry = ctx.Context;
        if (!entry) return;
        UObject* button = get_object_prop(entry, STR("BTN_Gamercard"));
        if (!button) return;
        set_widget_visibility(button, 0);
        Output::send<LogLevel::Verbose>(STR("[MBPId] profile hover show\n"));
    }
    catch (...) {}
}

static auto ensure_profile_hover_hook(UObject* entry) -> void
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
    Output::send<LogLevel::Verbose>(STR("[MBPId] profile hover hook ids=({}, {})\n"), ids.first, ids.second);
}

static auto set_profile_button_text(UObject* button, UClass* button_class) -> void
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

static auto is_child_of(UObject* parent, UObject* child) -> bool
{
    if (!parent || !child) return false;
    const int32 count = call_get_children_count(parent);
    for (int32 i = 0; i < count; ++i)
    {
        if (call_get_child_at(parent, i) == child) return true;
    }
    return false;
}

static auto add_profile_button(UObject* entry, UObject* actions, UObject* button) -> void
{
    if (!actions || !button) return;

    // 已经存在就保持原 slot（原 slot 的尺寸/对齐是游戏调好的），不要 remove/add。
    if (is_child_of(actions, button)) return;

    call_add_child(actions, button);
}

static auto launch_url(const std::string& url) -> void
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
            Output::send<LogLevel::Verbose>(STR("[MBPId] LaunchURL unavailable (fn={} self={})\n"),
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

static auto on_profile_button_click_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
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
            Output::send<LogLevel::Verbose>(STR("[MBPId] profile click raw mapped={} url={}\n"),
                !url.empty(), url_w);
        }

        if (!url.empty())
        {
            launch_url(url);
        }
    }
    catch (...) {}
}

// 点击“主页”后，游戏会调用条目的 OpenProfile（原 Profile 按钮逻辑）。
// 这里在 post 阶段额外打开浏览器；游戏自己的资料窗暂时也会出现。
static auto on_open_profile_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try
    {
        UObject* entry = ctx.Context;
        if (!entry) return;

        UObject* player_state = get_object_prop(entry, STR("PlayerState"));
        const std::string ps_id = get_player_state_steam_id(player_state);
        const std::string entry_name = get_entry_player_name(entry);
        const std::string lookup_id = get_steam_id_by_player_name(entry_name);

        std::string id;
        if (is_steam_id(ps_id)) id = ps_id;
        else if (is_steam_id(lookup_id)) id = lookup_id;

        if (id.empty())
        {
            Output::send<LogLevel::Verbose>(STR("[MBPId] profile open but no steam id\n"));
            return;
        }

        std::string url = "https://steamcommunity.com/profiles/";
        url += id;
        StringType url_w{ url.begin(), url.end() };
        Output::send<LogLevel::Verbose>(STR("[MBPId] profile open -> {}\n"), url_w);
        launch_url(url);
    }
    catch (...) {}
}

static auto ensure_profile_click_hook(UObject* entry) -> void
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
    Output::send<LogLevel::Verbose>(STR("[MBPId] profile open hook ids=({}, {})\n"), ids.first, ids.second);
}

static auto update_profile_buttons() -> void
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
    std::string diag;

    for (UObject* entry : entries)
    {
        if (!entry) continue;
        ensure_profile_hover_hook(entry);

        UObject* actions = get_object_prop(entry, STR("HorizontalBox_Actions"));
        if (!actions) continue;

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

        // 保留游戏原来的 OpenProfile 绑定；点击后由 on_open_profile_post 额外开浏览器。
        // （游戏自己的资料窗暂时保留，后续可再隐藏。）

        if (text_ready_buttons.find(button) == text_ready_buttons.end())
        {
            set_profile_button_text(button, button_class);
            copy_profile_button_size(button, entry);
            set_widget_visibility(button, 0);
            text_ready_buttons.insert(button);
        }

        add_profile_button(entry, actions, button);

        {
            std::lock_guard<std::mutex> lock(g_profile_mutex);
            g_profile_button_by_entry[entry] = button;
        }

        UObject* player_state = get_object_prop(entry, STR("PlayerState"));
        const std::string ps_id = get_player_state_steam_id(player_state);
        const std::string entry_name = get_entry_player_name(entry);
        const std::string lookup_id = get_steam_id_by_player_name(entry_name);
        std::string id;
        if (is_steam_id(ps_id)) id = ps_id;
        else if (is_steam_id(lookup_id)) id = lookup_id;
        const bool steam = is_steam_id(id);

        std::string ps_name = export_property(player_state, STR("PlayerNamePrivate"));
        if (ps_name.empty()) ps_name = entry_name;

        std::string url;
        if (steam)
        {
            url = "https://steamcommunity.com/profiles/";
            url += id;
            ++valid_buttons;
        }

        StringType cls_name = button->GetClassPrivate() ? button->GetClassPrivate()->GetName() : StringType(STR("?"));
        diag += wstr_to_utf8(cls_name);
        diag += "\t";
        diag += ps_id.empty() ? "-" : ps_id;
        diag += "\t";
        diag += lookup_id.empty() ? "-" : lookup_id;
        diag += "\t";
        diag += ps_name.empty() ? "-" : ps_name;
        diag += "\t";
        diag += entry_name.empty() ? "-" : entry_name;
        diag += "\t";
        diag += url.empty() ? "-" : url;
        diag += "\n";

        {
            std::lock_guard<std::mutex> lock(g_profile_mutex);
            if (url.empty()) g_profile_url_by_button.erase(button);
            else g_profile_url_by_button[button] = url;
            seen_buttons.insert(button);
        }
    }

    static std::string last_diag;
    if (!diag.empty() && diag != last_diag)
    {
        last_diag = diag;
        std::ofstream out(data_file(L"profile_buttons.txt"), std::ios::binary | std::ios::trunc);
        if (out.is_open())
        {
            out.write(diag.c_str(), static_cast<std::streamsize>(diag.size()));
            out.close();
        }
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
        Output::send<LogLevel::Verbose>(STR("[MBPId] profileEntries={} profileButtons={}\n"),
            entries.size(), valid_buttons);
    }
}

// 极简版：每个 Gamercard 按钮只处理一次；不做 WidgetTree 遍历/每轮 SetVisibility。
static auto is_widget_visible(UObject* widget) -> bool
{
    if (!widget) return false;
    FProperty* prop = widget->GetPropertyByNameInChain(STR("Visibility"));
    if (!prop) return true;
    const uint8* value = reinterpret_cast<const uint8*>(prop->ContainerPtrToValuePtr<void>(widget));
    if (!value) return true;
    return *value == 0 || *value >= 3; // Visible / HitTestInvisible / SelfHitTestInvisible
}

static auto update_profile_buttons_simple() -> void
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
        Output::send<LogLevel::Verbose>(STR("[MBPId] profileVisible={} profileReady={}\n"), visible_entries, ready);
    }
}

// 队友卡片：把原生 BTN_Gamercard 从动作栏移到名字/职业下方，作为常驻“主页”按钮。
static auto update_profile_buttons_under_name() -> void
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
        Output::send<LogLevel::Verbose>(STR("[MBPId] profileTeammates={} profileReady={}\n"), teammate_count, ready);
    }
}

static auto is_under_server_list(UObject* entry) -> bool
{
    if (!entry) return false;
    const StringType path = entry->GetPathName();
    return path.find(STR("ServerList")) != StringType::npos
        || path.find(STR("ITM_ServerList")) != StringType::npos;
}

static auto setup_teammate_profile_button(UObject* entry) -> void
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
    Output::send<LogLevel::Verbose>(STR("[MBPId] teammate profile button created\n"));
}

static auto on_entry_construct(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try
    {
        if (ctx.Context) setup_teammate_profile_button(ctx.Context);
    }
    catch (...) {}
}

static auto ensure_profile_construct_hook() -> void
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
    Output::send<LogLevel::Verbose>(STR("[MBPId] profile construct hook ids=({}, {})\n"), ids.first, ids.second);

    for (UObject* entry : entries) setup_teammate_profile_button(entry);
}

static auto update_teammate_visibility() -> void
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

// =====================================================================================
// ESC/Tab 玩家列表「主页」按钮 —— v0.1（事件驱动；取代上面的轮询实现）
// 上面的旧实现（ensure_profile_construct_hook / update_teammate_visibility /
// update_profile_buttons* 等）已不再被 on_update 调用，保留仅供参考/回滚。
// -------------------------------------------------------------------------------------
// 资产级事实（由 Content/UI/Menu_Tab 的 UAssetGUI JSON 离线确认，不要再猜）：
//   · 卡片 HUD_TabPlayerListEntry_InteractableV2_C 没有 Construct；只有
//     PreConstruct / SetPlayer / OnPlayerSet / CheckHover / Update Mute Button / BndEvt__*。
//     → 旧实现按 ":Construct" 找 UFunction 必然失败，hook 从未装上，日志 0 条。
//   · VerticalBox_0 / CanvasPanel_2 / DATA_playerName 都不是卡片类的 UPROPERTY；
//     名字与职业在子控件 HUD_TabPlayerListEntryV2_C 里（卡片属性 Entry）。
//   · 卡片 UPROPERTY：BTN_Gamercard/BTN_Kick/BTN_mute/BTN_unmute/BTN_Respawn_Spacerig/
//     Button_Hover/Entry/HorizontalBox_Actions/MuteSwitcher/PlayerState/PlayerController。
//   · 子控件 UPROPERTY：DATA_playerName/DATA_className/InfoBox/VerticalBox_AmmoHealth/
//     PutStuffHere_Rightside/ITM_*；Ranks 与 CanvasPanel_1 不是属性，只能按名字走 WidgetTree。
//   · ESC 列表 HUD_TabPlayerList_InteractableV2_C 有 Construct 与 AddPlayer，
//     卡片挂在 PlayersPanel(HorizontalBox)。
//   · OpenProfile 继承自 native 类 FSDUserWidget：/Script/FSD.FSDUserWidget:OpenProfile。
//   · Basic_ButtonCutCorner_C.Sizer 是 UPROPERTY(SizeBox)：MinDesiredWidth=200、
//     WidthOverride=200、bOverride_MinDesiredWidth=true、bOverride_HeightOverride=true。
//     → 把 MinDesiredWidth 置 0 就自适应宽度；bool 覆盖位不必碰（WidthOverride 本就没被覆盖）。
// 设计：hook 里只入队；on_update 每 200ms 落一次队列；稳态没有 FindAllOf。
// =====================================================================================
static const TCHAR* kCardClassPath  = STR("/Game/UI/Menu_Tab/HUD_TabPlayerListEntry_InteractableV2.HUD_TabPlayerListEntry_InteractableV2_C");
static const TCHAR* kListClassPath  = STR("/Game/UI/Menu_Tab/HUD_TabPlayerList_InteractableV2.HUD_TabPlayerList_InteractableV2_C");
static const TCHAR* kOpenProfilePath = STR("/Script/FSD.FSDUserWidget:OpenProfile");
static const TCHAR* kButtonClassPath = STR("/Game/UI/Art/WidgetParts/Basic_ButtonCutCorner.Basic_ButtonCutCorner_C");

// 跨帧保存 UObject 时一律用弱引用：ESC 列表会在关菜单/切图/退出时销毁卡片，
// 裸指针在 200ms 后可能已经失效（2026-11-17 退出崩溃即此）。
struct CardWeakRef
{
    FWeakObjectPtr ptr;

    CardWeakRef() = default;
    CardWeakRef(UObject* object) : ptr(object) {}

    auto get() const -> UObject* { return ptr.Get(); }
};

static std::mutex g_card_mutex;
static std::mutex g_card_diag_mutex;
static std::unordered_set<std::string> g_card_diag_seen;
static std::vector<CardWeakRef> g_card_queue;
static std::unordered_map<UObject*, CardWeakRef> g_card_by_button; // 我们的按钮 → 卡片
static CardWeakRef g_card_list;
static bool g_card_scan_pending = false;
static bool g_card_hook_card = false;
static bool g_card_hook_list_construct = false;
static bool g_card_hook_list_add = false;
static bool g_card_hook_click = false;
static bool g_card_warned_path = false;
static bool g_card_warned_list_path = false;
static int g_cards_created = 0;

// 可通过 data/profile_button.txt 热改，不用重新编译
// 默认值 = 游戏自己按钮（重生/静音/资料）在资产里的取值：FontSize=12、Width(minimum)=50、Height=20
static std::string g_card_placement = "bottomleft"; // bottomleft | infobox | ranks | canvas
static std::string g_card_action = "overlay";       // overlay | browser | both
static float g_card_offset_x = 20.0f;               // 计算定位后再微调（设计单位，正数=向右）
static float g_card_offset_y = 20.0f;               // 正数=向下
static int g_card_font = 12;
static float g_card_height = 20.0f;
static float g_card_width = 50.0f;

static auto mbp_card_diag(const std::string& text) -> void
{
    std::lock_guard<std::mutex> lock(g_card_diag_mutex);
    std::ofstream out(data_file(L"profile_diag.txt"), std::ios::binary | std::ios::app);
    if (!out.is_open()) return;
    out << text << "\n";
}

static auto mbp_card_diag_reset() -> void
{
    std::lock_guard<std::mutex> lock(g_card_diag_mutex);
    g_card_diag_seen.clear();
    std::ofstream out(data_file(L"profile_diag.txt"), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;
    out << "# MBPId v0.1 profile button diag\n";
}

// 同一类失败只记一条，避免每次 PreConstruct/扫描刷盘
static auto mbp_card_diag_once(const std::string& text) -> void
{
    {
        std::lock_guard<std::mutex> lock(g_card_diag_mutex);
        if (!g_card_diag_seen.insert(text).second) return;
    }
    mbp_card_diag(text);
}

static auto mbp_card_load_config() -> void
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

static auto mbp_card_noop_pre(UnrealScriptFunctionCallableContext&, void*) -> void {}

// ---------------- hook 回调：只入队，不改 UMG ----------------
static auto on_card_preconstruct_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
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

static auto on_card_list_construct_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try
    {
        std::lock_guard<std::mutex> lock(g_card_mutex);
        if (ctx.Context) g_card_list = CardWeakRef(ctx.Context);
        g_card_scan_pending = true;
    }
    catch (...) {}
}

static auto on_card_list_add_player_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
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
static auto mbp_card_call_bool(UObject* obj, const TCHAR* fn_path, const TCHAR* param, bool value) -> bool
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

static auto mbp_card_call_vec2(UObject* obj, const TCHAR* fn_path, const TCHAR* param, float x, float y) -> bool
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

// 按钮内部 Sizer(SizeBox)：清最小宽度 → 宽度由文字决定；高度压到紧凑值
static auto get_float_prop(UObject* obj, const TCHAR* name, float& out) -> bool
{
    if (!obj) return false;
    FProperty* prop = obj->GetPropertyByNameInChain(name);
    if (!prop) return false;
    float* ptr = reinterpret_cast<float*>(prop->ContainerPtrToValuePtr<void>(obj));
    if (!ptr) return false;
    out = *ptr;
    return true;
}

// 直接调用 USizeBox 的 Set*（这些会刷新 Slate，单纯写 UPROPERTY 不一定生效）
static auto mbp_card_sizebox_call(UObject* sizer, const TCHAR* func, const TCHAR* param, float value) -> bool
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

// 让新按钮长得和卡片上游戏自己的按钮（重生/静音/资料）一样。
// 关键：Basic_ButtonCutCorner_C 的外观由 BP 变量决定——CDO 是 FontSize=16、
// Width(minimum)=200、Height=33（所以默认创建出来又宽又大）；
// 游戏实例用的是 FontSize=12、Width(minimum)=50、Height=20。
// 这里是「拷变量 → SetFontSize → Update Look」，让 BP 自己把 Sizer/文字刷成小按钮。
static auto mbp_card_set_font(UObject* button, UClass* button_class, int font_size) -> void;

static auto mbp_card_apply_look(UObject* button, UObject* reference, UClass* button_cls) -> void
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

static auto mbp_card_set_font(UObject* button, UClass* button_class, int font_size) -> void
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

static auto mbp_card_pad_slot(UObject* button) -> void
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

// 把 N 个 float 写进 struct 参数（FVector2D=2、FAnchors=4）
static auto mbp_card_call_floats(UObject* obj, const TCHAR* fn_path, const TCHAR* param,
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

// bottom_left = true：卡片画布（CanvasPanel_2）左下角 → 角色头像正下方
// bottom_left = false：子控件画布（CanvasPanel_1）名字行下方
// 注意：卡片主体被 Button_Hover(UButton) 包着，插在它内容里的按钮收不到点击，
// 所以默认放在 Button_Hover 的兄弟层（CanvasPanel_2）。
static auto mbp_card_add_to_canvas(UObject* canvas, UObject* button, bool bottom_left) -> bool
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

static std::vector<PendingPos> g_card_pending_pos;

// UWidget::GetDesiredSize() -> FVector2D（设计单位，简单可靠，不用解析 FGeometry）
static auto mbp_card_desired_size(UObject* widget, float& w, float& h) -> bool
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
static auto mbp_card_slot_left(UObject* slot) -> float
{
    if (!slot) return 0.0f;
    FProperty* layout = slot->GetPropertyByNameInChain(STR("LayoutData"));
    if (!layout) return 0.0f;
    const float* v = reinterpret_cast<const float*>(layout->ContainerPtrToValuePtr<void>(slot));
    return v ? v[0] : 0.0f;
}

// UPanelSlot::Padding（FMargin: Left/Top/Right/Bottom）的 Left
static auto mbp_card_slot_padding_left(UObject* slot) -> float
{
    if (!slot) return 0.0f;
    FProperty* pad = slot->GetPropertyByNameInChain(STR("Padding"));
    if (!pad) return 0.0f;
    const float* v = reinterpret_cast<const float*>(pad->ContainerPtrToValuePtr<void>(slot));
    return v ? v[0] : 0.0f;
}

// 定位：水平对齐六边形头像中心，垂直贴卡片底边那一行（和「重生」同一行）。
// 全部用设计单位：名字行 main 的画布槽 Left + 头像槽 Left padding + 头像宽度/2。
// 画布槽偏移本来就是设计单位，所以跟分辨率/DPI 无关，不需要猜像素。
static auto mbp_card_apply_position(UObject* card, UObject* button) -> bool
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
    const float pad_left = mbp_card_slot_padding_left(get_object_prop(icon, STR("Slot")));

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

static auto mbp_card_process_positions() -> void
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
static auto mbp_card_open_url_for(UObject* card) -> void
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
    Output::send<LogLevel::Verbose>(STR("[MBPId] profile open -> {}\n"), url_w);
    launch_url(url);
}

// 走游戏自己的资料逻辑：Steam 平台下就是开 Steam 叠加界面里的玩家资料，不用登录网页。
// 先直接调 FSDUserWidget::OpenProfile（游戏「资料」按钮最终走的就是它），
// 拿不到再退回卡片 BTN_Gamercard 的点击处理（等价于玩家真按那个按钮）。
static auto mbp_card_game_profile(UObject* card) -> bool
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
static UObject* g_card_last_clicked = nullptr;
static std::chrono::steady_clock::time_point g_card_last_click_time{};

static auto mbp_card_handle_click(UObject* button) -> void
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
static auto on_card_button_click_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try { mbp_card_handle_click(ctx.Context); } catch (...) {}
}

// 内部 Button_0 的点击事件（最直接：UIButton 的 OnClicked 一定走这里）
static auto on_card_button_bndevt_click_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
{
    try { mbp_card_handle_click(ctx.Context); } catch (...) {}
}

static auto mbp_card_pick_container(UObject* card, UObject* nested, std::string& where, bool& bottom_left) -> UObject*
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
static auto mbp_card_find_our_button(UObject* container, UObject* card) -> UObject*
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

// ---------------- 每张卡片：加一个「主页」按钮（幂等） ----------------
static auto mbp_card_setup(UObject* card) -> void
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
    Output::send<LogLevel::Verbose>(STR("[MBPId] profile button created (#{}, containerChildren={})\n"),
        g_cards_created, children);
}

// ---------------- hook 安装（先 StaticFindObject 判空，绝不把 null 传给 RegisterHook） ----------------
static auto mbp_card_hook_one(UClass* cls, const TCHAR* func_name,
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

static auto mbp_card_install_hooks() -> void
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

static auto mbp_card_pump() -> void
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

static auto mbp_card_init() -> void
{
    mbp_card_diag_reset();
    mbp_card_load_config();
    mbp_card_diag(std::string("card class  : ") + wstr_to_utf8(kCardClassPath));
    mbp_card_diag(std::string("list class  : ") + wstr_to_utf8(kListClassPath));
    mbp_card_diag(std::string("openprofile : ") + wstr_to_utf8(kOpenProfilePath));
}

static auto run_game_thread_update() -> void
{
    // ProcessEvent 兜底回调可能在 ProcessEvent 内部再次进入；防止递归。
    static thread_local bool in_update = false;
    if (in_update) return;
    in_update = true;
    struct UpdateGuard
    {
        bool& flag;
        ~UpdateGuard() { flag = false; }
    } guard{ in_update };

    using clock = std::chrono::steady_clock;
    static auto last = clock::now();
    static auto last_names = clock::now() - std::chrono::seconds(4);
    static auto last_profiles = clock::now() - std::chrono::seconds(1);
    static auto perf_last = clock::now();
    static double perf_dump = 0.0, perf_scan = 0.0, perf_names = 0.0, perf_profiles = 0.0;
    static int perf_dump_n = 0, perf_scan_n = 0, perf_names_n = 0, perf_profiles_n = 0;
    auto now = clock::now();

    auto elapsed_ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= 3000)
    {
        last = now;
        auto t0 = clock::now();
        try { dump_players(); } catch (...) {}
        auto t1 = clock::now();
        perf_dump += elapsed_ms(t0, t1);
        ++perf_dump_n;

        t0 = clock::now();
        try { scan_rows(); } catch (...) {}
        t1 = clock::now();
        perf_scan += elapsed_ms(t0, t1);
        ++perf_scan_n;
    }

    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_names).count() >= 2000)
    {
        last_names = now;
        auto t0 = clock::now();
        try { update_blocked_name_colors(); } catch (...) {}
        auto t1 = clock::now();
        perf_names += elapsed_ms(t0, t1);
        ++perf_names_n;
    }

    // 200ms 只做「补装 hook + 队列落地」；稳态没有 FindAllOf，开销 ≈ 0。
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_profiles).count() >= 200)
    {
        last_profiles = now;
        auto t0 = clock::now();
        try { mbp_card_pump(); } catch (...) {}
        auto t1 = clock::now();
        perf_profiles += elapsed_ms(t0, t1);
        ++perf_profiles_n;
    }

    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - perf_last).count() >= 15000)
    {
        perf_last = now;
        auto avg = [](double total, int count) { return count > 0 ? total / count : 0.0; };
        Output::send<LogLevel::Verbose>(
            STR("[MBPId] perf avg dump={:.3f}ms scanRows={:.3f}ms names={:.3f}ms profiles={:.3f}ms\n"),
            avg(perf_dump, perf_dump_n), avg(perf_scan, perf_scan_n),
            avg(perf_names, perf_names_n), avg(perf_profiles, perf_profiles_n));
        perf_dump = perf_scan = perf_names = perf_profiles = 0.0;
        perf_dump_n = perf_scan_n = perf_names_n = perf_profiles_n = 0;
    }
}

static bool g_game_thread_hook_ready = false;
static bool g_game_thread_hook_warned = false;

static auto try_register_game_thread_hook() -> void
{
    if (g_game_thread_hook_ready) return;

    try
    {
        if (UE4SSRuntime::IsEngineTickAvailable())
        {
            Hook::RegisterEngineTickPostCallback([](UEngine*, float) { run_game_thread_update(); });
            g_game_thread_hook_ready = true;
            Output::send<LogLevel::Verbose>(STR("[MBPId] game-thread hook: EngineTick\n"));
            return;
        }
    }
    catch (...) {}

    try
    {
        Hook::RegisterGameViewportClientTickPostCallback([](UGameViewportClient*, float) { run_game_thread_update(); });
        g_game_thread_hook_ready = true;
        Output::send<LogLevel::Verbose>(STR("[MBPId] game-thread hook: GameViewportClientTick\n"));
        return;
    }
    catch (...) {}

    try
    {
        if (UE4SSRuntime::IsProcessEventAvailable())
        {
            Hook::RegisterProcessEventPostCallback([](UObject*, UFunction*, void*) { run_game_thread_update(); });
            g_game_thread_hook_ready = true;
            Output::send<LogLevel::Verbose>(STR("[MBPId] game-thread hook: ProcessEvent\n"));
            return;
        }
    }
    catch (...) {}

    if (!g_game_thread_hook_warned)
    {
        g_game_thread_hook_warned = true;
        Output::send<LogLevel::Verbose>(STR("[MBPId] no game-thread hook available; will retry\n"));
    }
}

class MBPIdMod : public CppUserModBase
{
public:
    MBPIdMod() : CppUserModBase()
    {
        ModName        = STR("MBPId");
        ModVersion     = STR("0.1");
        ModDescription = STR("Player SteamIDs + server list tag + blocked name color + profile button (live Steam blocklist)");
        ModAuthors     = STR("local");
        try { mbp_card_init(); } catch (...) {}
        Output::send<LogLevel::Verbose>(STR("[MBPId] constructed v0.1 (game-thread updates)\n"));
        Output::send<LogLevel::Verbose>(STR("[MBPId] UI language: {} / {}\n"),
            ui_strings().blocked_tag, ui_strings().profile_button);
    }

    ~MBPIdMod() override = default;

    // 关键：UE4SS 的 on_update 在它自己的事件循环线程，不是游戏线程。
    // 旧实现从那里直接 FindAllOf / ProcessEvent，会和游戏线程销毁控件竞争，
    // 触发 pure virtual 或访问冲突。所有 UObject 工作改到 EngineTick（游戏线程）。
    auto on_unreal_init() -> void override
    {
        try_register_game_thread_hook();
    }

    // 只做「补装游戏线程 hook」；不碰任何 UObject，避免回到旧线程问题。
    auto on_update() -> void override
    {
        if (g_game_thread_hook_ready) return;

        using clock = std::chrono::steady_clock;
        static auto last_try = clock::now() - std::chrono::seconds(5);
        const auto now = clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_try).count() < 1000) return;
        last_try = now;
        try_register_game_thread_hook();
    }
};

#define MBPID_API __declspec(dllexport)
extern "C"
{
    MBPID_API RC::CppUserModBase* start_mod() { return new MBPIdMod(); }
    MBPID_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}




