### What's new in 1.0.0-beta5

- **Fixed:** steerable lift axles stayed locked straight after the plugin lowered them until the game's lift key was pressed. Steering now works normally. Thanks to **MVD** for reporting it.
- **Fixed:** automatic mode "when reversing" now puts the axles back to their previous state when you drive forward again. Thanks to **Quichenic** for reporting it.
- **Removed:** axle load display, overload protection and the load-based automatic rules. The loads from the game's physics weren't reliable enough. Automatic mode is now the reversing rule only; old settings are ignored.

Full history: [CHANGELOG.md](https://github.com/kuhmori/ets2-lift-axle-extension/blob/main/CHANGELOG.md)

---

> **Single-player only.** The plugin switches itself off in TruckersMP and Convoy.
> Tested with ETS2 **1.61.1.9**, DirectX 11 renderer, Windows 11.

### Install / update

1. Download **`LiftAxleExtension.dll`** below.
2. Copy it to `<ETS2 folder>\bin\win_x64\plugins\` (create `plugins` if it doesn't exist). When updating, replace the old DLL – only **one** copy of the plugin may be in that folder.
3. Start the game and click **OK** on the SDK plugin message.

### First use

1. Couple a trailer with lift axles and **lift or lower them once with the game's own lift key** – the plugin sets itself up while they move (the game pauses ~3 s, once per trailer).
2. Press **`Alt+O`** to open the overlay and click an axle.

Default keys: trailer 1 axles `Alt+1`…`Alt+8`, truck axles `Ctrl+Alt+1`…`Ctrl+Alt+8`, release all `Alt+0`. Change them (also to wheel / gamepad buttons) in the overlay.

### Known limitations

- After using the plugin, the game's own lift key may only cause a short bounce – click **Release to game** (`Alt+0`) first, then it works normally.
- A lift with the game's key is needed after coupling a trailer and again after loading cargo (the plugin tells you when).
- Overlay needs the DirectX 11 renderer (default). ETS2 only, no ATS yet.

Please report problems on the [Issues](https://github.com/kuhmori/ets2-lift-axle-extension/issues) page with your game version and the `[axlelift]` lines from `Documents\Euro Truck Simulator 2\game.log.txt`.
