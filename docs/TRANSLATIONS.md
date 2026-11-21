# UI translations

MBPId localizes two strings:

- the server-list tag appended after the region: `[Blocked Player]`
- the profile button in the ESC player list: `Profile`

The language is read from the game's own configuration:

```
<game>/FSD/Saved/Config/WindowsNoEditor/GameUserSettings.ini
[Internationalization]
Language=...
```

Language changes take effect after restarting the game. Unknown or missing
languages fall back to English.

| Game language | Code | Server-list tag | Button |
|---|---|---|---|
| English | `en` | `[Blocked Player]` | `Profile` |
| French | `fr` | `[Joueur bloqué]` | `Profil` |
| German | `de` | `[Blockierter Spieler]` | `Profil` |
| Spanish (Spain) | `es` | `[Jugador bloqueado]` | `Perfil` |
| Spanish (Latin America) | `es-419` | `[Jugador bloqueado]` | `Perfil` |
| Polish | `pl` | `[Zablokowany gracz]` | `Profil` |
| Portuguese (Brazil) | `pt-br` | `[Jogador bloqueado]` | `Perfil` |
| Russian | `ru` | `[Заблокированный игрок]` | `Профиль` |
| Simplified Chinese | `zh-hans` | `[已屏蔽玩家]` | `主页` |
| Traditional Chinese | `zh-hant` | `[已封鎖玩家]` | `個人檔案` |
| Ukrainian | `uk` | `[Заблокований гравець]` | `Профіль` |
| Italian | `it` | `[Giocatore bloccato]` | `Profilo` |
| Japanese | `ja` | `[ブロックしたプレイヤー]` | `プロフィール` |
| Korean | `ko` | `[차단한 플레이어]` | `프로필` |
| Turkish | `tr` | `[Engellenen Oyuncu]` | `Profil` |
| Hungarian | `hu` | `[Letiltott játékos]` | `Profil` |
| Romanian | `ro` | `[Jucător blocat]` | `Profil` |

## Adding or correcting a translation

1. Add a branch in `ui_strings()` in `src/dllmain.cpp`.
2. Add the row to this table.
3. Use escaped wide-string literals (`L"\uXXXX"`) for non-ASCII characters,
   matching the existing style. This keeps the source independent of the
   compiler's source encoding.
4. Keep the brackets in the tag string itself; the mod inserts the string as-is.

The button width defaults to `50`. For long labels, set `width=0` in
`data/profile_button.txt` to let the button size itself from the text.
