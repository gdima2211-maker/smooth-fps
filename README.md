# Smooth FPS (Geode)

FPS slider (24-1440) plus frame extrapolation for Geometry Dash.
Physics, hitboxes and input are not changed, the mod is visual only.

## Features

- **FPS slider** from 24 to 1440.
- **Extrapolation mode**: draws the player and camera where they are predicted to be *right now*, so there is no added delay.
- **Interpolation mode**: blends between the last two physics ticks. Up to one tick behind (about 4 ms), but never overshoots.
- Smoothing is applied only while the frame is drawn. Collisions, triggers and progress always use the real positions.
- Works with time warp, dual mode and teleport portals. Large jumps are never smoothed.

## Building with GitHub

1. Create a new GitHub repository and upload the contents of this folder.
2. In `mod.json`, change `id` (`yourname.smooth-fps`) and `developer`.
3. Open the **Actions** tab. The build starts after a push, or run it manually (Run workflow).
4. When it finishes, download the **Build Output** artifact. It contains the `.geode` file.
5. Put the `.geode` file into `geode/mods` in your game folder (or drag it onto the GD window).

## If the build fails

- Geode version error: change `"geode"` in `mod.json` to the current Geode version.
- Errors about `setAnimationInterval`, `m_player1`, `m_objectLayer`, `m_gameState` or `m_isDead`: send the log, the field names may differ in your bindings version.

## Notes

- Turn off VSync in GD's video settings, otherwise FPS is capped by your monitor.
- Extrapolation does not predict through a bounce (jump / landing), so the icon never sinks into the floor.
- Not tested in-game yet. Try it in a normal level first, not on a record attempt.
