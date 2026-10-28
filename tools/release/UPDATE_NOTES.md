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
- Height Blend: a blended layer's edge (dirt over cobbles, plaster over bricks) is soft, no longer broken into
  pixel specks and blotches.
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
- **New: Remastered Models.** The game's models remastered: the soldiers of every army, the weapons in your hands
  and in theirs, the pickups, the vehicles (tanks, half-tracks, trucks, planes, anti-tank guns), the objects and the
  scenery. Round barrels, drums and stocks, soldiers without the old corners, round turrets and wheels, with the
  game's own textures and animations; the water, flat surfaces and the boats' hulls stay as they were.
- Remastered Models: no more textures smeared into streaks (the cobbles at the start of Road to Remagen, rubble,
  the inner corners of steps), and no more models flickering between their remastered and original shapes.
- Smooth Models: the T-34's gun (and other long thin parts) no longer swells into a cigar.
- Weapons in hand on a wide screen: no more loose bits of sleeve in the bottom corners (the arm's cut end the
  console's 4:3 screen hid), not even the small corner of it the console's screen showed, with nothing cut from the
  arms holding the weapon; the remastered weapons follow the game's own trimming triangle by triangle.
- Clouds: no more hard lines and arcs across the sky (in every level); the clouds' edges are soft and they take
  the light of the whole sky, day or night.
- Field of View is back (PC Settings, Display): wider in play, while cutscenes keep the game's own, and the weapon in
  your hands stays as the game draws it (no stretched arms at a wide field of view). Inside the tanks too.
- Keys 1 and 2 pick your primary and secondary weapon, as in later Call of Duty games; the mouse wheel still
  switches.
- Toggle Aim is on by default (a click of the aim button aims, the next one stops); your settings move over once,
  and you can turn it off in PC Settings, Controls.
- The lean hint shows the lean keys (Q and E) on the keyboard, or the d-pad on a controller, instead of naming the
  directional buttons.
- PC Settings: the options' descriptions stay inside the page in every language.
- The main menu: its text is sharper inside the old television, with the letters' original black outline and no
  light halo around them.
- Tanks: inside the tank, the sight's black band at the bottom now reaches the corners of a wide screen.
- HUD Size: Smaller by default, and the graphics presets set it too (Original keeps the original HUD).
- The menus' old television: a slightly stronger picture tube (scan lines, a faint phosphor mask and glow) that
  hides the low resolution of the menus' backgrounds, with the menus' text as sharp as before. A new Models setting (PC Settings, Graphics)
  chooses them: Remastered (the default), Smooth (the game's own characters and weapons rounded, the old Smooth
  Models) or Original; the Original preset sets Original. Your settings move over by themselves: Remastered if you
  had them on.
- Crash fixed: Road to Remagen, the small passage around the large building half-way through (it also crashed
  there on the Xbox emulators). When a part of the level was unloaded, things still standing in it kept pointing at
  it, and the game used it again later. That could also crash elsewhere, at any time, after long play.
- Crash fixed: the end of a mission (the British missions every time, the others sometimes). The port kept the
  game's camera after the game had thrown it away and still read from it as the next level loaded.
- Crash fixed: the American sewers, restarting from a checkpoint (or reaching a new part of the level). One check
  of the game's looked into a part of the level that was marked loaded before it was there; every other one of
  its checks waits for it.
- Leaving a mission to the main menu, its "Loading" and the Call of Duty logo show in the television, like the
  rest of the menus.
- HUD Size: the grenade's box (and its icon) shrinks with the rest of the HUD.
- The first-person hands no longer show dark streaks like dirt (the American hands above all).
- Anti-Aliasing: roofs and edges against the sky are smooth again, not in stair steps.
- Muzzle Flash Shadows: their edges are slightly soft, still sharp and as dark as before, at no extra cost.
- Whole surfaces no longer flicker between light and dark as the view moves (a roof, windows, the ground by the
  rails): the reflections and the clouds took the sky's colour from a dozen points of the screen, and it changed
  as soon as one of them saw the sky or not (through a doorway, past a post or a wire). The sky's colour is now held
  steady from frame to frame, and a reflection no longer takes a doorway's frame far in front of it for what it
  reflects.
- Tanks' vision slits: the dirt on the glass no longer shows as dark pixel blocks against the light. It is soft and
  faint, and the view through the glass stays sharp.
- The health, medkit and ammunition meters stay in their corner while a subtitle is up (they slid towards it and
  back on wide screens).
- The installer and the updater work on Linux (Wine, Proton): they unpack the update with their own decoder now, and
  no longer stop at "This program has no update inside" (Wine lacks the Windows part they used). The update is
  smaller too.
- PC Settings: no thin dark line across the page under the hints, no flash of the settings outside the television
  as they open and close.
- The main menu: PC Settings is centred like the other rows in every language (in Spanish it is now "Ajustes de
  PC"), and up from Single Player goes round to Quit Game, as down from Extras does.
- The loading screen with the Call of Duty logo shows the game's own picture, as the main menu's movie after it.
- Extras: the gallery's and the credits' movies no longer flicker with the main menu's picture at their sides.
- The lean hint's keys are two key caps, Q and E, each its own, with no full stop after them.
- The sewers' falling water: no more small white patches and lines on the pipes' streams (they were lit as pools),
  and a stream no longer looks cut where it meets the wall it falls from (the wall's grime was drawn over it).
- Remastered Models: soldiers further off no longer show broken arms and hands (frayed strips by the rifle). The
  game's distant versions of the soldiers, of every army, stay as the game has them: remastered from so few
  triangles they came out broken, and from that far the original is what the eye sees anyway.
- Snow Tracks: tanks and half-tracks leave the prints of their tracks in the snow too, cleats and all, as the
  soldiers leave their footprints.
- Much faster, with every effect on. On the CPU, busy scenes run up to twice as fast (the boat ride of the first
  mission went from about 75 to about 180 FPS on my PC): the remastered models are found once instead of every
  frame, only the game's changed states are passed on each draw, the soldiers' weights and bones no longer make
  the CPU convert their whole models, and much less is copied to the GPU. On the GPU, the same picture takes about
  a third less time: Ambient Occlusion and contact shadows are worked out at half size, the reflections are traced
  at half size, the dust and embers in the air are drawn as points instead of searched for at every pixel, Surface
  Materials' relief takes only as many steps as it moves on the screen, and its texture filtering costs less where
  nobody can see the difference.
- New Renderer option (PC Settings, Display): Direct3D 12 (the default) or Vulkan, the same picture through either.
  Vulkan is new and experimental; the change takes effect the next time the game starts.
- New Cheats page in PC Settings, everything off by default: Free Camera (F11 in play flies the camera through the
  level and its walls, F11 again comes back), God Mode, Infinite Ammo (grenades and mines included) and No Reload
  (the magazine of the weapon in hand stays full).
- New Sound option (PC Settings, General): Stereo or 5.1 Surround. In 5.1 the game mixes for Dolby Digital, as
  on a console connected to a surround receiver: gunfire and explosions around you on the front, back,
  centre and subwoofer. It takes effect the next time the game starts.
- Cloud Shadows is gone: it made no visible difference on the ground and cost GPU time every frame.
- Linux (Wine, Proton): the game no longer crashes at the start with the Direct3D 12 renderer. It asked for a few
  more texture samplers than Direct3D 12 allows; Windows drivers let it pass, vkd3d-proton does not.
- Vulkan renderer: changing the graphics preset could hang the GPU on some drivers (Intel on Linux): a newly made
  render target could be written to before it had finished being cleared.
- Anti-Aliasing is now FXAA, on the 3D scene only: the HUD and text stay sharp. It replaces 4x MSAA with temporal
  anti-aliasing, which blurred the picture in motion and cost much more; for a sharper image still, raise
  Render Scale.
- Surface Materials no longer shimmer as the view moves: their relief, highlights and reflections are filtered so
  detail finer than a pixel no longer sparkles (the temporal anti-aliasing used to hide it).
- Reflections are steady and worn surfaces reflect much less: the game's dirty metal floors no longer look wet,
  and every material is a little rougher.
- Smoke from a blast covers what is inside it evenly: figures and rubble in the smoke no longer show as black
  shapes (the blast's flash shadows darkened the smoke over them).
- Vehicles no longer leave a light patch under them in the shade (the smoke fix above also lifted the shadow under
  their dark blob).
- The sewers' falling water from the pipe meets the water on the floor without a pale disc around its foot.
- Wind now shows: the trees' leaves sway (and flags and cloth hung in the world, half as much). It moved them a tenth
  of a pixel before, and left out the trees the game lights as models.
- Wind: the trees' shadows no longer jump about as the camera turns (the sway followed the leaves' place on the
  screen; it follows the leaves themselves now, and the shadows stay still).
- Shadows no longer drift a little on the ground as the camera turns: the level's sun keeps its direction.
- Snow Tracks: the tanks' prints have smooth edges instead of stair-stepped lines, and look deeper.
- Underground Passage (the American sewers): the red warning light by the ladder blinks red and see-through again,
  instead of a black block.
- Defend the Factory: the metal beams no longer flicker between light and dark as the view turns.
- Into Red Square (the tank): no more dark patch on the cobbles ahead that came and went as the tank drew near (the
  shadow of a nearly see-through layer of rubble and dirt, cast as if it were solid).
- Surface Materials: less streaking where the game's own texture is stretched along a surface (walls, beams).
- Faster, on both renderers: much less CPU per frame (the game's vertices that never change are no longer copied and
  converted again every frame, the weapon in hand and the render states cost far less, the shadow casters are
  recorded on several threads) and less GPU time (shadows, ambient occlusion, bounce light, volumetric fog, dynamic
  lights, Not One Step Back's oil smoke, the lens dirt, the anti-aliasing and the guard band all cost less). On my PC
  the CPU-heavy places run 10 to 40% faster, the rest a little faster, with the same picture.
- Depot Saboteurs: the town's walls no longer show cracks the night sky shows through, and a building's top band is
  straight again. Its walls, doorways, steps and mountains are drawn as the game made them: my remastered versions
  of them opened those cracks.
- Pausing in a snow level could garble the snow tracks or the TV screen of the menu: both were drawn into the same
  texture.
- Ambient Occlusion and Contact Shadows no longer draw shaded lines across the ground seen at a low angle (Raiding
  the Fortress's courtyards most of all).
- Not One Step Back: the oil smoke over the city is now the smoke's own texture in high resolution, slowly rising
  and drifting, in place of the generated smoke: sharper, much cheaper, and without the hard edges and flicker of
  the smoke pieces under it. The white clouds no longer show between the smoke there.
- Not One Step Back: the oil smoke's outline blends softly into the sky, with a finer layer of smoke along it that
  rises with the column's shape (planes and buildings in front no longer disturb it), and looking straight up no
  longer pinches the smoke into a star.
- New language: Russian (PC Settings, General, Language). All the game's text, subtitles, objectives and menus, the
  PC Settings too, with Cyrillic letters in the game's own fonts, with or without the remastered textures (the voices
  stay in English). A first translation: corrections are welcome in issue #34.
- New option, Cinematic Depth of Field (PC Settings, Effects, off by default): in the game's own cutscenes the
  background goes out of focus, the characters in front sharp. Holding aim in a cutscene no longer blurs the view.
- Operation Little Saturn: the Katyusha rockets' flame no longer has a dark halo around it against the night sky, and
  it lights its surroundings as it flies.
- The hint for cycling the thrown item (mines, grenades) shows the Switch Grenade key (T by default) instead of the
  lean key (E), which did nothing there (issue #38). In Russian, the lean hint shows the keys instead of naming the
  d-pad.
- New language: Polish (PC Settings, General, Language), like Russian: all the game's text and the PC Settings, with
  Polish letters added to the game's own fonts, with or without the remastered textures (the voices stay in English).
  A first translation: corrections are welcome in issue #34.
- HDR output (PC Settings, Display; Direct3D 12): on by default when HDR is on in Windows (off and locked otherwise),
  with the display's peak from Windows. Brighter lights, fire and skies and brighter lit mid tones, the HUD, the text
  and the menus at Windows' SDR content brightness. HDR Peak Brightness and HDR Game Brightness to adjust it.
- The installer and the updater show a small window while they unpack (it can take a minute), and if they cannot
  start they now say why, with an error number (and a log in the Temp folder) instead of closing silently (issue #40).
- The game looks for updates at its start, in the background (it never delays the start, whatever the connection):
  when there is a newer one, the main menu shows what it brings, in the game's language, and offers to install it:
  Update (downloaded, checked, installed by the updater, the game started again, nothing left in the Temp folder),
  Not Now, or Don't Ask Again, which turns the check off; Check for Updates (PC Settings, General) turns it back on.
  From this update on, the next ones are offered in the game.
- The installer and the updater run on any 64-bit processor: they stopped at once (error 9) on processors without
  SSE4.1 (issue #40). The game itself needs SSE4.2 (README, Requirements).
- Movies: the credits (at the end of the game and in Extras) no longer leave pieces of the names before behind the
  new ones, like a badly encoded video (issue #42). The game copies each frame of a movie straight onto the screen;
  only the pixels that changed were taken, and those that went back to black (text fading out) kept the frame
  before's.
- PC Settings: the Television Frame's description says an old television, not a 1940s one.
- Russian: the translation reviewed by a native speaker (YaroslavValerySablin, issue #34), and the movies subtitled in
  Russian from his subtitled versions: the briefings, the acts' and the ending's, drawn into the movie's own picture
  as if burned in (the game's movies are not changed or shipped).
- Russian and Polish: Quit Game is back under the main menu, and the mouse points and clicks in the menus again: their
  letters are drawn into the game's fonts, which changed the fonts the menus were recognised by. Empty profile slots
  are translated.
- Russian, Polish and Portuguese: Press ENTER is centred on the title screen, as in the other languages, and the
  menus' rows are centred too (the main menu, the campaigns, Extras and the rest): the translations kept the English
  rows' spaces, which centred the English words, not theirs.
- Remastered models: the Tunisian towns' buildings (Matmata by night and by day, and the desert's) are drawn as the
  original again, all of them by their textures, wherever they load: remastered they lost their shape, rounded and
  bent (as the walls of Matmata by day did before).
- Cheats: the Free Camera moves three times as fast (about 18 m a second, four times that with fire held).
- Remastered textures: the lattice towers' crossed beams use the original texture again: the remastered one's cut-out
  edges broke into dots.
- Russian: the briefings' subtitles now show in every briefing (the ones played while a mission loads had none, Alexander
  Sokolov's among them), the profile keyboard's Delete, Done and Shift are translated (as in Polish and Portuguese: rows
  of the game's text tables past their first 4 KB were never translated), and the town in Sam Rivers' briefing is
  Тиллет throughout.
- Remastered textures: the Matmata headquarters (Matmata by night) lit right: its plaster walls and roof keep their
  remastered colour without the surface maps that showed lighter patches where the faces met and a diagonal across
  the roof, and its flag is lit as cloth, not skin (light polygons over it).
- The HUD: the thrown items' remastered icons (the grenades, the Teller and sticky mines, the bears) all sit in one
  place at one size, clear of how many you carry: the Teller mine's disc covered the count and the grenades touched it.

Call of Duty is a trademark of Activision Publishing, Inc. This project is not affiliated with or endorsed by
Activision.
