// SPDX-License-Identifier: MIT
//
// Mercenaries: Playground of Destruction — the PC port at D:\MercenariesPC.
//
// The port is a 32-bit build of the game's own code (Pandemic's Red/RetroStrike
// engine) and it was compiled with RTTI, so all 1,197 of its class names survive
// in the image. That is the lever this file pulls: a live object of class C is
// any address whose first word is (module base + C's vtable RVA). The game's
// state machines are found that way, with no map of its globals, and it survives
// ASLR because the base is read at run time.
//
// The RVAs were read out of Mercenaries.exe
// (sha1 e51d9a0276588eff9aaa3a8b8bdf6fbeb2622cf6) by walking each RTTI type
// descriptor to its complete-object locator and on to the vtable naming it, and
// they are specific to that build. Everything else is found by signature, so it
// should survive a rebuild; `probe` reports what it could and could not see.
//
// The layouts come from the game's own source, which the RTTI names match
// one-for-one.

#pragma once

#include "procmem.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mercs
{
	constexpr const char* EXE_NAME = "Mercenaries.exe";

	struct VtableEntry
	{
		const char* name;
		uint32_t rva;
	};

	// ------------------------------------------------------------------
	// State machines. Each state is its own class, so which one a machine is
	// running can be read straight off the vtable of the object it points at.
	// ------------------------------------------------------------------

	// RsMainStateGame's sub-states: the top-level "what is on screen".
	inline constexpr VtableEntry GAME_STATES[] = {
		{"Playing", 0x366CE8},              // StatePlay
		{"Paused", 0x366CB8},               // StatePause
		{"In the DataPod", 0x366CA0},       // StateDataPod
		{"At the desktop", 0x366C88},       // StateDesktop
		{"Watching a cutscene", 0x366C70},  // StateCinematic
		{"Loading", 0x366C58},              // StateForcedLoad
		{"Down", 0x366C40},                 // StateMissionDeath
		{"Down", 0x366C28},                 // StateNonMissionDeath
		{"Respawning", 0x366C10},           // StateRespawn
		{"Mission over", 0x366BF4},         // StateMissionWonOrLost
		{"Controller unplugged", 0x366BDC}, // StateControllerDisconnected
		{"Reading a message", 0x366BC4},    // StateMessageWindow
		{"Calling in support", 0x366BAC},   // StateSupportMenu
		{"Loading", 0x366B94},              // StatePlayerModelLoad
	};

	// RsAiPlayer's sub-states: on foot, driving, or manning a gun.
	inline constexpr VtableEntry PLAYER_STATES[] = {
		{"On foot", 0x3499C4},     // StatePlayerNormal
		{"Surviving", 0x3499AC},   // StatePlayerSurvivor
		{"Boarding", 0x349994},    // StatePlayerEnterVehicle
		{"Dismounting", 0x34997C}, // StatePlayerExitVehicle
		{"Driving", 0x349964},     // StatePlayerDriver
		{"On the gun", 0x34994C},  // StatePlayerGunner
	};

	// RsActorVehicleHumanPlayer's sub-states: what the merc's hands are doing.
	// Takedown and Verify are the Deck of 52 capture.
	inline constexpr VtableEntry ACTION_STATES[] = {
		{"", 0x3462D4},                     // NormalStatePlayer - nothing to say
		{"Down", 0x3462BC},                 // DeadStatePlayer
		{"Taking down a target", 0x3462A0}, // TakedownState
		{"Verifying a target", 0x346284},   // VerifyState
		{"Planting charges", 0x34626C},     // PlantChargeState
	};

	inline constexpr uint32_t RVA_STATE_GAME = 0x366B7C;    // RsMainStateGame
	inline constexpr uint32_t RVA_STATE_SHELL = 0x36A5C0;   // RsMainStateShell2
	inline constexpr uint32_t RVA_AI_PLAYER = 0x3499DC;     // RsAiPlayer
	inline constexpr uint32_t RVA_PLAYER_ACTOR = 0x3462EC;  // RsActorVehicleHumanPlayer
	inline constexpr uint32_t RVA_WORLD = 0x36231C;         // RsWorld
	inline constexpr uint32_t RVA_OBJECTIVE_TRAY = 0x35577C;// RsHudObjectiveTray
	inline constexpr uint32_t RVA_CHALLENGE_MARKER = 0x34D75C;

	// ------------------------------------------------------------------
	// The mercs. RsGameOptions keeps the choice as a CRC of the ODF template
	// name, set in the character-select screen and restored from a save, so it
	// is right in the menus as well as in play.
	// ------------------------------------------------------------------

	struct MercName
	{
		uint32_t crc;
		const char* name;
		const char* badge; // Discord art-asset key for this merc's ID photo
	};

	// The badges come out of the game's own `fe_char_photos` sheet, which is the
	// character-select screen's three ID photos; the two unlockables are not on
	// it, so they get no badge.
	inline constexpr MercName MERCS[] = {
		{0xb5d9d774, "Chris Jacobs", "merc_chris"},       // template_player_american
		{0x707e723d, "Jennifer Mui", "merc_jennifer"},    // template_player_british
		{0xc3c67997, "Mattias Nilsson", "merc_mattias"},  // template_player_swedish
		{0xe8e4793e, "Han Solo", ""},                     // template_player_hansolo
		{0xd46eb4c7, "Indiana Jones", ""},                // template_player_indianajones
	};

	inline bool IsMercCrc(uint32_t v)
	{
		for (const MercName& m : MERCS)
		{
			if (m.crc == v)
				return true;
		}
		return false;
	}

	inline const char* MercNameFor(uint32_t v)
	{
		for (const MercName& m : MERCS)
		{
			if (m.crc == v)
				return m.name;
		}
		return nullptr;
	}

	inline const char* MercBadgeFor(const std::string& name)
	{
		for (const MercName& m : MERCS)
		{
			if (name == m.name)
				return m.badge;
		}
		return "";
	}


	// ------------------------------------------------------------------
	// Levels. The engine carries the loaded level as an FNV-1a hash of its short
	// name, case-folded by OR-ing 0x20 into each byte (Pebble's PblHash). The
	// hashes below were computed with that function and checked against the two
	// the game's own source hardcodes, "swn" and "aspade".
	//
	// The game has no place names of its own: RsActorLocation is a point marker
	// with no containment test, and nothing anywhere maps these short names to
	// regions. The labels on the right are therefore mine, read off the cities
	// the mission text names in each map. Edit freely.
	// ------------------------------------------------------------------

	struct LevelName
	{
		uint32_t hash;
		const char* id;
		const char* label;
	};

	inline constexpr LevelName LEVELS[] = {
		{0x4A364A32, "nw", "North West Korea"},
		{0x4A5220AF, "sw", "South West Korea"},
		{0xC0498FD3, "swn", "South West Korea, after dark"},
		{0x47AA1A09, "aclubs", "the Ace of Clubs"},
		{0x30BCF6D5, "admnd", "the Ace of Diamonds"},
		{0x9D290D50, "aheart", "the Ace of Hearts"},
		{0x400F92A5, "aspade", "the Ace of Spades"},
		{0x633905C5, "e3n", "a demo map"},
		{0x763923AE, "e3s", "a demo map"},
		{0x1D1D09F9, "cddnw", "North West Korea"},
		{0xE02DEBB6, "shl", ""},
		{0xDD5E607E, "core", ""},
	};

	inline const LevelName* LevelFor(uint32_t hash)
	{
		for (const LevelName& l : LEVELS)
		{
			if (l.hash == hash)
				return &l;
		}
		return nullptr;
	}

	/// The level the game is in, as its name hash, or 0.
	///
	/// Three separate globals hold it at once (RsMain's uiNextLevel,
	/// RedVirtualDisk::_uiCurrentMap and RsGameOptions::_uiStartLevel), and the
	/// level table holds each level's hash exactly once. So the hash that turns
	/// up most often in the module's writable data is the one that is loaded.
	uint32_t CurrentLevelHash(const procmem::Process& proc);

	// ------------------------------------------------------------------
	// The game's own data files, so contracts are described in the game's words.
	// ------------------------------------------------------------------

	struct Mission
	{
		std::string id;          // "NW_Mafia1"
		std::string title;       // localised title
		std::string description; // localised one-liner
		std::string chapter;     // "CHAPTER 3"
		std::string faction;     // "MAFIA"
		std::string map;         // "NW"
		int number = 0;
	};

	std::map<std::string, std::string> LoadLocalisation(const std::string& path);
	std::map<std::string, Mission> LoadMissions(const std::string& data_root);

	// ------------------------------------------------------------------
	// Reading the live game
	// ------------------------------------------------------------------

	/// Which of `table`'s states the object at `object` is, or nullptr.
	const char* ClassifyState(const procmem::Process& proc, uintptr_t object,
		const VtableEntry* table, size_t count);

	/// The state a machine is currently running: the first pointer inside the
	/// machine object that points at one of its own state objects.
	const char* CurrentState(const procmem::Process& proc, uintptr_t machine,
		const VtableEntry* table, size_t count, uint32_t window = 0x600);

	/// The three lines of HUD objective text, already localised by the game and
	/// already formatted, with the engine's `[button]` glyph tokens removed.
	/// `tray` is the RsHudObjectiveTray object; `buffer_offset` is cached across
	/// calls because finding it means probing for the 3x512 char block.
	std::vector<std::string> ObjectiveLines(const procmem::Process& proc, uintptr_t tray,
		uint32_t& buffer_offset);

	/// RsGameOptions::_kInstance, found by its adjacent
	/// `_uiDefaultCharacter` / `_uiCharacter` pair of merc CRCs.
	uintptr_t FindGameOptions(const procmem::Process& proc);

	/// RsMissionDataManager::_CurrentMissionData, found by its `char _acName[64]`
	/// holding one of the ids from mission.ini with a localised title 64 bytes
	/// later. Returns 0 when no contract has been started this session.
	uintptr_t FindCurrentMissionData(const procmem::Process& proc,
		const std::map<std::string, Mission>& missions);

	/// The contract id that blob names right now, lower-cased, or empty.
	std::string CurrentMissionId(const procmem::Process& proc, uintptr_t mission_data);
} // namespace mercs
