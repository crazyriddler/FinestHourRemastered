# Finest Hour Remastered 1.0

A native PC port of Call of Duty: Finest Hour (original Xbox, 2004) with a remastered renderer. By crazyriddler.

## Downloads
- `FinestHourRemasteredSetup.exe`: the installer.
- `FinestHourRemastered.index` and `FinestHourRemastered.data.001` to `.003`: the port, the remastered textures and
  the shaders' recipes (4.85 GB). Put them in the same folder as the installer.
- `FinestHourRemastered_Trailer_1440p60.mp4`: the trailer.

## Requirements
- Windows 10 or 11, 64-bit, a Direct3D 12 GPU with 4 GB of video memory or more, about 9 GB of disk.
- **Your own copy** of Call of Duty: Finest Hour for the original Xbox, **PAL (Europe) release**, title ID
  `4156002A`, version 3 (`default.xbe` SHA-1 `938a7b1e17928de49cb3c5834d61abcd3d9081c8`). No game files are included.

## Install
Run `FinestHourRemasteredSetup.exe`, choose your copy (its folder, a disc image or an XISO) and where to install.
The first start prepares every shader for your GPU in a few seconds; none is compiled while you play.

## Updates
The installer here always carries the latest update and installs it with the game. Already installed? Get
`FinestHourRemasteredUpdate.exe` from the [latest update](https://github.com/crazyriddler/FinestHourRemastered/releases/tag/update)
release (its date is the update's): it brings any earlier installation to the latest version and keeps your
settings and saved games.

## Highlights
- Statically recompiled to native x86-64 code: no emulator. Any resolution, widescreen (Hor+), 30 to 360 FPS or
  unlimited, keyboard and mouse with rebinding, Xbox, PlayStation and Switch controllers.
- Per-pixel lighting, real-time sun and moon shadows, dynamic lights from every flash, tracer and fire, volumetric
  fog, sun shafts, clouds, lens flare, ambient occlusion, bounce light, HDR with bloom and eye adaptation.
- Every texture remastered 4x with normal, roughness and height maps; surface materials, parallax occlusion, detail
  textures, character detail, screen-space reflections, rippling water, snow tracks.
- Redrawn high-resolution fonts and HUD; the menus inside a 1940s television.

See the README for the full list, the controls and how to build from source.

Call of Duty is a trademark of Activision Publishing, Inc. This project is not affiliated with or endorsed by
Activision.
