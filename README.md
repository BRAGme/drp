# pcsx2-drp

Discord Rich Presence for PCSX2 that says what you are **doing**, not just which
game is open.

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

It is a separate little program, not a PCSX2 fork, so it works with **any**
PCSX2 — an official release, a nightly, or somebody's fork — and you can hand
the exe to a friend without asking them to change emulator.

## How it works

PCSX2 has a built-in IPC server called PINE. With PINE switched on, a program on
the same machine can ask the emulator for the disc serial and read the emulated
PS2's memory. `pcsx2-drp` does exactly that once a second: it looks up a few
known addresses for the game you are playing, works out the mode, the map and
whether it is single player, split screen or online, and pushes that to Discord
over Discord's own local socket.

Nothing is written to the emulator or the game. It only reads.

## Setup

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

## Supported games

| Disc | Serial | What it can tell |
|---|---|---|
| Tom Clancy's Rainbow Six 3 | SLUS-20883 | game mode (Campaign, Practice, Terrorist Hunt, the co-op and split-screen variants, the adversarial modes), the map and its A/B half, and whether it is single player, split screen or online. In the menus it says whether you are in the split-screen front end. |

Everything else falls back to the game's name, which is what PCSX2 shows today,
so running the tool never makes a game's presence worse.

## Building

Needs Visual Studio with the C++ workload. No third-party libraries — the PINE
client and the Discord IPC client are both a few hundred lines in `src/`.

```
build.bat
```

The result is `bin\pcsx2-drp.exe`, a single file you can copy anywhere.

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

MIT. The PINE protocol and the default Discord application id belong to the
PCSX2 project; using their application id is what makes the card say "PCSX2" and
carry PCSX2's artwork. Pass `--client-id` to use your own Discord application
instead.
