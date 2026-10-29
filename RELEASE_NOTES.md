### What's new in 1.0.0-beta7

- **Fixed: no more freezes during setup.** Finding the lift data (first lift with the game key) and the steering data (first turn) used to freeze the game for several seconds. Both searches now run in the background – the game keeps running.
- **Fixed:** with lifted steered axles, the steering lock setup kept starting over (another freeze each time). Each steered axle is now set up on its own, the first time it actually steers. Until then it shows *"setup: steer once"* in the overlay.
- If one axle's steering data stops working, only that axle is set up again; the other locks keep working.

New in 1.0.0-beta6: **steering lock** – hold steered trailer axles (and the truck's steered pusher / tag axles) straight, always (*Lock*) or only while reversing (*Reverse*), with smooth centering and release.

Full history: [CHANGELOG.md](https://github.com/kuhmori/ets2-lift-axle-extension/blob/main/CHANGELOG.md)

---

> **Single-player only.** The plugin switches itself off in TruckersMP and Convoy.
> Tested with ETS2 **1.61.1.9**, DirectX 11 renderer, Windows 11.

### Install / update

1. Download **`LiftAxleExtension.dll`** below.
2. Copy it to `<ETS2 folder>\bin\win_x64\plugins\` (create `plugins` if it doesn't exist). When updating, replace the old DLL – only **one** copy of the plugin may be in that folder.
3. Start the game and click **OK** on the SDK plugin message.

### First use

1. Couple a trailer with lift axles and **lift or lower them once with the game's own lift key** – the plugin sets itself up in the background while they move (once per trailer).
2. Press **`Alt+O`** to open the overlay and click an axle.
3. For the steering lock, drive through a turn once, then tick *Lock* or *Reverse* next to a steered axle.

Default keys: trailer 1 axles `Alt+1`…`Alt+8`, truck axles `Ctrl+Alt+1`…`Ctrl+Alt+8`, release all `Alt+0`. Change them (also to wheel / gamepad buttons) in the overlay.

### Known limitations

- After using the plugin, the game's own lift key may only cause a short bounce – click **Release to game** (`Alt+0`) first, then it works normally.
- A lift with the game's key is needed after coupling a trailer and again after loading cargo (the plugin tells you when).
- Overlay needs the DirectX 11 renderer (default). ETS2 only, no ATS yet.

Please report problems on the [Issues](https://github.com/kuhmori/ets2-lift-axle-extension/issues) page with your game version and the `[axlelift]` lines from `Documents\Euro Truck Simulator 2\game.log.txt`.
