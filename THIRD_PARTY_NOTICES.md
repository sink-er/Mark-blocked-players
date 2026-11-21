# Third-party notices

This repository contains only original MBPId code, documentation and example
configuration. It does **not** vendor or redistribute any of the following:

## RE-UE4SS

- Project: https://github.com/UE4SS-RE/RE-UE4SS
- License: MIT
- Usage: build-time headers, link-time `UE4SS` target, and runtime
  `UE4SS.dll` that the user installs separately.
- MIT is compatible with this project's GPL-2.0-or-later licensing. No UE4SS
  source or binary is included in this repository.

```text
MIT License

Copyright (c) 2022 Narknon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Unreal Engine

- MBPId links against UE4SS's generated Unreal API headers, which are supplied
  by the user's RE-UE4SS checkout.
- No Unreal Engine source code or headers are included in this repository.
- Unreal Engine is subject to Epic Games' own license terms.

## Steamworks / steam_api64.dll

- MBPId calls the `ISteamFriends` flat API exported by the `steam_api64.dll`
  that ships with the game.
- The DLL and Steamworks SDK are **not** included in this repository.
- Valve's Steamworks SDK terms apply to the SDK; MBPId only performs runtime
  dynamic loading of the game-provided DLL.

## Deep Rock Galactic

- No game assets, data tables, localization files or binaries are included.
- Deep Rock Galactic is property of Ghost Ship Games. Users must own the game.

## Microsoft Visual C++ / Windows SDK

- Build-time dependencies only; not redistributed by this repository.
