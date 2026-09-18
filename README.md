# drp

Discord Rich Presence that says what you are **doing**, not just which game is
open. Two small programs, one per host:

| exe | watches | how |
|---|---|---|
| `pcsx2-drp.exe` | any PCSX2 | over PCSX2's built-in PINE server |
| `mercs-drp.exe` | the Mercenaries: Playground of Destruction PC port | reads the game process's memory |

Neither is a fork or a patch, so both work with a binary you did not build and
can be handed to a friend as they are.

```
PCSX2
Tom Clancy's Rainbow Six 3 • Terrorist Hunt
Alpine Village A • Split screen
01:12 elapsed
```

instead of

```
PCSX2
Tom Clancy's Rainbow Six 3
01:12 elapsed
```

## How it works

PCSX2 has a built-in IPC server called PINE. With PINE switched on, a program on
the same machine can ask the emulator for the disc serial and read the emulated
PS2's memory. `pcsx2-drp` does exactly that once a second: it looks up a few
known addresses for the game you are playing, works out the mode, the map and
whether it is single player, split screen or online, and pushes that to Discord
over Discord's own local socket.

Nothing is written to the emulator or the game. It only reads.

## Setup — PCSX2

1. In PCSX2: **Settings → Advanced → Enable PINE** (leave the slot at 28011).
2. In PCSX2: **Settings → Interface → Enable Discord Presence → off.**
   PCSX2's own presence and this tool would otherwise both push one and fight.
3. Run `pcsx2-drp.exe` and leave it in the background. Start it before or after
   PCSX2, in either order; it reconnects on its own.

There is no installer and no config file. If you run PCSX2 on a different PINE
slot, pass `--slot N`.

```
pcsx2-drp.exe                     watch PCSX2, update Discord
pcsx2-drp.exe --slot 28012        a different PINE slot
pcsx2-drp.exe --quiet             no console chatter
pcsx2-drp.exe scan-menu           diagnostic, see "Adding a game"
```

The console prints every presence change, so if something reads wrong you can
see exactly what it thought was going on.

## Setup — Mercenaries

This is a PC game, not an emulator, so there is nothing to switch on in the game
itself. Two steps:

1. **Make a Discord application**, because Discord names the card after whatever
   application connects to it. Go to
   <https://discord.com/developers/applications>, press New Application, and
   call it exactly what you want people to read — "Mercenaries: Playground of
   Destruction". Copy its Application ID.
   Optionally, under Rich Presence → Art Assets, upload a cover image and note
   the name you gave it.
2. Save the id in a file called `mercs-drp-client-id.txt` next to
   `mercs-drp.exe`. (Or pass `--client-id`, or set `MERCS_DRP_CLIENT_ID`. If you
   uploaded artwork, add `--image <asset name>`.)

Then run `mercs-drp.exe` and leave it in the background. It attaches when the
game starts and lets go when it exits.

Without an id it still runs and prints what it *would* show, so you can see it
working before bothering with Discord — it simply pushes nothing.

The port ships no RTTI-stripped build, which is what makes this possible: every
class name survives in `Mercenaries.exe`, so the tool finds the game's own state
machines by their vtables rather than by hardcoded addresses, and reads (never
writes) their current state. `mercs-drp.exe probe` prints everything it can see,
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

The result is `bin\pcsx2-drp.exe` and `bin\mercs-drp.exe`, single files you can
copy anywhere.

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

`scan-menu` is a worked example of the other half of the problem: finding which
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

`pcsx2-drp` connects as PCSX2's own Discord application by default, which is what
makes its card say "PCSX2" and carry PCSX2's artwork — correct there, since PCSX2
is what is running. `mercs-drp` deliberately has no default: Mercenaries is not
PCSX2 and should not claim to be, so it wants an application of your own. Both
take `--client-id`.
