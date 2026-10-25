First public beta of **Lift Axle Extension** for Euro Truck Simulator 2 – lift every truck and trailer lift axle on its own.

> **Single-player only.** The plugin switches itself off in TruckersMP and Convoy.
> Tested with ETS2 **1.61.1.9**, DirectX 11 renderer, Windows 11.

### Install

1. Download **`LiftAxleExtension.dll`** below.
2. Copy it to `<ETS2 folder>\bin\win_x64\plugins\` (create `plugins` if it doesn't exist).
3. Start the game and click **OK** on the SDK plugin message.

### First use

1. Couple a trailer with lift axles and **lift or lower them once with the game's own lift key** – the plugin sets itself up while they move (the game pauses ~3 s, once per trailer).
2. Press **`Alt+O`** to open the overlay and click an axle.

Default keys: trailer 1 axles `Alt+1`…`Alt+8`, truck axles `Ctrl+Alt+1`…`Ctrl+Alt+8`, release all `Alt+0`. Change them (also to wheel / gamepad buttons) in the overlay.

### Features

- Individual control of every lift axle – truck plus up to 10 trailers, B-doubles told apart automatically
- In-game overlay (Dear ImGui) with its own mouse cursor – the camera doesn't move while it's open
- Smooth lifting, adjustable time
- Keyboard and controller keybinds per axle
- Optional: axle loads, overload protection (EU 9 t per axle), automatic mode (lift below 7.0 t, lower above 8.5 t)
- "Release to game" hands the axles back to the game's own control
- No hard-coded memory addresses – recognises the game's data at run time and stays inactive if it can't

### Known limitations

- After using the plugin, the game's own lift key may only cause a short bounce – click **Release to game** (`Alt+0`) first, then it works normally.
- A first lift with the game's key is needed after coupling a trailer.
- Overlay needs the DirectX 11 renderer (default). ETS2 only, no ATS yet.

Please report problems on the [Issues](https://github.com/kuhmori/ets2-lift-axle-extension/issues) page with your game version and the `[axlelift]` lines from `Documents\Euro Truck Simulator 2\game.log.txt`.

**Files:** `LiftAxleExtension.dll` (the plugin) · `LiftAxleExtension.zip` (plugin + readme + licences)
