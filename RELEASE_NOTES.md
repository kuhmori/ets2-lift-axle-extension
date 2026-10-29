### What's new in 1.0.0-beta6

- **New: steering lock.** Steered trailer axles (and the truck's steered pusher / tag axles) can now be held straight. In the overlay, every steered axle has two checkboxes:
  - **Lock** – straight all the time
  - **Reverse** – straight only while reversing, steers normally again when you drive forward
- Locking and releasing are **smooth**: the wheels ease to straight and back to the game's steering. Adjust the speed with *Settings → Steering centering time*.
- **Setup:** drive through a turn or two once after coupling, so the steered axles move. The overlay shows *"steering lock ready"* when it's done.
- Lifted steered axles are left alone; the game already holds them straight.

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
3. For the steering lock, drive through a turn once, then tick *Lock* or *Reverse* next to a steered axle.

Default keys: trailer 1 axles `Alt+1`…`Alt+8`, truck axles `Ctrl+Alt+1`…`Ctrl+Alt+8`, release all `Alt+0`. Change them (also to wheel / gamepad buttons) in the overlay.

### Known limitations

- After using the plugin, the game's own lift key may only cause a short bounce – click **Release to game** (`Alt+0`) first, then it works normally.
- A lift with the game's key is needed after coupling a trailer and again after loading cargo (the plugin tells you when).
- Overlay needs the DirectX 11 renderer (default). ETS2 only, no ATS yet.

Please report problems on the [Issues](https://github.com/kuhmori/ets2-lift-axle-extension/issues) page with your game version and the `[axlelift]` lines from `Documents\Euro Truck Simulator 2\game.log.txt`.
