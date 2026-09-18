drp - Discord Rich Presence for Mercenaries: Playground of Destruction (PC port)
================================================================================

Instead of your Discord just saying you are playing something, it says what you
are actually doing:

    Mercenaries - POD
    Jennifer Mui - On contract: Hunt down Sergei for the Mafia
    Eliminate the renegades - Driving - in North West Korea

It knows which merc you picked (and shows their ID photo), whether you are on a
contract or free roaming, the contract's own name and who hired you, the current
objective off your HUD, whether you are on foot, driving or on a gun, which map
you are on, and the moments worth mentioning - verifying a Deck of 52 target,
planting charges, sitting in the DataPod.


HOW TO USE IT
-------------

1. Unzip anywhere you like.
2. Run drp.exe.
3. Play.

That is all. There is nothing to configure, no account to make, and it does not
need to live next to the game - it finds the game and the game's data on its own.

To have it start with Windows so you never think about it again, open a terminal
where you unzipped it and run:

    drp.exe --startup on

and to undo that:

    drp.exe --startup off

In PowerShell a full path needs the call operator, or it fails with
"Unexpected token":

    & "C:\path\to\drp.exe" --startup on

It has no window. What it is doing goes to drp.log next to the exe. Run it from a
terminal instead and it prints there.


WHAT IT DOES, AND WHAT IT DOES NOT DO
-------------------------------------

It reads the game's memory while you play, the same way a speedrun timer or an
overlay does, and sends two lines of text to Discord. It only ever READS. It does
not write to the game, patch anything, touch your saves, or go near the network
apart from Discord's own local socket.

Two consequences worth knowing:

* Windows may warn that the publisher is unknown, because the exe is not code
  signed. Some antivirus is also suspicious of anything that reads another
  program's memory. That is the category of tool this is.
* The addresses it reads are specific to one build of the game. On a different
  build it says so in drp.log and falls back to just showing the game's name,
  rather than making things up.

It also does PCSX2, if you use it: turn on Settings > Advanced > Enable PINE and
turn off Settings > Interface > Enable Discord Presence. Rainbow Six 3 is the
disc it knows in detail so far.


TROUBLESHOOTING
---------------

Nothing shows up on Discord
    Check drp.exe is actually running. It is the usual answer - and --startup on
    only takes effect from your next login, so it does not come back by itself
    after you close it.

It says the game's name but nothing else
    Your build of the game is not the one the addresses came from. drp.log says
    so explicitly.

The card has no artwork
    Discord caches its asset list; restarting the Discord client usually fixes it.


drp.ini
-------

Written next to the exe on first run, with every setting commented out and
showing its default. You do not need to touch it. It is there if you want to
point at a different data folder or use a Discord application of your own.
