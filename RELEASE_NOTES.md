### What's new in 1.0.0-beta4

- **Fixed:** lift axles stopped responding to keys and controller buttons after loading cargo. The game rebuilds the trailer when cargo is loaded and the plugin kept using the old data. It now notices this and asks you to lift or lower the axles once with the game key to set itself up again.
- After the game re-sends a vehicle (for example when loading cargo), the plugin checks with a tiny, invisible test movement whether its data still works.
- Every lift or lower done by the plugin is checked against the real wheel height. If the wheel doesn't follow, the plugin sets itself up again instead of silently doing nothing.

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
