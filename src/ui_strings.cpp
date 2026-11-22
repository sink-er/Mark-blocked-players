// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 MBPId contributors
//
// 游戏语言 / 界面文案：读 GameUserSettings.ini + 17 种语言的文案表。
#include "ui_strings.hpp"

#include <fstream>

namespace mbpid
{

namespace
{

auto read_game_language() -> std::string
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
            // 跳过 UTF-8 BOM
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

auto normalize_language(std::string code) -> std::string
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

} // namespace

auto ui_language() -> const std::string&
{
    static const std::string language = normalize_language(read_game_language());
    return language;
}

auto ui_strings() -> const UiStrings&
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
        if (lang == "ko") return UiStrings{L"[\uCC28\uB2E8\uD55C \uD504\uB808\uC774\uC5B4]", L"\uD504\uB85C\uD544"};
        if (lang == "tr") return UiStrings{L"[Engellenen Oyuncu]", L"Profil"};
        if (lang == "hu") return UiStrings{L"[Letiltott j\u00E1t\u00E9kos]", L"Profil"};
        if (lang == "ro") return UiStrings{L"[Juc\u0103tor blocat]", L"Profil"};
        return UiStrings{L"[Blocked Player]", L"Profile"};
    }();
    return strings;
}

} // namespace mbpid
