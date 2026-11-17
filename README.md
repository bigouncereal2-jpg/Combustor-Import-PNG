# Combustor Import PNG

A MelonLoader mod for Combustor that turns PNG images into colored walls in Sandbox.

[Download the mod ZIP](./CombustorPngMod.zip?raw=true)

## Install

1. Install [MelonLoader](https://github.com/LavaGang/MelonLoader) for Combustor, then close the game.
2. In Steam, right-click Combustor and choose **Manage → Browse local files**.
3. Extract the ZIP into the folder containing `EngineSimulator.exe` so its `Mods` and `UserData` folders merge with the existing folders.
4. Start Combustor and enter Sandbox.

Tested on Windows x64 with Combustor Steam build 26701522 and MelonLoader 0.7.3.

## Use

Press **Esc → Import PNG**, or copy a PNG image or file and press **Ctrl+V**. Scroll to resize the preview, left-click to place, and right-click or press **Esc** to cancel.

Opaque pixels become colored walls using the selected material. Pixels below 50% opacity are skipped. Keep the `.engine.pngcolors` sidecar with its `.engine` file when sharing a colored build.
