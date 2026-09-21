// SPDX-License-Identifier: MIT
//
// Per-game presence providers.
//
// A provider reads a handful of known globals out of the emulated PS2's memory
// and says what the player is doing. Adding a game means finding its addresses
// and adding one entry to PROVIDERS at the bottom.

#pragma once

#include "pine.hpp"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace games
{
	struct Presence
	{
		std::string mode;     // "Terrorist Hunt"
		std::string map;      // "Alpine Village A"
		std::string session;  // "Split Screen" / "Single Player" / "Online"
		std::string activity; // "Gearing up", used instead of a map in the menus

		/// Seconds the current level has been running, 0 when unknown. Drives
		/// the elapsed timer on the card.
		int64_t elapsed = 0;

		bool empty() const { return mode.empty() && map.empty() && session.empty() && activity.empty(); }
	};

	using ReadFn = bool (*)(pine::Client& mem, Presence& out);

	struct Provider
	{
		const char* serial;
		/// What to call the game. Preferred over the title PCSX2 reports,
		/// which is the disc's full name - a third of line one spent on
		/// "Tom Clancy's" for no information a reader does not already have.
		const char* name;
		ReadFn read;
	};

	// -----------------------------------------------------------------------
	// helpers
	// -----------------------------------------------------------------------

	inline char UpperAscii(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }
	inline char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

	inline std::string ToUpper(std::string s)
	{
		for (char& c : s)
			c = UpperAscii(c);
		return s;
	}

	inline bool EndsWith(std::string_view s, std::string_view suffix)
	{
		return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	/// "MOUNTAIN_HIGHWAY" -> "Mountain Highway", for any token with no hand
	/// written name, so a modded or unlisted level still reads sensibly.
	inline std::string PrettifyToken(std::string_view token)
	{
		std::string out;
		bool start_of_word = true;
		for (const char c : token)
		{
			if (c == '_' || c == '-')
			{
				if (!out.empty() && out.back() != ' ')
					out.push_back(' ');
				start_of_word = true;
				continue;
			}
			out.push_back(start_of_word ? UpperAscii(c) : LowerAscii(c));
			start_of_word = false;
		}
		while (!out.empty() && out.back() == ' ')
			out.pop_back();
		return out;
	}

	// -----------------------------------------------------------------------
	// Tom Clancy's Rainbow Six 3 (PS2, SLUS-20883)
	//
	// Addresses were found by static analysis of the stock SP.SOZ overlay
	// (sha1 e9bb12138a1e69d551ac9f6b958114e5b2e830da) and then confirmed by
	// reading them back out of five savestates covering campaign, practice,
	// terrorist hunt and split-screen terrorist hunt.
	//
	//   0x006B95E0  char[]  package name of the level loaded last, e.g.
	//                       "ALPINES_AOFF" / "PARADE_B_SS" / "PRISON_MP_S".
	//                       Written by the level-load path at 0x002F7C08 and
	//                       read back as a char at 0x001CF334, which is what
	//                       fixes it as a buffer rather than a pointer.
	//   0x0065500C  ptr     game manager singleton; the overlay reaches it as
	//                       *(gp-0x66E4) with gp = 0x0065B6F0.
	//     +0x31     u8      game mode, the enum below.
	//     +0x90     struct  { char* data; u32 count; u32 max; } holding the game
	//                       type class name, "R6Game.R6TerroristHuntGame" etc.
	//   0x006546A0  u32     g_bSplitScreen. Set at overlay boot from -ssc/-sst,
	//                       so it is already true while the player is still in
	//                       the split-screen front end.
	// -----------------------------------------------------------------------

	namespace r6three
	{
		constexpr uint32_t ADDR_LEVEL_PACKAGE = 0x006B95E0;
		constexpr uint32_t ADDR_GAME_MANAGER = 0x0065500C;
		constexpr uint32_t OFFSET_DIFFICULTY = 0x30;
		constexpr uint32_t OFFSET_GAME_MODE = 0x31;
		constexpr uint32_t OFFSET_GAME_TYPE = 0x90;
		constexpr uint32_t ADDR_SPLIT_SCREEN = 0x006546A0;

		struct ModeInfo
		{
			const char* name;
			const char* session;
		};

		// Order comes from the URL builder's jump table at 0x005EC160, which
		// maps each value to the R6Game class it loads. 0 means "not set".
		constexpr ModeInfo MODES[] = {
			{nullptr, nullptr},                  // 0
			{"Campaign", "Single Player"},       // 1  R6StoryModeGame
			{"Practice Mission", "Single Player"},       // 2  R6PracticeModeGame
			{"Campaign Co-op", "Online"},        // 3  R6CoopStoryModeGame
			{"Terrorist Hunt", "Single Player"}, // 4  R6TerroristHuntGame
			{"Terrorist Hunt Co-op", "Online"},  // 5  R6CoopTerroristHuntGame
			{"Deathmatch", "Online"},            // 6  R6DeathMatch
			{"Team Deathmatch", "Online"},       // 7  R6TeamDeathMatchGame
			{"Sharpshooter", "Online"},          // 8  R6SharpShooterGame
			{"Sharpshooter", "Split Screen"},    // 9  R6SharpShooterGameForSplitScreen
			{"Terrorist Hunt", "Split Screen"},  // 10 R6TerroristHuntGameForSplitScreen
			{"Practice Mission", "Split Screen"},        // 11 R6PracticeModeGameForSplitScreen
			{"No Rules", "Online"},              // 12 R6NoRules
		};

		struct GameTypeInfo
		{
			const char* class_name;
			uint8_t mode;
		};

		// The class name says the same thing as the mode byte and the overlay
		// keeps both, so reading it as well covers a path that only sets one.
		constexpr GameTypeInfo GAME_TYPES[] = {
			{"R6StoryModeGame", 1},
			{"R6PracticeModeGame", 2},
			{"R6CoopStoryModeGame", 3},
			{"R6TerroristHuntGame", 4},
			{"R6CoopTerroristHuntGame", 5},
			{"R6DeathMatch", 6},
			{"R6TeamDeathMatchGame", 7},
			{"R6SharpShooterGame", 8},
			{"R6SharpShooterGameForSplitScreen", 9},
			{"R6TerroristHuntGameForSplitScreen", 10},
			{"R6PracticeModeGameForSplitScreen", 11},
			{"R6NoRules", 12},
		};

		struct MapName
		{
			const char* token;
			const char* name;
		};

		// The level tokens that ship on the disc. The names on the right are
		// editorial - the PS2 disc keeps its menu text inside the cooked .LIN
		// packages rather than in a readable .int - so edit freely. Anything not
		// listed falls back to the token with its underscores turned to spaces.
		constexpr MapName MAP_NAMES[] = {
			{"AIRPORT", "Airport"},
			{"AIRPORT2", "Airport"},
			{"ALCATRAZ", "Alcatraz"},
			{"ALPINES", "Alpine Village"},
			{"CROSSFIRE", "Crossfire"},
			{"GARAGE", "Parking Garage"},
			{"IMPORT_EXPORT", "Import/Export"},
			{"ISLAND", "Island Estate"},
			{"MEATPACKING", "Meat Packing Plant"},
			{"MOUNTAIN_HIGHWAY", "Mountain Highway"},
			{"OFFICE_COMPLEX", "Office Complex"},
			{"OIL_REFINERY", "Oil Refinery"},
			{"OLDCITY", "Old City"},
			{"PARADE", "Parade"},
			{"PENTHOUSE", "Penthouse"},
			{"PRESIDIO", "Presidio"},
			{"PRISON", "Prison"},
			{"SANDSTORM", "Sandstorm"},
			{"SHIPYARD", "Shipyard"},
			{"STREETS", "Streets"},
			{"TRAINING_BASICS", "Training: Basics"},
			{"TRAINING_SHOOTING", "Training: Shooting Range"},
			{"TRAINING_TEAM", "Training: Team"},
			{"TRIESTE", "Trieste"},
		};

		struct SuffixInfo
		{
			const char* suffix;
			const char* session; // nullptr = says nothing about the session
		};

		// The level-load path builds the package name as map + suffix between
		// 0x002F7B34 and 0x002F7C08.
		constexpr SuffixInfo SUFFIXES[] = {
			{"_MULTI", "Online"},
			{"_SS", "Split Screen"},
			{"OFF", nullptr}, // offline: the mode decides who is playing
			{"_S", "Online"}, // hosting
			{"_C", "Online"}, // joined
			{"_D", "Online"}, // dedicated
		};

		// -------------------------------------------------------------------------
		// Tangos left
		//
		// There is no "terrorists remaining" integer anywhere in memory - the
		// whole of RAM was searched across six snapshots taken around known
		// kills and nothing tracks it. The game counts live terrorist
		// controllers, so that is what this does too.
		//
		// The route is all fixed addresses, which matters because every heap
		// address moves on a level load:
		//
		//   0x0068a0c0  GObjObjects, a UE2 TArray {data, count}
		//   object+0x24 its UClass, +0x28 its name
		//   0x007a17e0  the R6TerroristAI UClass, verified by name before use
		//   0x00686dd8  the R6TerroristMgr, whose +0x10c is the level's total
		//
		// Measured against a live Oil Refinery hunt: 17, 17, 17, 17, 12, 12
		// across six snapshots, matching the kills reported at the time.
		// -------------------------------------------------------------------------

		constexpr uint32_t ADDR_OBJECT_TABLE = 0x0068A0C0;
		constexpr uint32_t CLASS_TERRORIST_AI = 0x007A17E0;
		constexpr uint32_t OFFSET_OBJ_CLASS = 0x24;
		constexpr uint32_t OFFSET_OBJ_NAME = 0x28;
		constexpr uint32_t OFFSET_AI_PAWN = 0x108;     // R6TerroristAI -> its pawn
		constexpr uint32_t OFFSET_PAWN_HEALTH = 0x630; // 0 once it is a corpse
		constexpr uint32_t CLASS_DZONE_WAVE = 0x007A2FE0;
		constexpr uint32_t OFFSET_WAVE_SPAWNED = 0x478;  // m_iNbSpawned
		constexpr uint32_t OFFSET_WAVE_TO_SPAWN = 0x47C; // m_iNbToSpawn

		/// EE pointers come back with the cached-segment bits set; the object
		/// table is indexed by the physical address.
		constexpr uint32_t EE_MASK = 0x01FFFFFF;

		/// True when that address really is the R6TerroristAI class object. The
		/// class address is a constant rather than a search because resolving it
		/// by name would mean reading 25,000 strings, but a constant that has
		/// quietly moved would silently report zero tangos forever - so it is
		/// checked, cheaply, every time.
		inline bool IsTerroristClass(pine::Client& mem, uint32_t class_address)
		{
			uint32_t name_ptr = 0;
			if (!mem.ReadU32(class_address + OFFSET_OBJ_NAME, name_ptr))
				return false;
			return mem.ReadString(name_ptr & EE_MASK, 32) == "R6TerroristAI";
		}

		// -------------------------------------------------------------------------
		// Loading screens
		//
		// 0x00595020 is ShowLoadingScreen(kind, arg); it stores the kind at
		// 0x006F54C4 and the engine busy-waits on that word in four places
		// (0x005939A4, 0x005939E4, 0x00593B34, 0x00593B6C), each yielding until
		// it reads back zero. Ten sites clear it when the load finishes. So
		// non-zero means a loading screen is up; kind 3 is the level one, whose
		// caller composes "loading\lvl\<map>.fbz" from the map name.
		//
		// The destination is at 0x006B9910, a plain lowercase map name written
		// at 0x001BC27C before any level data is read, and later strcpy'd to the
		// package buffer this code normally reads. That ordering is the point:
		// during a load the package name is still the level being left, while
		// 0x006B9910 already names the one being entered.
		//
		// Measured 0 at 0x006F54C4 in all twelve RAM snapshots, none of which
		// caught a load - so this cannot yet produce a false "Loading", but the
		// true case is inferred from the code rather than observed. drp logs the
		// value on change so the next real transition settles it.
		// -------------------------------------------------------------------------

		constexpr uint32_t ADDR_LOADING_KIND = 0x006F54C4;
		constexpr uint32_t ADDR_DESTINATION = 0x006B9910;

		/// A level token, however it is spelled, as the name a player knows.
		/// Deliberately separate from the main path's map lookup: that one also
		/// has to work out the session from the package's suffix, and this is
		/// the only thing a loading screen can tell you.
		inline std::string PrettyLevel(std::string token)
		{
			token = ToUpper(token);
			if (token.empty() || token == "MENU" || token == "MENUOFF" || token == "ENTRY" ||
				token == "COMMON")
				return "Main Menu";

			if (EndsWith(token, "OFF"))
				token.resize(token.size() - 3);
			if (EndsWith(token, "_MP"))
				token.resize(token.size() - 3);

			std::string part;
			if (token.size() > 2 && token[token.size() - 2] == '_' && token.back() >= 'A' &&
				token.back() <= 'Z')
			{
				part = token.back();
				token.resize(token.size() - 2);
			}

			for (const MapName& mn : MAP_NAMES)
			{
				if (token == mn.token)
					return part.empty() ? std::string(mn.name) : (std::string(mn.name) + ' ' + part);
			}

			std::string pretty = PrettifyToken(token);
			if (!part.empty())
				pretty += ' ' + part;
			return pretty;
		}

		/// Kind 4 is the screen that comes up on the way OUT. Measured twice in
		/// one transition: leaving Oil Refinery for the menu it read kind 4 with
		/// the destination still "oil_refinery_b", and leaving the menu for
		/// Shipyard it read kind 4 with the destination still "menu". Both would
		/// have named the level being left. So kind 4 says only that something
		/// is loading; kinds 2 and 3 name it, and were right every time.
		constexpr uint32_t LOADING_KIND_LEAVING = 4;

		/// Where the game is going while a loading screen is up, or empty when it
		/// is not loading.
		///
		/// "Entry" counts as loading in its own right. The kind briefly returns
		/// to 0 between the outgoing screen and the incoming one - one poll, at
		/// 12:53:44 in the trace - and the card flicked back to "In the menus"
		/// mid-load. Entry is the engine's transition level, so it covers that
		/// gap without needing a timer.
		inline std::string ReadLoading(pine::Client& mem, const std::string& package)
		{
			uint32_t kind = 0;
			if (!mem.ReadU32(ADDR_LOADING_KIND, kind) || kind > 8)
				return {};

			const bool in_transition = (kind != 0) || ToUpper(package) == "ENTRY";
			if (!in_transition)
				return {};

			if (kind == LOADING_KIND_LEAVING)
				return "Loading";

			return "Loading " + PrettyLevel(mem.ReadString(ADDR_DESTINATION, 63));
		}

		// -------------------------------------------------------------------------
		// The anchor chain
		//
		// One static address reaches everything worth knowing, without walking
		// the 24,000-entry object table:
		//
		//   0x0065529C   -> the player's pawn
		//   pawn  +0x104 -> LevelInfo        +0x45C float, seconds since level load
		//   level +0x520 -> GameInfo
		//   game  +0x4EC -> R6MissionObjectiveMgr
		//   mgr   +0x370 -> 0 in progress, 1 success, 2 failed
		//   mgr   +0x384 -> objective pointers, +0x388 how many
		//   obj   +0x30  -> bit0 failed, bit1 satisfied
		//   obj   +0x68  -> the failure reason, an FString {char* data, _, int len}
		//
		// The chain runs this way round on purpose: pawn->controller nulls the
		// moment the player dies, which is exactly when the verdict matters.
		//
		// Verified across four snapshots spanning a failed and a completed
		// mission: result 0 while playing, 2 on the failed debrief, 1 after the
		// win, and the failure reason rewritten in place from its authored
		// default "YourTeamWasWipedOut" to "DingChavezIsDead" - which is what
		// the screen said.
		// -------------------------------------------------------------------------

		constexpr uint32_t ADDR_PLAYER_PAWN = 0x0065529C;
		constexpr uint32_t OFF_PAWN_LEVEL = 0x104;
		constexpr uint32_t OFF_LEVEL_SECONDS = 0x45C;
		constexpr uint32_t OFF_LEVEL_GAME = 0x520;
		constexpr uint32_t OFF_GAME_OBJECTIVES = 0x4EC;
		constexpr uint32_t OFF_MGR_RESULT = 0x370;
		constexpr uint32_t OFF_MGR_LIST = 0x384;
		constexpr uint32_t OFF_MGR_COUNT = 0x388;
		constexpr uint32_t OFF_OBJ_FLAGS = 0x30;
		constexpr uint32_t OFF_OBJ_FAILURE = 0x68;
		constexpr uint32_t OBJ_FLAG_FAILED = 0x01;

		/// Follow a pointer, masked and range-checked. 0 for anything that
		/// cannot be an object, so a half-built chain simply stops.
		inline uint32_t Chase(pine::Client& mem, uint32_t at)
		{
			uint32_t value = 0;
			if (!mem.ReadU32(at, value))
				return 0;
			value &= EE_MASK;
			return (value >= 0x00100000 && value < 0x02000000) ? value : 0;
		}

		/// An FString: char* at +0, length at +8.
		inline std::string ReadFString(pine::Client& mem, uint32_t at)
		{
			const uint32_t data = Chase(mem, at);
			uint32_t length = 0;
			if (!data || !mem.ReadU32(at + 8, length) || length == 0 || length > 200)
				return {};
			return mem.ReadString(data, length);
		}

		/// "DingChavezIsDead" -> "Ding Chavez Is Dead". The game stores these as
		/// identifiers; the screen spaces them out, so this does too.
		inline std::string SpaceWords(const std::string& text)
		{
			std::string out;
			for (size_t i = 0; i < text.size(); i++)
			{
				const bool boundary = i > 0 && std::isupper(static_cast<unsigned char>(text[i])) &&
									  !std::isupper(static_cast<unsigned char>(text[i - 1]));
				if (boundary)
					out.push_back(' ');
				out.push_back(text[i]);
			}
			return out;
		}

		/// The mission objective manager, or 0 when there is no mission.
		inline uint32_t ObjectiveMgr(pine::Client& mem)
		{
			const uint32_t pawn = Chase(mem, ADDR_PLAYER_PAWN);
			const uint32_t level = pawn ? Chase(mem, pawn + OFF_PAWN_LEVEL) : 0;
			const uint32_t game = level ? Chase(mem, level + OFF_LEVEL_GAME) : 0;
			return game ? Chase(mem, game + OFF_GAME_OBJECTIVES) : 0;
		}

		/// Seconds since the level loaded, or 0. Note this does NOT reset when a
		/// mission is retried - it is the level's clock, not the mission's.
		inline int64_t ReadMissionSeconds(pine::Client& mem)
		{
			const uint32_t pawn = Chase(mem, ADDR_PLAYER_PAWN);
			const uint32_t level = pawn ? Chase(mem, pawn + OFF_PAWN_LEVEL) : 0;
			uint32_t bits = 0;
			if (!level || !mem.ReadU32(level + OFF_LEVEL_SECONDS, bits))
				return 0;

			float seconds = 0.0f;
			std::memcpy(&seconds, &bits, sizeof(seconds));
			if (!(seconds > 0.0f) || seconds > 360000.0f) // a hundred hours is not a mission
				return 0;
			return static_cast<int64_t>(seconds);
		}

		/// How the mission ended, and why, or empty while it is still running.
		///
		/// This replaced a scrape of the front end's on-screen text, which
		/// flickered: the UI rewrites its text slots every frame, so a read
		/// landed between rewrites and the verdict appeared and vanished every
		/// couple of seconds. The manager's own field does not move.
		inline std::string ReadVerdict(pine::Client& mem)
		{
			const uint32_t mgr = ObjectiveMgr(mem);
			uint32_t result = 0;
			if (!mgr || !mem.ReadU32(mgr + OFF_MGR_RESULT, result) || result == 0 || result > 2)
				return {};

			if (result == 1)
				return "Mission Successful";

			// The failing objective carries the reason, rewritten in place from
			// its authored default to what actually happened.
			std::string reason;
			const uint32_t list = Chase(mem, mgr + OFF_MGR_LIST);
			uint32_t count = 0;
			if (list && mem.ReadU32(mgr + OFF_MGR_COUNT, count) && count <= 32)
			{
				for (uint32_t i = 0; i < count && reason.empty(); i++)
				{
					const uint32_t objective = Chase(mem, list + i * 4);
					uint32_t flags = 0;
					if (!objective || !mem.ReadU32(objective + OFF_OBJ_FLAGS, flags))
						continue;
					if ((flags & OBJ_FLAG_FAILED) == 0)
						continue;
					reason = SpaceWords(ReadFString(mem, objective + OFF_OBJ_FAILURE));
				}
			}

			return reason.empty() ? "Mission Failed" : ("Mission Failed \xE2\x80\xA2 " + reason);
		}

		/// Terrorists still held back by the level's wave zones.
		///
		/// Reports 0 until a zone has actually fired. Every hunt level ships
		/// armed zones - Oil Refinery holds 2+3+2 - but on a normal run they
		/// never trigger, and "7 in reserve" for reinforcements that are never
		/// coming is worse than saying nothing.
		inline void ReadWaves(pine::Client& mem, const std::vector<uint32_t>& zones, int& reserve)
		{
			reserve = 0;
			int fired = 0;
			for (const uint32_t zone : zones)
			{
				uint32_t spawned = 0, to_spawn = 0;
				if (!mem.ReadU32(zone + OFFSET_WAVE_SPAWNED, spawned) ||
					!mem.ReadU32(zone + OFFSET_WAVE_TO_SPAWN, to_spawn))
					continue;
				if (spawned > 500 || to_spawn > 500)
					continue;
				fired += static_cast<int>(spawned);
				if (to_spawn > spawned)
					reserve += static_cast<int>(to_spawn - spawned);
			}
			if (fired == 0)
				reserve = 0;
		}

		/// Terrorists alive now, and the level's authored total. Both stay 0
		/// when this is not a mode with terrorists in it.
		inline bool ReadTangos(pine::Client& mem, int& left, int& reinforcements)
		{
			left = reinforcements = 0;

			if (!IsTerroristClass(mem, CLASS_TERRORIST_AI))
				return false;

			uint32_t table = 0, count = 0;
			if (!mem.ReadU32(ADDR_OBJECT_TABLE, table) ||
				!mem.ReadU32(ADDR_OBJECT_TABLE + 4, count))
				return false;

			table &= EE_MASK;
			if (table < 0x00100000 || table >= 0x02000000 || count == 0 || count > 65536)
				return false;

			std::vector<uint8_t> raw;
			if (!mem.ReadBytes(table, count * 4, raw) || raw.size() != count * 4)
				return false;

			std::vector<uint32_t> objects;
			objects.reserve(count);
			for (uint32_t i = 0; i < count; i++)
			{
				const uint32_t object = (static_cast<uint32_t>(raw[i * 4]) |
											(static_cast<uint32_t>(raw[i * 4 + 1]) << 8) |
											(static_cast<uint32_t>(raw[i * 4 + 2]) << 16) |
											(static_cast<uint32_t>(raw[i * 4 + 3]) << 24)) &
										EE_MASK;
				if (object >= 0x00100000 && object < 0x02000000)
					objects.push_back(object);
			}

			// One batched request for every object's class pointer, rather than
			// one request each.
			std::vector<uint32_t> fields;
			fields.reserve(objects.size());
			for (const uint32_t object : objects)
				fields.push_back(object + OFFSET_OBJ_CLASS);

			std::vector<uint32_t> classes;
			if (!mem.ReadWordsAt(fields, classes) || classes.size() != objects.size())
				return false;

			// The controller outlives the tango. It is still in the object table
			// while the corpse is on the floor - measured at eight corpses to
			// four live terrorists - so counting controllers counts the dead and
			// the card sits on a stale number for minutes. Ask each controller's
			// pawn whether it still has health instead.
			// The wave zones fall out of the same pass: their class pointers
			// have already been read, so they cost nothing to notice.
			fields.clear();
			std::vector<uint32_t> zones;
			for (size_t i = 0; i < objects.size(); i++)
			{
				if ((classes[i] & EE_MASK) == CLASS_TERRORIST_AI)
					fields.push_back(objects[i] + OFFSET_AI_PAWN);
				else if ((classes[i] & EE_MASK) == CLASS_DZONE_WAVE)
					zones.push_back(objects[i]);
			}

			ReadWaves(mem, zones, reinforcements);

			if (!fields.empty())
			{
				std::vector<uint32_t> pawns;
				if (!mem.ReadWordsAt(fields, pawns))
					return false;

				fields.clear();
				for (const uint32_t raw_pawn : pawns)
				{
					const uint32_t pawn = raw_pawn & EE_MASK;
					if (pawn >= 0x00100000 && pawn < 0x02000000)
						fields.push_back(pawn + OFFSET_PAWN_HEALTH);
				}

				std::vector<uint32_t> health;
				if (!fields.empty() && !mem.ReadWordsAt(fields, health))
					return false;

				for (const uint32_t h : health)
				{
					const int32_t hp = static_cast<int32_t>(h);
					if (hp > 0 && hp <= 1000)
						left++;
				}
			}

			return true;
		}

		/// Mission difficulty, the byte immediately before the game mode. The
		/// numbering is the menu's own: the difficulty page hands 1, 2 or 3 to
		/// the routine at VA 0x0049b2d0, which picks the Recruit, Veteran or
		/// Elite artwork from them. Returns nullptr for anything else, which
		/// covers the front end and the modes that have no difficulty.
		inline const char* ReadDifficulty(pine::Client& mem)
		{
			uint32_t manager = 0;
			if (!mem.ReadU32(ADDR_GAME_MANAGER, manager) || manager == 0)
				return nullptr;

			uint8_t value = 0;
			if (!mem.ReadU8(manager + OFFSET_DIFFICULTY, value))
				return nullptr;

			switch (value)
			{
				case 1: return "Recruit";
				case 2: return "Veteran";
				case 3: return "Elite";
				default: return nullptr;
			}
		}

		inline uint8_t ReadGameMode(pine::Client& mem)
		{
			uint32_t manager = 0;
			if (!mem.ReadU32(ADDR_GAME_MANAGER, manager) || manager == 0)
				return 0;

			uint32_t name_ptr = 0, name_len = 0;
			if (mem.ReadU32(manager + OFFSET_GAME_TYPE, name_ptr) &&
				mem.ReadU32(manager + OFFSET_GAME_TYPE + 4, name_len) && name_ptr != 0 && name_len > 0 &&
				name_len <= 64)
			{
				const std::string class_name = mem.ReadString(name_ptr, 63);
				const size_t dot = class_name.find('.');
				const std::string_view bare = (dot == std::string::npos) ?
												  std::string_view(class_name) :
												  std::string_view(class_name).substr(dot + 1);
				for (const GameTypeInfo& gt : GAME_TYPES)
				{
					if (bare == gt.class_name)
						return gt.mode;
				}
			}

			uint8_t mode = 0;
			if (!mem.ReadU8(manager + OFFSET_GAME_MODE, mode))
				return 0;
			return (mode < std::size(MODES)) ? mode : 0;
		}

		inline bool Read(pine::Client& mem, Presence& out)
		{
			std::string package = mem.ReadString(ADDR_LEVEL_PACKAGE, 63);

			// Checked first, and before the empty test: during a load the package
			// buffer still holds the level being LEFT - or nothing at all - so
			// everything below it would name the wrong one or give up entirely.
			if (std::string loading = ReadLoading(mem, package); !loading.empty())
			{
				out.activity = std::move(loading);
				return true;
			}

			if (package.empty())
				return false;
			package = ToUpper(package);

			uint32_t split_screen = 0;
			mem.ReadU32(ADDR_SPLIT_SCREEN, split_screen);

			const char* suffix_session = nullptr;
			for (const SuffixInfo& si : SUFFIXES)
			{
				if (EndsWith(package, si.suffix))
				{
					package.resize(package.size() - std::strlen(si.suffix));
					suffix_session = si.session;
					break;
				}
			}

			// The menu and the shared package are levels as far as the engine is
			// concerned, but not somewhere the player is playing. Picking split
			// screen relaunches the overlay with -ssc/-sst, so the flag is
			// already set while the two of them are still setting up.
			if (package.empty() || package == "MENU" || package == "COMMON" || package == "ENTRY")
			{
				out.mode = split_screen ? "Split Screen" : "In the menus";
				out.activity = "Gearing up";
				return true;
			}

			std::string part;
			bool multiplayer_map = false;
			if (EndsWith(package, "_MP"))
			{
				package.resize(package.size() - 3);
				multiplayer_map = true;
			}
			else if (package.size() > 2 && package[package.size() - 2] == '_' && package.back() >= 'A' &&
					 package.back() <= 'Z')
			{
				part = package.back();
				package.resize(package.size() - 2);
			}

			std::string map_name;
			for (const MapName& mn : MAP_NAMES)
			{
				if (package == mn.token)
				{
					map_name = mn.name;
					break;
				}
			}
			if (map_name.empty())
				map_name = PrettifyToken(package);
			if (!part.empty())
				map_name += ' ' + part;
			out.map = std::move(map_name);

			const uint8_t mode = ReadGameMode(mem);
			if (mode != 0 && mode < std::size(MODES) && MODES[mode].name)
			{
				out.mode = MODES[mode].name;
				out.session = MODES[mode].session;
			}

			// The package's own suffix beats the mode table: it was written by
			// the load that is actually on screen.
			if (suffix_session)
				out.session = suffix_session;
			else if (out.session.empty() && multiplayer_map)
				out.session = "Online";

			if (split_screen)
				out.session = "Split Screen";

			// Line two is built in the order you would want to read it: where
			// you are, then the thing that is changing, then the context that
			// is not. The live count used to sit last, behind two things that
			// never move, which is the wrong way round on a card somebody
			// glances at.
			//
			// "Single Player" is dropped: it is the unremarkable case and it
			// pushed the count further right. Split Screen and Online stay,
			// because those say something.
			const bool notable_session = (out.session != "Single Player");
			std::string session_word = notable_session ? out.session : std::string();
			out.session.clear();

			const bool hunting = (mode == 4 || mode == 5 || mode == 10);
			int left = 0, reinforcements = 0;
			const bool counted = hunting && !out.map.empty() && ReadTangos(mem, left, reinforcements);

			// A debrief ends the mission, so it replaces the live state rather
			// than joining it: a frozen tango count beside a verdict reads as
			// though the mission were still running.
			if (std::string verdict = ReadVerdict(mem); !verdict.empty())
			{
				out.session = std::move(verdict);
				return true;
			}

			std::vector<std::string> parts;

			if (counted)
			{
				std::string tangos = std::to_string(left);
				tangos += (left == 1) ? " tango left" : " tangos left";
				// Reinforcements only once a wave has actually fired, so the
				// line means "more are coming" rather than naming furniture.
				if (reinforcements > 0)
					tangos += ", " + std::to_string(reinforcements) + " inbound";
				parts.push_back(std::move(tangos));
			}

			// Only once a mission is loaded: the field holds whatever was left
			// there while you were in the menus.
			if (const char* difficulty = ReadDifficulty(mem); difficulty && !out.map.empty())
				parts.emplace_back(difficulty);

			if (!session_word.empty())
				parts.push_back(std::move(session_word));

			out.elapsed = ReadMissionSeconds(mem);

			for (const std::string& part : parts)
				out.session = out.session.empty() ? part
						   : (out.session + " \xE2\x80\xA2 " + part);

			return true;
		}
	} // namespace r6three

	inline constexpr Provider PROVIDERS[] = {
		{"SLUS-20883", "Rainbow Six 3", &r6three::Read},
	};

	inline const Provider* Find(const std::string& serial)
	{
		for (const Provider& p : PROVIDERS)
		{
			if (serial == p.serial)
				return &p;
		}
		return nullptr;
	}
} // namespace games
