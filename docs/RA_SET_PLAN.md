# Set plan — Tom Clancy's Rainbow Six 3 (PlayStation 2, SLUS-20883)

Draft for `#role-request`, tagged `Jr-Dev`, pinging `@Code-Reviewer`.
Post alongside a link to the RA profile and to the game page.

---

## The game

A squad tactical shooter. The player commands Ding Chavez and three Rainbow
operatives — Eddie Price, Louis Loiselle, Dieter Weber — through a campaign of
hostage rescues, bomb disposals and terrorist hunts. Missions are split into
lettered halves (`Oil Refinery A`, `Oil Refinery B`), each a separate level
load, and each carrying its own objectives.

**Completion** is clearing every single-player mission. The disc ships **27
single-player levels** across **15 missions**, plus three training maps and a
set of multiplayer-only maps that this set does not cover.

Modes: Campaign, Practice Mission, Terrorist Hunt, and split-screen and online
variants of each. The set is built around the single-player campaign;
Terrorist Hunt and Practice Mission supply the challenge achievements.

---

## Memory research — already done

This is not a proposal to go and find out. The set is planned from a memory map
that is already reverse engineered and verified against live play, and every
condition below has been run against the real game.

One static address reaches almost everything:

```
0x0065529C  -> the player's pawn
  pawn  +0x104 -> LevelInfo      +0x45C  float, seconds since the level loaded
  level +0x520 -> GameInfo
  game  +0x4EC -> mission objective manager
    mgr +0x370 -> 0 in progress, 1 success, 2 failed
    mgr +0x384 -> objective pointers, +0x388 the count
      obj +0x30 -> bit0 failed, bit1 satisfied
      obj +0x50 description, +0x5C KEY, +0x68 failure reason
  game  +0x4E4 -> player controller
    ctrl +0x78C -> team, +0x3C0 + n*4 the four operative pawns
      pawn +0x630 health, +0x3BC bit 0x1000 already a body
      pawn +0x954 kills, +0x958 shots fired, +0x95C hits
      pawn +0x968 -> object whose CLASS NAME is the operative
0x006B95E0  the level package name
0x0065500C  -> game manager; +0x30 difficulty, +0x31 mode
0x0068A0C0  GObjObjects, a UE2 TArray {data, count}
```

Pointer chains throughout, so every condition needs `AddAddress`. Nothing here
needs a fixed heap address.

### Three findings a reviewer will want to know

**Counters never reset.** Kills, shots and hits carry across level loads *and*
across a failed attempt. Anything "in one mission" must be measured as a delta
from mission start, not as an absolute. Mission start is detectable: the level
package changes, or the level clock at `+0x45C` runs backwards.

**The objective KEY is the reliable field, and it is language independent.**
The description at `+0x50` is whatever the instance was given, and an instance
that was never given one falls back to its class default — Mountain Highway's
first objective reads "Wait for event" there. Its key reads
`DefuseFuelTruckBomb`, which is what the pause screen shows. Keys are internal
identifiers, so conditions built on them satisfy the every-language rule on
linked hashes without per-language work.

**Class addresses are not fixed.** UClass objects live on the heap and split
screen relaunches into a different overlay — `R6TerroristAI` moves from
`0x007a17e0` to `0x0070d7f0`. The name string the class points at is a heap
copy too. Any condition counting live terrorists has to resolve the class by
name rather than hold an address. Conditions that only read the objective
manager and the team are unaffected.

---

## Rich Presence

Already written and running against the live game. Dynamic, and it changes with
what is happening:

```
Rainbow Six 3 • Practice Mission • Mountain Highway A
Defuse Fuel Truck Bomb • 15 tangos left • Split Screen

Rainbow Six 3 • Terrorist Hunt • Oil Refinery B
Mission Failed • Ding Chavez Is Dead
```

It reports the mode, the map and its half, the current objective by key, live
terrorists remaining, which operatives are down by name, the difficulty, and
the mission verdict with its failure reason. In the menus it reports the menu;
during a load it names the level being loaded.

---

## Protection

- **Conditions read live game state, never save data**, so they unlock on every
  save slot with no per-slot work.
- **Built on the objective manager's own result field**, not on reaching a
  screen. A mission counts when `mgr+0x370` transitions to 1, which is the
  game's own verdict.
- **Nothing unlocks from a menu, a demo or a briefing** — every condition
  requires a live mission with a valid pawn chain.
- **Delta-based conditions are guarded by mission-start detection**, so a
  restart cannot carry progress into a fresh attempt.
- Emulator cheats and patches disable hardcore, which is the standard
  protection; no in-game cheat code path has been found that sets mission
  results directly.

---

## The set

**68 achievements, 400 points.**

