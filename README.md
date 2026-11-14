# Tomb Raider on the m20

This branch (`m20`) runs Tomb Raider (1996) on the m20, a RISC-V SoC on
the Tang Nano 20K (`../core` in m20-system). The game is TRX (formerly
Tomb1Main) at its first standalone, fully decompiled build; everything
below this section is upstream's README from then.

**What's new here:** a platform layer (`src/m20/`: video, sound, keys,
files on the SD card) and a software renderer (`src/specific/s_hwr.c`)
in place of the OpenGL one, with no doubles (the FPU is single
precision). The opaque textured faces go to core's span engine
(`src/specific/s_hwr_span.h`): it maps them with perspective, lights them
per pixel from a small lightmap, tests depth, and walks their edges
itself; the CPU draws the rest (transparent faces, sprites, shadows, the
2D) in the same order. The levels' pages are tiled for the engine's
texel cache, and the faces under water use its second palette.

**Speed** (640 × 360, the timedemos: `make TIMEDEMO=1`): the title 29.8
fps, City of Vilcabamba 11.3 fps, Lost Valley 20.2 fps; with the CPU only
(`SPANOFF=1`) Vilcabamba runs at 3.3 fps.

**Building:** `make` (the board: `build-640x360-z-t-e/tomb1.bin`) needs
riscv64-unknown-elf-gcc, picolibc and core (`CORE`, `../core` by default).
`make host` builds a test harness for 32-bit x86 Linux (SDL2, i386) with
the board's frame and heap, which draws the engine's faces with its C
model (`core/firmware/span_core.h`); its environment variables are listed
in `src/m20/host_sdl.c` (`M20_SEED` repeats a run, `M20_TRACE` records the
engine's commands for core's `sim/run_trace.sh`). The Makefile lists the
other options (`TIMEDEMO`, `PROFILE`, `TIMERS`, the engine's features).

**On the card:** `TOMB1.BIN` and `TOMB1/` with `DATA/` (the game's files,
see `data/README.md`) and `cfg/` (`bin/cfg` here); m20-system's
`sdcard/make_sdcard.sh` puts them there.

**Keys:** a USB keyboard on either port, or the terminal's (arrows,
Enter, Esc, letters), with TRX's controls: arrows to move, right Ctrl or
F for action, right Alt or J to jump, Space or G to draw weapons, Esc for
the inventory (the terminal has the letter keys). On the terminal, Ctrl-K
switches the span engine off and on, Ctrl-Z its depth test.

**License:** GPL 3 (`COPYING.md`), as upstream.

# Tomb1Main

This is a dynamic library for the classic Tomb Raider I game (TombATI version).
The purpose of the library is to reimplement all the routines performed by the
game and enhance the gameplay with new options.

