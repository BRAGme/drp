# drp

Discord Rich Presence that says what you are **doing**, not just which game is
open. One small background program, `drp.exe`, that watches for either of:

| game | how it reads it |
|---|---|
| any PCSX2 | over PCSX2's built-in PINE server |
| Mercenaries: Playground of Destruction (PC port) | reads the game process's memory |

Neither is a fork or a patch, so both work with a binary you did not build, and
the exe can be handed to a friend as it is.

```
PCSX2                                     Mercenaries
Tom Clancy's Rainbow Six 3 • Terrorist    Jennifer Mui • On contract: Hunt down
Hunt                                      Sergei for the Mafia
Alpine Village A • Split screen           Eliminate the renegades • Driving •
01:12 elapsed                             in North West Korea
```

## Never thinking about it again

```
drp.exe --startup on
```

In PowerShell, a quoted path at the start of a line is read as a string, so a
full path needs the call operator or it fails with "Unexpected token":

```powershell
& "C:\path	o\drp.exe" --startup on
```

That adds one `HKCU\...\Run` entry so it starts when you log in, and
`drp.exe --startup off` takes it back out. It has no window of its own, keeps one
copy running at a time, and while nothing is playing it does no more than glance
at the process list every five seconds. What it does goes to `drp.log` next to
the exe.

Run it from a terminal and it prints to that terminal instead, which is the way
to watch it work.

## How it works

PCSX2 has a built-in IPC server called PINE. With PINE switched on, a program on
the same machine can ask the emulator for the disc serial and read the emulated
PS2's memory. `pcsx2-drp` does exactly that once a second: it looks up a few
known addresses for the game you are playing, works out the mode, the map and
whether it is single player, split screen or online, and pushes that to Discord
over Discord's own local socket.

Nothing is written to the emulator or the game. It only reads.

## Setup — PCSX2

1. **Settings → Advanced → Enable PINE** (leave the slot at 28011).
2. **Settings → Interface → Enable Discord Presence → off.** PCSX2's own presence
   and this one would otherwise both push a card and fight.

Nothing else: PCSX2 already has a Discord application of its own, which is what
makes its card say PCSX2 and carry PCSX2's artwork. A different PINE slot goes in
`drp.ini`.

The console prints every presence change, so if something reads wrong you can
see exactly what it thought was going on.

## Setup — Mercenaries

This is a PC game, not an emulator, so there is nothing to switch on in the game
itself. Two steps:

1. **Make a Discord application**, because Discord names the card after whatever
   application connects to it. Go to
   <https://discord.com/developers/applications>, press New Application, and
   call it exactly what you want people to read. **Discord allows 32 characters**,
   so "Mercenaries: Playground of Destruction" is refused — use `Mercenaries`,
   or `Mercenaries POD` to tell it apart from Mercs 2. Copy its Application ID.
   Optionally, under Rich Presence → Art Assets, upload a cover image and note
   the name you gave it.
2. Put the id in `drp.ini` next to the exe as `mercs_client_id = ...`. If you
   uploaded artwork, name it there too as `mercs_image = ...`. The file is
   written for you on first run, with these comments in it.

Without an id it still reads the game and logs what it *would* show, so you can
see it working before bothering with Discord — it simply pushes nothing.

### Art assets to upload

All of these are in `assets/` in this repo, already at the 512x512 Discord wants.
Upload them under **Rich Presence -> Art Assets** with exactly these keys:

| key | image | where it came from |
|---|---|---|
| `cover` | `mercenaries_512.png` | the crest in `Mercenaries.exe`'s own icon |
| `merc_chris` | `merc_chris.png` | Chris Jacobs |
| `merc_jennifer` | `merc_jennifer.png` | Jennifer Mui |
| `merc_mattias` | `merc_mattias.png` | Mattias Nilsson |

The three faces are the game's own character-select ID photos, lifted out of its
`fe_char_photos` texture; whoever you are playing shows as the small badge in the
corner of the card. Han Solo and Indiana Jones are not on that sheet, so they get
no badge — the card just drops it.

