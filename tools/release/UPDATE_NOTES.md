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
- New **Baked Lighting** (Graphics, on by default): every level's world has been ray traced ahead of time, its
  ambient occlusion and the sun's or moon's shadows with real soft edges, for every surface and for the air over the
  floors (soldiers walking into a building's shadow are shaded too). The shade in corners no longer shimmers or
  changes as the camera moves, sees what is off the screen, and the static world's shadows now reach as far as the
  eye sees. The game's own painted lighting is kept: the bake only shades it. It is also lighter on the GPU: the
  real-time shadow maps now draw only what moves.
- **Reflections** are baked too: every level has probes, ray traced ahead of time, that see what is about each glossy
  surface in the colours the game draws it. The reflections no longer shimmer or come and go as the view turns, show
  what is off the screen or behind you, and cost less than before.
- The **bounce light** (Bounce Light, Effects) comes from the bake too: the light off the walls and the ground about
  a surface, seen or not, steady, for a fraction of its cost before (what moves still takes it from the screen).
- **Anti-Aliasing** is now a choice: Off, SMAA, SMAA + 2x MSAA or SMAA + 4x MSAA. SMAA smooths the whole 3D with
  every effect on it (fences, foliage, particles, the post-processing), never the HUD or the menus; MSAA adds the
  models' and the world's edges. The temporal anti-aliasing is gone: with the lighting and the reflections baked
  nothing needs it to smooth its grain any more, and the picture stays sharp in motion. Very High and High use SMAA +
  4x MSAA, Medium SMAA + 2x MSAA, Low SMAA, Original none. (Before, Low had no anti-aliasing at all: its SMAA was
  never applied.)
- The **Render Scale** option is gone: the game is always drawn at the screen's resolution.
- **Ambient Occlusion**: the world's own shade comes with Baked Lighting and stays with the option off; the option
  adds the screen's shade about what moves (soldiers, vehicles).
- The **presets** were reworked around the baked lighting: the bounce light is on down to Medium, the floating dust
  too, the anti-aliasing as above. At 2560x1440 on an RTX 4080 the first level takes about 3.1, 2.9, 2.2, 0.9 and
  0.5 ms of GPU time on Very High, High, Medium, Low and Original. Your settings move over by themselves: a preset
  stays that preset, your own choices stay yours.
- The **sun and the moon** (Sun and Moon) are larger, as a film shows them: the sun a white hot disc with a corona and
  a wide glow, the moon twice as large with its seas on its face and a soft halo; round wherever they are on the screen.
- Many effects cost less with the same look: the clouds (worked out at half size, as soft as before), the floating
  dust and embers (only where a fire is near), the flashes' shadows (as many steps as they cross pixels), the
  surfaces' relief (Parallax: as many steps as it shifts on the screen), the volumetric fog's smoothing and the eye's
  exposure meter.
- **Shadows**: each step now costs less than before (High is the old Medium, Medium the old Low, Low lower still):
  with Baked Lighting the static world's shadows are baked, sharp and soft where they should be, and the real-time
  shadow maps only add what moves. Baked Lighting is on in every preset but Original; your settings move over by
  themselves (a file on Original stays Original).
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
- Fires' lights hold steady: their light and shadows on the walls around a fire no longer jump from frame to frame.
- The surfaces' highlights by a shadow's edge no longer flicker as the view turns.

Call of Duty is a trademark of Activision Publishing, Inc. This project is not affiliated with or endorsed by
Activision.
