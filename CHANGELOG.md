# Changelog

## 1.0.0-beta4

### Fixed
- Lift axles stopped responding to keys and controller buttons after loading cargo. The game rebuilds the trailer when cargo is loaded and the plugin kept using the old data. The plugin now notices this and asks you to lift or lower the axles once with the game key to set itself up again.

### Changed
- After the game re-sends a vehicle (for example when loading cargo), the plugin checks with a tiny, invisible test movement whether its data still works.
- Every lift or lower done by the plugin is checked against the real wheel height reported by the game. If the wheel doesn't follow, the plugin sets itself up again instead of silently doing nothing.

## 1.0.0-beta2

First public beta.
