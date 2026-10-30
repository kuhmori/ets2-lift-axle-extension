# Changelog

## 1.0.0-beta7

### Fixed
- The game froze for several seconds while the plugin set itself up (first lift with the game key, first turn for the steering lock). The memory searches now run in the background and the game keeps running. They are also faster.
- With lifted steered axles, the steering lock setup started over again and again (another freeze each time), because lifted axles never steer and were never found. Each steered axle is now set up on its own, the first time it actually steers; the others are ready right away.
- After the game rebuilt the truck or trailer (taking a job, loading cargo), individual lifting could stop working for good and the overlay wrongly said the game version may not be supported. A search that misses the lift data is now thrown away and done again on the next lift with the game key, and the search also works when the axles finish lifting before it is done. The "not supported" note only appears after several failed lifts in a row.

### Changed
- An axle whose steering data is not found yet shows *setup: steer once* in the overlay.
- If one axle's steering data stops working, only that axle is set up again; the locks of the other axles keep working.

## 1.0.0-beta6

### Added
- Steering lock for steered axles. Every steered trailer axle, and the truck's steered pusher / tag axles, get two checkboxes in the overlay: *Lock* holds the axle straight all the time, *Reverse* holds it straight only while reversing and lets it steer again when you drive forward.
- Locking centers the wheels smoothly, and releasing eases them back to the game's own steering angle. The time is set with *Steering centering time* in the settings (default 1 s).
- The plugin finds the steering data by itself the first time the steered axles move (drive through a turn). If the game keeps steering an axle despite the lock, the overlay tells you.
- Diagnostics file now includes the steering data.

## 1.0.0-beta5

### Fixed
- Steerable lift axles stayed locked straight after the plugin lowered them, until the game's own lift key was pressed. The plugin now uses the game's own lift action whenever that gives the same result, so steering works normally. Thanks to MVD for the report.
- Automatic mode "when reversing" did not put the axles back when driving forward again. The axles now return to their previous state once you drive forward. Thanks to Quichenic for the report.

### Removed
- Axle load display, overload protection and the load-based automatic rules. The loads read from the game's physics were not reliable enough. Automatic mode is now the reversing rule only. Old settings in `axlelift.ini` are ignored.

## 1.0.0-beta4

### Fixed
- Lift axles stopped responding to keys and controller buttons after loading cargo. The game rebuilds the trailer when cargo is loaded and the plugin kept using the old data. The plugin now notices this and asks you to lift or lower the axles once with the game key to set itself up again.

### Changed
- After the game re-sends a vehicle (for example when loading cargo), the plugin checks with a tiny, invisible test movement whether its data still works.
- Every lift or lower done by the plugin is checked against the real wheel height reported by the game. If the wheel doesn't follow, the plugin sets itself up again instead of silently doing nothing.

## 1.0.0-beta2

First public beta.