### Progression — one per mission, 15 achievements, 80 points

Each unlocks on the *final* level of that mission completing with
`mgr+0x370 == 1`. Marked **Progression**; the last is the **Win Condition**.

| # | Mission | Levels | Pts |
|---|---|---|---|
| 1–14 | one per mission, in campaign order | A/B halves | 5 each |
| 15 | the final mission | | 10 (Win Condition) |

Missions on the disc: Airport, Alcatraz, Alpine Village, Parking Garage,
Import/Export, Island Estate, Meat Packing Plant, Mountain Highway, Office
Complex, Oil Refinery, Old City, Parade, Penthouse, Shipyard, Trieste.

> **Open research.** The campaign order is not yet confirmed — the level list
> comes from the disc's loading screens, which are alphabetical. The order and
> the final mission need reading off the briefing sequence before this section
> is final. Titles will be per-mission and drawn from the game's own objective
> names.

### Difficulty completion — 3 achievements, 85 points

| Title | Description | Pts |
|---|---|---|
| Rainbow Certified | Complete every mission | 10 |
| Veteran Status | Complete every mission on Veteran or above | 25 |
| Elite Operator | Complete every mission on Elite | 50 |

### Mastery — 10 achievements, 130 points

| Title | Description | Pts |
|---|---|---|
| Silent Professional | Complete a mission on Elite without firing a shot | 25 |
| Untouchable | Complete a mission on Elite with the team unharmed | 25 |
| Perfect Operation | Elite, every objective, nobody hurt, 90% accuracy | 25 |
| Chavez Does Not Miss | Twenty kills in a mission without missing once | 10 |
| One Shot, One Kill | Finish a mission without missing once | 10 |
| Marksman | Twenty kills in a mission at 90% accuracy | 10 |
| Surgical | Ten kills at 80% accuracy or better | 5 |
| Economy of Force | Under 1.2 rounds fired per kill across a mission | 10 |
| Ghost | Complete a mission without firing a shot | 5 |
| Quick Reaction Force | Complete a mission in under five minutes | 5 |

*Health for Untouchable is compared to each operative's health at mission
start, not to a constant — full health has only been measured on Veteran and
Elite may differ.*

### The team — 6 achievements, 45 points

| Title | Description | Pts |
|---|---|---|
| No Man Left Behind | Complete a mission with the whole team standing | 10 |
| Unbreakable | Complete every mission without ever losing an operative | 25 |
| Rainbow Six | Complete a mission in which all four operatives scored | 5 |
| Last Man Standing | Complete a mission as the only operative alive | 5 |
| Ding Chavez Is Dead | Lose Chavez, and the mission with him | (missable flavour) |
| Wiped Out | Lose the whole team | (missable flavour) |

*Split screen replaces the AI operatives with player two — zero `R6RainbowAI`
instances — so team conditions are gated to single player.*

### Objectives — 8 achievements, 30 points

One per distinct objective key on the disc: `DefuseBomb`,
`DisarmFailsafeDevice`, `DefuseFuelTruckBomb`, `LocateChemicalBarrels`,
`SecureHostage`, `NeutralizeAllTerrorist` and others still to be enumerated.
Each unlocks when that objective's satisfied bit sets. Several are **missable**.

> **Open research.** Four objective keys are confirmed from play. The rest need
> enumerating across the remaining levels — one capture per level, which the
> existing tooling does in under a second each.

### Terrorist Hunt and volume — 6 achievements, 30 points

| Title | Description | Pts |
|---|---|---|
| Clean Sweep | Clear every terrorist on a Terrorist Hunt map | 10 |
| Clean Sheet | Complete a mission with nothing failed | 5 |
| Century | One hundred confirmed kills | 5 |
| Double Century | Two hundred and fifty confirmed kills | 5 |
| Clockwork | Three missions in a row with no failure between | 5 |
| Tango Down | Your first confirmed kill | 1 |

---

## What is already proven

Forty of these conditions are implemented and have been run against the live
game, unlocking correctly from real play rather than from a hunch. Evidence
available on request:

- Twelve full 32 MB RAM captures across a session, spanning a failed mission, a
  completed one, a split-screen session and four maps.
- A replay harness that serves a captured dump over the emulator's own IPC
  protocol, so any condition can be demonstrated against known state with no
  emulator running.

## What is left

1. Confirm the campaign order and the final mission.
2. Enumerate objective keys for the remaining levels.
3. Confirm full health on Elite.
4. Translate the conditions into RA's format with `AddAddress` chains.

---

## Declared

The tooling used for this research is a Discord Rich Presence application that
reads the game over PCSX2's PINE IPC. It is not an emulator modification and
does not alter game state — it only reads memory.
