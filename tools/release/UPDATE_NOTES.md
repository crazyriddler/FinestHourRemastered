# Finest Hour Remastered: latest update

The latest fixes for Finest Hour Remastered 1.0, all in one small program. It is cumulative: it updates any earlier
installation of 1.0 to the latest version, whichever version it has now. Your settings (`codfh.toml`) and saved
games are kept.

New installations do not need it: the installer on the [1.0 release](https://github.com/crazyriddler/FinestHourRemastered/releases/tag/v1.0)
always carries the latest update and installs it with the game.

> [!IMPORTANT]
> The baked lighting update of 19 October has been withdrawn: it broke too many things. This update brings any
> installation back to the version before it, that update's included (its baked lighting files are removed, the
> textures it changed restored), and brings earlier versions up to it as usual. Your settings and saved games are
> kept.

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
- The updater no longer stops at "Cannot write d3dcompiler_47.dll": files already up to date are left alone, and a
  game still running from the folder (even without its window) is closed first.
- A frozen game's log now tells more of where it stopped, for reports.
- Much less CPU in play: the game's background loading thread kept a whole core busy waiting for work (harmless on
  the Xbox, not on a laptop or a handheld). It now rests while there is nothing to load: at a 60 FPS cap the game
  went from about 140% of a core to about 40%.
- The main menu's Quit Game row makes the menu's move sound like the other rows.
- New Toggle Aim option (PC Settings, Controls; off by default): a click of the mouse's aim button aims, the next one
  stops, and reloading or throwing a grenade stops it too, as in Call of Duty up to Black Ops II. Iron sights and
  scopes alike; keys and the controller's trigger still aim while held.
- Level 4 (the sewers): the white disc under the pipe's splash and the white layer over the waterfall are gone; a
  texture stage that reads no texture now reads black, as on the console.
- The installer and the updater speak English, French, Italian and Spanish (the system's language), and a new
  installation starts the game in the system's language when it is one of the game's four (English otherwise).
- Crashes fixed: moving the tank on Airfield Ambush after the planes are destroyed, and the dead-end room in the
  tunnel (among others at level loads). The game's loading thread and its main thread changed the same RenderWare
  lists at once: on the Xbox's single core they always took turns, on a PC's many cores they did not. They take
  turns again, with no cost to the frame rate.
- More crashes fixed (a fault in RenderWare's frames or memory blocks, at any point of play): the turns above were
  taken by the wrong thread. The game's main thread is not the one the XBE starts with (that one only starts main's
  thread and ends), so main and the loading thread still ran at once. Now main's thread takes the turns.
- Muzzle Flash Shadows: soldiers and things by a shot or a fire cast their whole shadow, not only below the
  knees: anything in the way shades now, only the gun firing (or a fire's own heap) is left out. The shadows' edges
  are smooth, with no extra cost.
- A freeze fixed (Airfield Ambush coming out of the hangar after the planes, and elsewhere while playing): the
  loading thread, now taking turns with the game's main thread, waited in a loop for the main thread to let go of
  what it was loading, holding its turn all the while. It gives its turn up while it waits.
- HUD Size: the compass no longer grows back and wobbles as the view turns (its dial turns with the view and was
  measured by its turning outline); nor does it shift when the objective star's pulse or a tank's turning
  hull reaches past it.
- The compass rose: each point's base is whole and none of them runs into the centre dot.
- New HUD Size option (PC Settings, Display): Original, Small or Smaller. The compass with the stance, the
  crosshair, health with the medkits, the ammunition and a tank's bars are drawn smaller where they were, each
  keeping its distance to the screen's edges; subtitles, hints and the hit markers keep their size.
- New language: Portuguese (for Brazil and Portugal). The menus, the objectives, the hints and the subtitles in
  Portuguese, a translation of the port's own, with the game's English speech. PC Settings, General, Language; a
  Windows in Portuguese starts the game and the installer in it.
- The game runs at 60 FPS always, as on the console: above it the game itself ran faster. The Max Frame Rate option
  is gone; V-Sync stays.
- Fires' lights hold steady: their light and shadows on the walls around a fire no longer jump from frame to frame.
- The surfaces' highlights by a shadow's edge no longer flicker as the view turns.

Call of Duty is a trademark of Activision Publishing, Inc. This project is not affiliated with or endorsed by
Activision.
