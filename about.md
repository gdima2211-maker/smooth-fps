# Smooth FPS

Set any frame rate from **24 to 1440** and keep motion smooth with **frame extrapolation**.

- Extrapolation predicts the player and camera position for the current frame (no added delay).
- Interpolation blends between physics ticks instead.
- Visual only: physics, hitboxes and input are never touched.

Turn off VSync in GD's video settings to go above your monitor's refresh rate.