See the [Tomb Raider Forums
topic](https://www.tombraiderforums.com/showthread.php?p=8286101).

This project is inspired by Arsunt's
[TR2Main](https://github.com/Arsunt/TR2Main/) project.

## Installing

1. Get a copy of the latest Tomb1Main release from
    [here](https://github.com/rr-/Tomb1Main/releases).
2. Unpack the contents to your game directory. Make sure you overwrite existing
    files (Tomb1Main_config.json5 can be left alone).

To play the Unfinished Business expansion pack, launch the game with `-gold`
command line switch:

1. Create a shortcut to `tombati.exe`
2. Select the newly created shortcut, go to Properties
3. At the end of the "target" field, append `-gold` so it looks something like
    this:
    ```
    (...)\tombati.exe -gold
    ```

## Configuring

To configure Tomb1Main, edit the `Tomb1Main.json5` file in your text editor
such as Notepad. All the configuration is explained in that file.

## Improvements over original game

Not all options are turned on by default. Refer to `Tomb1Main.json5` for details.

- added proper UI and bar scaling
- added enemy health bar
- added more control over when to show health bar and air bar
- added customizability to health bar and air bar
- added ability to set user-defined FOV
- added ability to select weapons / using items with numeric keys
- added ability to look around while running
- added TR3-like sidesteps
- added shotgun flash sprites
- added a fly cheat
- added a level skip cheat
- added a door open cheat (while in fly mode)
- added ability to disable all medpacks
- added ability to disable Magnums
- added ability to disable UZIs
- added ability to disable shotgun
- added ability to disable main menu demos
- added ability to disable FMVs
- added ability to disable cutscenes
- added ability to disable healing between levels
- added rendering of pickups on the ground as 3D meshes
- added braid (currently only works in Lost Valley)
- added support for displaying more than 3 pickup sprites
- added a choice whether to play NG or NG+
- added Japanese mode (guns deal twice the damage); available for both NG and NG+
- added external game flow (no longer 2 different .exes for TR1 and TR1UB). For TRLE builders:
    - the levels can be reordered
    - the levels can be renamed
    - all the strings can be translated, including keys and items
    - you no longer are constrained to 4 or 21 levels only
    - you can offer a custom Gym level
    - you can change the main menu backdrop
- added automatic calculation of secret numbers
- added compass level stats
- added ability to keep timer on in inventory
- added save game crystals game mode (enabled via gameflow)
- added pause screen
- added movable camera on W,A,S,D
- added Xbox One Controller support
    - Per Axis Dead Zone
    - Left Stick = movement
    - A = Jump/Select
    - B = Roll/Deselect
    - X = Action/Select
    - Y = Look/Select
    - LB = Walk
    - RB = Draw Weapons
    - Dpad Up = Draw Weapons
    - Back = Option
    - Start = Pause
    - Right Stick = Camera Movement
    - R3 = Reset Camera
- added rounded shadows (instead of the default octagon)
- added per-level customizable water color (with customizable blue component)
- added per-level customizable fog distance
- added adjustable in-game brightness
- changed internal game memory limit from 3.5 MB to 16 MB
- changed moveable limit from 256 to 10240
- changed maximum textures from 2048 to 8192
- changed maximum texture pages from 32 to 128
- changed input method to DirectInput
- fixed inability to switch Control keys during shimmy
- fixed skipping FMVs triggering inventory
- fixed skipping credits working too fast
- fixed setting user keys being very difficult
- fixed keys and items not working when drawing guns immediately after using them
- fixed not being able to close level stats with Escape
- fixed freeze when holding action key during end of level
- fixed reading user settings not restoring the volume
- fixed Tomb of Tihocan playing secret sound
- fixed The Great Pyramid secret
- fixed running out of ammo forcing Lara to equip pistols even if she doesn't carry them
- fixed a crash when Lara is on fire and goes too far away from where she caught fire
- fixed settings not being saved when exiting the game with Alt+F4
- fixed settings not persisting chosen layout (default vs. user keys)
- fixed the sound of collecting a secret killing the music
- fixed the infamous Tihocan crocodile bug (integer overflow causing creatures to deal damage across the entire level)
- fixed Lara jumping forever when alt+tabbing out of the game
- fixed Lara voiding if a badly placed timed door closes on her (doesn't occur in OG levels)
- fixed bats being positioned too high

## Showcase

#### Mode selection

![](docs/showcase1.jpg)

#### Braid

![](docs/showcase2.jpg)

#### Fly cheat

![](docs/showcase3.jpg)

#### Door open cheat

![](docs/showcase4.jpg)

#### Enemy health bars

![](docs/showcase5.jpg)

## Q&A

1. **Is the game fully playable from beginning to the end?**

    Yes. If you encounter a bug, please file a ticket.

2. **Can we get HD textures? Reflections? Other graphical updates?**

    Eventually, probably yes. But first we need to decompile the renderer,
    which is going to take a while (can't give an estimate).

3. **Can we get braid in every level? Skyboxes? Flyby cameras? New animations? etc.**

    Not sure at this moment; this requires meddling with the original game
    level files. It's one thing fixing a faulty trigger bit in The Great
    Pyramid for the final secret, and another to insert a completely new
    animation, a textured mesh or a sound file and pretend it's always been a
    part of the original game. So far we haven't found a good way that'll keep
    the code maintainable.

4. **Can I play this on Mac, Linux, Android...?**

    Currently only Windows version is available.

5. **Do I really need to install this TombATI patch? Why not have just the .exe?**

    One of our goals is to eventually provide a single .exe file (maybe with
    some .dll-s related to multimedia) and only require the players to supply
    the game data, i.e. the level files, music and FMVs. However, this requires
    decompiling GFX and SFX routines which, again, is going to take a while.

## Decompilation progress

Currently the entirety of TombATI.exe's code is decompiled. Our goal now is to
ship with our own .exe rather than a DLL. To make it possible, we must reverse
portions of the glrage patch (glrage.dll) and the ATI renderer (ati3dcif.dll)
and that is the current project focus.

## Importing data to IDA

This option requires IDAPython and a recent version of IDA. Install Python 3.8+
and IDA 7.5+, then hit alt+f7, then choose `scripts/ida_import.py`.

## License

This project is licensed under the GNU General Public License - see the
[COPYING.md](COPYING.md) file for details.

## Copyright

(c) 2021 Marcin Kurczewski. All rights reserved. Original game is created by
Core Design Ltd. in 1996. Lara Croft and Tomb Raider are trademarks of Square
Enix Ltd.