Put `cover` in `drp.ini` as `mercs_image`. The badge keys are built in, so there
is nothing to configure for those.

The port ships no RTTI-stripped build, which is what makes this possible: every
class name survives in `Mercenaries.exe`, so the tool finds the game's own state
machines by their vtables rather than by hardcoded addresses, and reads (never
writes) their current state. `drp.exe probe` prints everything it can see,
which is the thing to run if a line comes out wrong.

## Supported games

### PCSX2 — `pcsx2-drp.exe`

| Disc | Serial | What it can tell |
|---|---|---|
| Tom Clancy's Rainbow Six 3 | SLUS-20883 | game mode (Campaign, Practice, Terrorist Hunt, the co-op and split-screen variants, the adversarial modes), the map and its A/B half, and whether it is single player, split screen or online. In the menus it says whether you are in the split-screen front end. |

Everything else falls back to the game's name, which is what PCSX2 shows today,
so running the tool never makes a game's presence worse.

### Mercenaries: Playground of Destruction (PC port) — `mercs-drp.exe`

Reports the merc you picked, whether you are on a contract or free roaming, the
contract's own title and who hired you, the live objective line off the HUD, and
whether you are on foot, driving or on a gun — plus the moments worth calling
out, like verifying a Deck of 52 target or planting charges.

```
Jennifer Mui • On contract: Hunt down Sergei for the Mafia
Eliminate the renegades • Driving • in North West Korea
```

Point it at the game's data folder with `--data` if the port is not at
`D:\MercenariesPC\datapc`; that is where the contract titles come from, so the
lines read in the game's own words rather than mine.

Two honest limits, both the game's rather than the tool's:

* **There are no vehicle challenges or races in this game.** The engine has no
  race, lap or time-trial system at all — the only "challenge" is a scoreboard
  that mission scripts write numbers into, shown in the PDA.
* **The map has no place names.** The engine's location actors are unlabelled
  points with no containment test, so the finest "where" available is the level,
  which the tool names after the cities its missions talk about. Those labels
  are editorial and sit in one table in `src/mercs.hpp`.

## Building

Needs Visual Studio with the C++ workload. No third-party libraries — the PINE
client and the Discord IPC client are both a few hundred lines in `src/`.

```
build.bat
```

The result is `bin\drp.exe`, a single file you can copy anywhere.

## Adding a game

A game is supported once somebody finds the handful of addresses in its memory
that name the level and the mode. That is the whole job; the rest of the program
does not change.

1. Find the addresses. Savestates are the easiest way in: a `.p2s` is a zip
   whose `eeMemory.bin` is the full 32 MB of PS2 RAM, so you can diff two
   savestates taken in different levels and look for a fixed address whose text
   changed. Rainbow Six 3's level name turned out to be a plain `char[]` at
   `0x006B95E0`.
2. Add a `namespace` in `src/games.hpp` with a `Read()` that fills in
   `Presence::mode`, `::map` and `::session`.
3. Add one line to `PROVIDERS` at the bottom of that file.

`drp.exe scan-menu` is a worked example of the other half of the problem: finding which
menu screen is on show. Rainbow Six 3 loads its whole front end at once — 57
pages — so the page list cannot say which one you are looking at. The mode
prints two things as you move around:

* `ptr 0x…  -> P_Something` — a global that points at a page name and changed.
* `page P_Something changed at +60 +61 +62` — a page object whose own fields are
  moving. Only the page being updated ticks, so this is the likely tell.

Run it, walk through the menus, and whichever page lights up on each screen is
the one to key off. `--from` and `--to` move the pointer sweep elsewhere; the
default covers the overlay's globals.

## Credit and licence

MIT.

Discord names a card after whichever application connects to it, so `drp`
reconnects as a different one when you switch games. PCSX2 gets PCSX2's own
application, which is the honest answer there. Mercenaries deliberately has no
default — it is not PCSX2 and should not claim to be — so it wants one of yours.
