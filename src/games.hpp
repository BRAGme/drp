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
		std::string session;  // "Split screen" / "Single player" / "Online"
		std::string activity; // "Gearing up", used instead of a map in the menus

		bool empty() const { return mode.empty() && map.empty() && session.empty() && activity.empty(); }
	};

	using ReadFn = bool (*)(pine::Client& mem, Presence& out);

	struct Provider
	{
		const char* serial;
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
			{"Campaign", "Single player"},       // 1  R6StoryModeGame
			{"Practice Mission", "Single player"},       // 2  R6PracticeModeGame
			{"Campaign Co-op", "Online"},        // 3  R6CoopStoryModeGame
			{"Terrorist Hunt", "Single player"}, // 4  R6TerroristHuntGame
			{"Terrorist Hunt Co-op", "Online"},  // 5  R6CoopTerroristHuntGame
			{"Deathmatch", "Online"},            // 6  R6DeathMatch
			{"Team Deathmatch", "Online"},       // 7  R6TeamDeathMatchGame
			{"Sharpshooter", "Online"},          // 8  R6SharpShooterGame
			{"Sharpshooter", "Split screen"},    // 9  R6SharpShooterGameForSplitScreen
			{"Terrorist Hunt", "Split screen"},  // 10 R6TerroristHuntGameForSplitScreen
			{"Practice Mission", "Split screen"},        // 11 R6PracticeModeGameForSplitScreen
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
			{"_SS", "Split screen"},
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
		constexpr uint32_t ADDR_TERRORIST_MGR = 0x00686DD8;
		constexpr uint32_t OFFSET_MGR_TOTAL = 0x10C;
		constexpr uint32_t CLASS_TERRORIST_AI = 0x007A17E0;
		constexpr uint32_t OFFSET_OBJ_CLASS = 0x24;
		constexpr uint32_t OFFSET_OBJ_NAME = 0x28;
		constexpr uint32_t OFFSET_AI_PAWN = 0x108;     // R6TerroristAI -> its pawn
		constexpr uint32_t OFFSET_PAWN_HEALTH = 0x630; // 0 once it is a corpse

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

		/// Terrorists alive now, and the level's authored total. Both stay 0
		/// when this is not a mode with terrorists in it.
		inline bool ReadTangos(pine::Client& mem, int& left, int& total)
		{
			left = total = 0;

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
			fields.clear();
			for (size_t i = 0; i < objects.size(); i++)
				if ((classes[i] & EE_MASK) == CLASS_TERRORIST_AI)
					fields.push_back(objects[i] + OFFSET_AI_PAWN);

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

			uint32_t manager = 0;
			if (mem.ReadU32(ADDR_TERRORIST_MGR, manager) && (manager & EE_MASK) >= 0x00100000)
			{
				uint32_t n = 0;
				if (mem.ReadU32((manager & EE_MASK) + OFFSET_MGR_TOTAL, n) && n <= 500)
					total = static_cast<int>(n);
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
				out.session = "Split screen";

			// Difficulty rides on line two with the map, so line one stays the
			// game and the mode. Only shown once a mission is actually loaded -
			// the field holds whatever was left there while in the menus.
			if (const char* difficulty = ReadDifficulty(mem); difficulty && !out.map.empty())
				out.session = out.session.empty() ? difficulty : (std::string(difficulty) + " \xE2\x80\xA2 " + out.session);

			// Tangos last, because it is the line that changes while you play.
			// Only the hunt modes count terrorists; in a campaign mission the
			// same objects exist but killing them is not the objective, so a
			// number there would read as a score it is not.
			const bool hunting = (mode == 4 || mode == 5 || mode == 10);
			if (hunting && !out.map.empty())
			{
				int left = 0, total = 0;
				if (ReadTangos(mem, left, total))
				{
					std::string tangos = std::to_string(left);
					if (total > 0 && total >= left)
						tangos += " of " + std::to_string(total);
					tangos += (left == 1) ? " tango left" : " tangos left";
					out.session = out.session.empty()
									  ? tangos
									  : (out.session + " \xE2\x80\xA2 " + tangos);
				}
			}


			return true;
		}
	} // namespace r6three

	inline constexpr Provider PROVIDERS[] = {
		{"SLUS-20883", "Tom Clancy's Rainbow Six 3", &r6three::Read},
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
