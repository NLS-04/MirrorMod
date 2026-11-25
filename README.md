# MirrorMod

[![CI](https://github.com/NLS-04/MirrorMod/actions/workflows/ci.yml/badge.svg)](https://github.com/NLS-04/MirrorMod/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/NLS-04/MirrorMod)](https://github.com/NLS-04/MirrorMod/releases/latest)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)

A mirror mode for HITMAN: World of Assassination, built as a [ZHM Mod SDK](https://github.com/OrfeasZ/ZHMModSDK) plugin. Every level is shown horizontally flipped, so that familiar maps feel new, while the UI stays readable and the controls stay intuitive.

| MirrorMod | ZHM Mod SDK | Game version |
|---|---|---|
| 1.0.0 | v4.2.1 | 3.290.1.0 |

Use it offline or with [Peacock](https://thepeacockproject.org/).

**Contents:** [For players](#for-players) · [For developers](#for-developers) · [License](#license)

---

# For players

## What it does

The finished 3D image is flipped right before the game draws its UI. Everything that would no longer match is then corrected:

| Part | Behaviour |
|---|---|
| World | Mirrored, including 47, who becomes left-handed |
| UI, menus, subtitles | Stay readable |
| Prompts, world markers, off-screen indicators | Placed at the mirrored position |
| Detection arrows and red hit markers around 47 | Point the mirrored way |
| Minimap | Mirrored as a whole, its icons keep their orientation |
| Map in the pause menu | Mirrored horizontally or vertically, labels and icons stay readable, panning follows the mouse |
| Picture-in-picture | Stays in its corner |
| Mouse, sticks, strafing | Reversed horizontally, menus are left alone |
| FreeCam of the SDK | Turning, panning and sideways flight reversed horizontally |
| Audio | Left and right swapped, including surround pairs |
| Third person camera | Moves to the other shoulder at once, without the blend |
| Loading screens | Not mirrored |

## Installation

1. Install the [ZHM Mod SDK](https://github.com/OrfeasZ/ZHMModSDK/releases) in the version from the table above.
2. Download `MirrorMod-<version>.zip` from the [latest release](https://github.com/NLS-04/MirrorMod/releases/latest) and unpack it.
3. Copy `MirrorMod.dll` to `Retail/mods` in the game folder.
4. Start the game, open the SDK overlay with `~` (`^` on a QWERTZ keyboard), and enable `MirrorMod` in the mod list.
5. Turn off Frame Generation in the game's graphics options.

To update, replace `MirrorMod.dll`. Your settings in `mirrormod.ini` are kept. The installed version is shown in the `About` section of the mod's window and in the file properties of the DLL.

## Usage

Press **F7** to switch the mirror on and off, also in the pause menu.

The `MIRROR MODE` button in the SDK overlay opens the mod's window. There you can

- change the toggle key: click it, then press the new one,
- change all options in the `Config` section,
- find counters and debug tools in the `Developer` section.

## Configuration

All settings are stored in `Retail/mods/mirrormod.ini` and written there on the first start. Changes made in the file apply at the next start of the game. If the mod crashes the game on start, set `enabled = false`.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `general` | `enabled` | `false` | Whether the mirror is on |
| `general` | `toggle_key` | `F7` | The key switching the mirror, see below |
| `general` | `mode` | `0` | `0` flips before the UI, `1` flips the whole frame including the UI |
| `hud` | `mirror_positions` | `true` | Corrects the HUD elements anchored in the world |
| `hud` | `mirror_minimap` | `true` | Mirrors the minimap, needs `mirror_positions` |
| `hud` | `mirror_full_map` | `true` | Mirrors the map in the pause menu, needs `mirror_positions` |
| `hud` | `full_map_axis` | `0` | `0` mirrors that map horizontally, `1` vertically |
| `input` | `invert_look` | `true` | Reverses mouse and right stick horizontally |
| `input` | `invert_move` | `true` | Reverses strafing and left stick horizontally |
| `input` | `invert_freecam` | `true` | Reverses turning, panning and sideways flight of the SDK's FreeCam mod |
| `audio` | `swap_channels` | `true` | Swaps left and right audio |
| `camera` | `swap_shoulder` | `true` | Changes the shoulder when the mod is toggled. A level always starts on the other shoulder while the mod is on |

Valid values for `toggle_key`: `A`-`Z`, `0`-`9`, `F1`-`F24`, `Numpad0`-`Numpad9`, `NumpadAdd`, `NumpadSubtract`, `NumpadMultiply`, `NumpadDivide`, `NumpadDecimal`, `Space`, `Enter`, `Tab`, `Backspace`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Up`, `Down`, `Left`, `Right`, `Pause`, `ScrollLock`.

## Limitations

- **Frame Generation is not supported.** It interpolates with motion vectors that belong to the unmirrored image. Turn it off.
- Signs and other text inside the world are mirrored. So is text that is part of a map's background image.
- Some icons on the vertically mirrored pause menu map, such as the briefcase, are upside down.
- The audio swap applies to all sound of the game, including menus.
- Menus with a 3D background are mirrored.
- The mod works only with the game version in the table above. After a game update, the affected parts switch themselves off instead of crashing, until the mod is updated.
- Switch the mod off before unloading it in the SDK menu, since GPU work may still be in flight.

## Troubleshooting

`Retail/mods/MirrorMod.log` is rewritten on every game start and flushed after every line, so it survives crashes. It starts with the mod and game version, and every line has the same columns: time, level (`DEBUG`, `INFO`, `WARN`, `ERROR`), the part of the mod, and the message.

```
14:03:27.512 WARN  render   Flip rehearsal failed, falling back to flipping whole frames: ...
```

Warnings and errors also appear in the SDK console. A problem that repeats is logged only a limited number of times.

| Log message | Meaning |
|---|---|
| `... does not match this game version` | The game was updated, the affected part stays off |
| `Flip rehearsal failed` | The render target cannot be flipped in the engine's command list. Whole frames are flipped instead, including the UI |
| `A frame with N UI draws missed the flip` | That frame's UI got mirrored, usually visible as a flicker |
| `A fault occurred in the engine's UI objects` | Map mirroring is off until the next start |
| `The load screen event did not arrive` | Mirroring started on its one second timeout |

When you report a problem, attach `MirrorMod.log` and say which version of the mod, the SDK and the game you use.

---

# For developers

## Requirements

- Windows with the Visual Studio 2022 Build Tools (or newer) and the C++ workload
- CMake 3.15 or newer and Git
- vcpkg, which is not part of the repository, see below

The ZHM Mod SDK is downloaded by CMake in the version set by `ZHMMODSDK_VER` in [CMakeLists.txt](CMakeLists.txt). Set `ZHMMODSDK_DIR` to use a local copy instead.

## Building

Clone vcpkg into the repository and pin it to the commit the CI uses. The vcpkg version pinned by the official SDK sample no longer works, because its msys2 package is gone.

```bash
git -c core.longpaths=true clone https://github.com/Microsoft/vcpkg.git vcpkg
```

```bash
git -C vcpkg checkout ec48c11eae9448c8c6e7739d93e06fa2433aa95f
```

```bash
vcpkg/bootstrap-vcpkg.bat -disableMetrics
```

Then configure and build:

```bash
cmake --preset vs-release
```

```bash
cmake --build _build/vs-release --config RelWithDebInfo --target MirrorMod
```

The result is `_build/vs-release/RelWithDebInfo/MirrorMod.dll`. In Visual Studio the `x64-Release` and `x64-Debug` presets work as well. Set `GAME_INSTALL_PATH` to have the install step copy the DLL into the game folder.

| Preset | Generator | Use |
|---|---|---|
| `vs-release` | Visual Studio 17 2022 | Command line build, includes the tests |
| `x64-Release` | Ninja | Visual Studio, RelWithDebInfo |
| `x64-Debug` | Ninja | Visual Studio, Debug |

## Tests

The `vs-release` preset also builds standalone tests (`MIRRORMOD_BUILD_TESTS=ON`). They need neither the SDK nor the game.

```bash
cmake --build _build/vs-release --config RelWithDebInfo
```

```bash
ctest --test-dir _build/vs-release -C RelWithDebInfo --output-on-failure
```

| Test | Covers | Needs |
|---|---|---|
| `CoreTest` | Log, hotkey, hooking helpers | |
| `HudMirrorTest` | Arithmetic of the HUD and map corrections, shoulder swap | |
| `UiTraceTest` | The UI trace | |
| `MirrorFlipTest` | The GPU flip pass and the command list hooks | A D3D12 GPU |
| `AudioSwapTest` | The WASAPI channel swap | A playback device |

Whether the corrections hit the right places in the game can only be checked by playing.

## Project layout

| Path | Purpose |
|---|---|
| `src/MirrorMod.*`, `MirrorModUi.cpp` | The plugin: lifecycle, game hooks, input, and its window |
| `src/Settings.*` | The options and their ini file |
| `src/core/` | Log, toggle key, and the hooking helpers |
| `src/render/` | `FrameMirror` decides where a frame is flipped, `MirrorFlip` is the GPU pass, `CommandListHooks` finds the engine's command list |
| `src/hud/` | `HudMirror` corrects HUD elements, `MapMirror` the two maps |
| `src/audio/` | The WASAPI channel swap |
| `src/game/` | Scene state, the load screen event and the shoulder camera |
| `src/diagnostics/` | The UI trace |
| `tests/` | The standalone tests |
| `cmake/` | SDK download and the template of the DLL's version resource |
| `scripts/Set-Version.ps1` | Raises the version, see [Releasing](#releasing) |
| `.github/workflows/ci.yml` | Build, test, packaging and release |

Everything except the plugin itself (the `MirrorModCore` library) is free of SDK dependencies, which is what makes it testable without the game. [docs/DESIGN.md](docs/DESIGN.md) explains the main design decisions.

## Debugging tools

The `Developer` section of the mod's window offers more, none of it stored between sessions:

| Tool | Result |
|---|---|
| Verbose log | Adds the `DEBUG` lines, such as loading stages and render target choices |
| Trace next 60 frames | One `frame` line per frame: UI passes, UI draws and where the frame was flipped |
| Log used input actions | Names every input action once when it is first used |
| Record UI trace | `Retail/mods/MirrorMod-uitrace.log`, what the engine tells the UI, grouped by kind of call |

## Supporting a new game version

All addresses are relative to the game's executable and listed next to their use. Each hooked or called function comes with its first bytes, which are compared before anything is hooked, so a game update switches the affected part off instead of crashing. To port the mod, find the same functions again and update their addresses and bytes. `Record UI trace` shows which game code writes which UI values. Update the compatibility table at the top of this README afterwards.

## Versioning

MirrorMod follows [Semantic Versioning](https://semver.org/):

- **Major** for changes that break existing setups, such as renamed or removed settings,
- **Minor** for new features and support for a new game or SDK version,
- **Patch** for fixes.

The version in `project(MirrorMod VERSION x.y.z)` in [CMakeLists.txt](CMakeLists.txt) is the only source of truth. It is compiled into the mod (shown in its window and log) and into the DLL's file properties. `vcpkg.json` has to carry the same version, and [CHANGELOG.md](CHANGELOG.md) a section for it. The CI checks both.

| Build | Version | Where |
|---|---|---|
| Release | `1.0.0` | GitHub release with tag `v1.0.0` |
| CI build of any other commit or pull request | `1.0.0-dev.<run>+<commit>` | Artifact of the workflow run, kept 30 days |
| Local build | `1.0.0` | Set `MIRRORMOD_VERSION_SUFFIX` to mark it |

## CI

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs on every push to `main`, every pull request, every `v*` tag and by hand:

1. **Determine version** reads the version, checks `vcpkg.json` and `CHANGELOG.md`, and decides whether this run is a release.
2. **Build and test** builds the mod and the tests on Windows with the pinned vcpkg, runs the tests, checks the version in the DLL, and packages it:
   - `MirrorMod-<version>.zip` with `MirrorMod.dll`, `README.md`, `CHANGELOG.md`, `LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES` and `SOURCE.txt`, which names the source code and commit as the GPL requires,
   - `MirrorMod-<version>-symbols.zip` with `MirrorMod.pdb` for crash analysis,
   - `SHA256SUMS.txt`.

   `MirrorFlipTest` and `AudioSwapTest` run as well but cannot fail the build, since the runners have no GPU or audio device for sure. Run them locally before a release.
3. **Publish release** creates the tag and the GitHub release with these files, using the version's section of `CHANGELOG.md` as notes.

## Releasing

1. Raise the version:

   ```powershell
   ./scripts/Set-Version.ps1 1.1.0
   ```

   This updates `CMakeLists.txt` and `vcpkg.json` and adds a section to `CHANGELOG.md`.
2. Describe the changes in that section of `CHANGELOG.md`.
3. Commit and push to `main`, or merge a pull request.

The CI sees that `v1.1.0` does not exist yet, builds the release and publishes it. A push to `main` without a new version only produces a development build. Pushing a tag `v1.1.0` by hand also releases, provided it matches the version in `CMakeLists.txt`.

## Contributing

Pull requests are welcome. Please keep the code free of SDK dependencies outside the plugin itself where possible, add tests to `tests/` for anything that can be tested without the game, and add a line to the topmost section of `CHANGELOG.md`.

---

# Credits

Built on the [ZHM Mod SDK](https://github.com/OrfeasZ/ZHMModSDK) by OrfeasZ and contributors. Uses MinHook, Dear ImGui, ImPlot, {fmt} and DirectX-Headers, see [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).

# License

MirrorMod by NLS-04 is free software under the [GNU General Public License v3.0 or later](LICENSE), the license of the ZHM Mod SDK it is built on. One additional term under section 7(b) applies: whoever shares MirrorMod or a work based on it keeps the attribution "MirrorMod by NLS-04", see [NOTICE](NOTICE).

A binary release has to include `LICENSE`, `NOTICE` and `THIRD_PARTY_NOTICES` next to `MirrorMod.dll` and say where its source code is. The release packages of the CI do both.
