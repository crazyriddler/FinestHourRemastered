# Finest Hour Remastered: latest update

The latest fixes for Finest Hour Remastered 1.0, all in one small program. It is cumulative: it updates any earlier
installation of 1.0 to the latest version, whichever version it has now. Your settings (`codfh.toml`) and saved
games are kept.

New installations do not need it: the installer on the [1.0 release](https://github.com/crazyriddler/FinestHourRemastered/releases/tag/v1.0)
always carries the latest update and installs it with the game.

## How to update
1. Download `FinestHourRemasteredUpdate.exe`.
2. Run it and choose the folder where Finest Hour Remastered is installed, then **Update**.

The version installed is in `version.txt` beside the game. Unattended:
`FinestHourRemasteredUpdate.exe --update "C:\Games\Finest Hour Remastered" --log update.log` (exit code 0 updated,
1 failed).

## Changes since 1.0
- The world's animated textures now move, as their artists made them: the light caustics on the sewer walls, the
  falling water, the Volga and the rivers, glowing coals, fans and more. The game always ran these animations, but
  RenderWare drew those surfaces without them: their sectors never had the material effects turned on.
- The pause screen: the key and button pictures are square on wide screens and no longer overlap the prompt before
  them (at 4:3 too). On the controller, B now leaves the pause screen from any of its pages, as START does: the game
  showed B for continue and back but never bound it there.
- Shadows go from Off to High: Ultra is gone (it cost a lot of GPU time for little difference). The presets now set
  High shadows on Very High, Medium on High and Medium, Low on Low. Your settings move over by themselves: a preset
  stays that preset with its new shadows, and Ultra shadows in your own settings become High.
- New HUD options (PC Settings, Display): Hit Markers, Compass, Stance and Crosshair can each be hidden, with the
  original or the remastered graphics (a tank's sight always stays).
- PC Settings opened from the pause screen on a wide screen are inside the television, as from the main menu.
- Level 4 (the sewers): the white disc under the pipe's splash and the white layer over the waterfall are gone; a
  texture stage that reads no texture now reads black, as on the console.
- The installer and the updater speak English, French, Italian and Spanish (the system's language), and a new
  installation starts the game in the system's language when it is one of the game's four (English otherwise).

Call of Duty is a trademark of Activision Publishing, Inc. This project is not affiliated with or endorsed by
Activision.
