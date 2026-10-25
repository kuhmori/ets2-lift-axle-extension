# Lift Axle Extension for Euro Truck Simulator 2

Lift every lift axle of your truck and trailers **on its own** – not just all of them at once.
Control them from an in-game overlay, with keyboard shortcuts or with buttons on your wheel / gamepad.
Optional extras: live axle loads, overload protection and an automatic mode based on EU axle-load rules.

> **Single-player only.** The plugin switches itself off in TruckersMP and in Convoy sessions.

> **Status:** beta. Tested with ETS2 **1.61.1.9** on Windows 11, DirectX 11 renderer.

---

## Features

- **Individual lift axles** – every liftable axle of the truck and of up to 10 trailers (doubles, B-doubles, …), up to 8 per vehicle
- **In-game overlay** (`Alt+O`) drawn inside the game, like the developer console. While it is open the mouse moves the overlay cursor instead of the camera
- **Smooth movement** – axles move like the game's own animation (adjustable time)
- **Keybinds** – a keyboard shortcut and a wheel / gamepad button for every axle and for "release all", set in the overlay
- **Axle loads** (optional) – load per axle in tonnes, measured by the game's physics
- **Overload protection** (optional) – an axle can't be lifted if the remaining axles would carry more than the limit, and comes down if the limit is exceeded
- **Automatic mode** (optional, per axle) – lifts and lowers by load like a real lift-axle control; optional rule for reversing
- **Release to game** – axles glide back to the game's own state and the game takes over again
- **Identical trailers** (e.g. B-doubles) are told apart automatically
- **No fixed memory addresses** – the plugin recognises the game's lift data by its behaviour every time. If a game update changes it, the plugin says "not recognised" and does nothing instead of writing to the wrong place

## Installation

1. Download `LiftAxleExtension.dll` from the [Releases](../../releases) page.
2. Copy it to `<ETS2 folder>\bin\win_x64\plugins\`
   (create the `plugins` folder if it doesn't exist).
   In Steam: right-click ETS2 → *Manage* → *Browse local files*.
3. Start the game. ETS2 shows a message that SDK plugins are active – click **OK**.

**Uninstall:** delete `LiftAxleExtension.dll` and `axlelift.ini` from the `plugins` folder.

**Updating:** replace the DLL. Make sure only **one** copy of the plugin is in the `plugins` folder.

## First use

1. Couple a trailer (or drive a truck) with lift axles.
2. **Lift or lower the axles once with the game's own lift key.**
   While they move, the plugin finds the lift data in the game's memory. The game pauses for about **3 seconds** once – this happens once per trailer after coupling.
3. Press **`Alt+O`** – the overlay opens. Click an axle to lift or lower it.

## Controls

| Action | Default |
|---|---|
| Open / close overlay | `Alt+O` |
| Trailer 1, lift axle 1–8 | `Alt+1` … `Alt+8` |
| Truck, lift axle 1–8 | `Ctrl+Alt+1` … `Ctrl+Alt+8` |
| Release all axles to the game | `Alt+0` |

All of these can be changed in the overlay: click the key field, then press the new key or controller button.
`Esc` cancels, `Backspace` or a right click removes a binding. Trailers 2+ have no default keys.

## Settings

Everything is set in the overlay (*Settings*) and saved in `axlelift.ini` next to the DLL.

| Setting | Default | Notes |
|---|---|---|
| Lift / lower time | 2.0 s | |
| Overlay mouse speed | 1.0 | |
| Show axle loads | off | loads appear after the first lift / lower |
| Overload protection | off | max **9.0 t** per axle (EU: 9 t per axle in a tandem) |
| Automatic mode: lift below | 7.0 t | per axle on the ground |
| Automatic mode: lower above | 8.5 t | real controls lower *before* the legal limit |
| Automatic mode: when reversing | no change | or always lower / always lift |

Automatic mode is switched on per axle with the *Auto* checkbox in the overlay.

## Known limitations

- **Game's own lift key after using the plugin:** the game remembers its own lift state and doesn't know what the plugin changed. Pressing the game's key may then only cause a short bounce. Click **Release to game** (or `Alt+0`) first – after that the game's key works normally again.
- **First lift needed** after coupling a trailer (see *First use*).
- **DirectX 11 only** for the overlay (the game's default renderer). Keys work regardless.
- **ETS2 only** for now – American Truck Simulator is not supported yet.
- Other game versions than the one listed above are untested. The overlay shows a note if your version is untested.

## Troubleshooting

- Open `Documents\Euro Truck Simulator 2\game.log.txt` and search for `[axlelift]` – the plugin logs what it does.
- *"Lift data not recognised"* → your game version may not be supported yet. Please open an issue with your game version and the `[axlelift]` lines from the log.
- For bug reports, *Settings → Show debug tools → Write diagnostics file* creates `axlelift_diagnostics.txt` next to the DLL (the game pauses for ~10–20 s).

## Building from source

Requirements: Visual Studio 2022 or 2026 with the **Desktop development with C++** workload and a Windows SDK.

1. Open `Lift Axle Extension.slnx`.
2. Select **Release** / **x64** and build.
   (Visual Studio 2022 may ask to retarget the project to its toolset – accept.)
3. The DLL is in `x64\Release\Lift Axle Extension.dll`.

Everything needed is in this repository (SCS SDK headers, Dear ImGui, MinHook).

### How it works

- The game's telemetry SDK reports each wheel's lift state and the vehicle layout.
- When the axles move with the game's key, the plugin searches the game's memory for values that follow exactly the same movement and recognises the per-axle lift list that drives the physics. Nothing is hard-coded.
- The plugin then writes its own lift positions into that list. Axle loads are found next to it by watching how the load moves when an axle lifts.
- The overlay is drawn with Dear ImGui inside the game's DirectX 11 frame; mouse input is taken from the game's DirectInput mouse while the overlay is open.

## Credits & licences

- This plugin: [MIT](LICENSE)
- [Dear ImGui](https://github.com/ocornut/imgui) – MIT
- [MinHook](https://github.com/TsudaKageyu/minhook) – BSD 2-Clause
- SCS Software telemetry SDK headers – MIT-style licence by SCS Software

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

*This is an unofficial fan project and is not affiliated with or endorsed by SCS Software. Euro Truck Simulator 2 is a trademark of SCS Software. The plugin modifies the game's memory at run time – use it at your own risk and only in single-player.*
