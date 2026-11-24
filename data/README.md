# The game's data

The m20 build reads `TOMB1/DATA/` on the card: Tomb Raider's (1996) own
files from the CD's `DATA/` directory (the levels as `.PHD`, the title and
credit pictures as `.PCX`; 50 MB). They aren't in the repository. Put them
in `data/DATA/` here (m20-system's `sdcard/make_sdcard.sh` takes them from
there, or from `--tr1 DIR`). The host harness reads them from `M20_DATA`:
the card's tree (`M20_DATA=../sdcard/out/card` in m20-system), its
`TOMB1/`, or any directory holding `DATA/` and `cfg/` (`../bin/cfg`);
by default the current directory. What the game writes goes to `M20_OUT`.

Any copy of the PC game has them: the CD, or Steam's and GOG's *Tomb Raider
I*, which carry the CD as `GAME.GOG`, a raw image (2352-byte sectors,
`GAME.DAT` is its cue sheet): strip each sector to its 2048 bytes of data
(from byte 16) to get an ISO, and extract `DATA/` from it (e.g. with 7z).
The Unfinished Business levels aren't on that CD.
