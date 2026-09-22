// SPDX-License-Identifier: MIT
//
// Achievements for a game that never had any.
//
// Everything here is read from the same places the presence reads, so this
// costs a handful of pointer chases per poll and no object-table walk. The
// hard part is not the conditions, it is that the game's counters never reset:
// kills, shots and hits carry across level loads and across a failed attempt,
// so anything "in one mission" has to be measured against a baseline latched
// when the mission started.

#pragma once

#include "games.hpp"
#include "pine.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <set>
#include <fstream>
#include <string>
#include <vector>

namespace achievements
{
	/// Everything a rule is allowed to look at, gathered once per poll.
	struct Snapshot
	{
		bool in_mission = false;
		std::string level; // the package token, so a retry of the same map is the same mission
		uint8_t mode = 0;
		int difficulty = 0; // 1 Recruit, 2 Veteran, 3 Elite
		int result = 0;     // 0 running, 1 success, 2 failed
		std::string failure;
		int64_t seconds = 0;

		std::vector<std::string> done;   // objectives satisfied
		std::vector<std::string> failed; // objectives failed

		struct Operative
		{
			std::string name;
			int health = 0;
			int kills = 0;
			int shots = 0;
			int hits = 0;
			bool alive = false;
		};
		Operative team[games::r6three::TEAM_SIZE];
		int team_size = 0;
	};

	/// What a rule gets: the live state, and the same numbers measured from the
	/// start of this mission.
	struct Context
	{
		const Snapshot& now;
		int kills = 0; // this mission
		int shots = 0;
		int hits = 0;
		int team_kills = 0;
		int scorers = 0;    // operatives who killed something this mission
		int mates_down = 0; // of the three, not counting the player
		bool player_alive = false;
		int career_kills = 0;

		bool won() const { return now.result == 1; }
		bool lost() const { return now.result == 2; }
		bool did(const char* task) const
		{
			for (const std::string& d : now.done)
				if (d == task)
					return true;
			return false;
		}
		bool failure_mentions(const char* word) const
		{
			return now.failure.find(word) != std::string::npos;
		}
	};

	struct Rule
	{
		const char* id;
		const char* name;
		const char* note;
		bool (*test)(const Context&);
	};

	// -------------------------------------------------------------------------
	// The set
	//
	// Only conditions that can be read today. Nothing here depends on a
	// headshot counter, a reload count or an alarm state, because none of those
	// have been found in memory - a rule that can never fire is worse than no
	// rule, because you cannot tell it from one you have not earned yet.
	// -------------------------------------------------------------------------

	inline constexpr Rule RULES[] = {
		{"tango_down", "Tango Down", "Your first confirmed kill",
			[](const Context& c) { return c.career_kills >= 1; }},

		{"six_actual", "Six Actual", "Complete a mission",
			[](const Context& c) { return c.won(); }},

		{"recruit_no_more", "Recruit No More", "Complete a mission on Veteran or above",
			[](const Context& c) { return c.won() && c.now.difficulty >= 2; }},

		{"iron_discipline", "Iron Discipline", "Complete a mission on Elite",
			[](const Context& c) { return c.won() && c.now.difficulty >= 3; }},

		{"no_man_left_behind", "No Man Left Behind", "Complete a mission with the whole team standing",
			[](const Context& c) { return c.won() && c.mates_down == 0; }},

		{"last_man_standing", "Last Man Standing", "Complete a mission as the only operative alive",
			[](const Context& c) { return c.won() && c.player_alive && c.mates_down >= 3; }},

		{"walking_wounded", "Walking Wounded", "Complete a mission with an operative barely standing",
			[](const Context& c) {
				if (!c.won())
					return false;
				for (int i = 0; i < c.now.team_size; i++)
					if (c.now.team[i].alive && c.now.team[i].health > 0 && c.now.team[i].health <= 12)
						return true;
				return false;
			}},

		{"rainbow_six", "Rainbow Six", "Complete a mission in which all four operatives scored",
			[](const Context& c) { return c.won() && c.scorers >= 4; }},

		{"chavez_solo", "Chavez Solo", "Complete a mission having taken every kill yourself",
			[](const Context& c) { return c.won() && c.kills >= 5 && c.kills == c.team_kills; }},

		{"surgical", "Surgical", "Finish a mission at 80% accuracy or better, ten kills up",
			[](const Context& c) {
				return c.won() && c.kills >= 10 && c.shots > 0 && c.hits * 5 >= c.shots * 4;
			}},

		{"one_shot_one_kill", "One Shot, One Kill", "Finish a mission without missing once",
			[](const Context& c) { return c.won() && c.kills >= 5 && c.shots > 0 && c.hits == c.shots; }},

		{"ghost", "Ghost", "Complete a mission without firing a shot",
			[](const Context& c) { return c.won() && c.shots == 0; }},

		{"quick_reaction_force", "Quick Reaction Force", "Complete a mission in under five minutes",
			[](const Context& c) { return c.won() && c.now.seconds > 0 && c.now.seconds < 300; }},

		{"the_long_way_round", "The Long Way Round", "Complete a mission that took over twenty minutes",
			[](const Context& c) { return c.won() && c.now.seconds > 1200; }},

		{"clean_sheet", "Clean Sheet", "Complete a mission with nothing failed along the way",
			[](const Context& c) { return c.won() && c.now.failed.empty(); }},

		{"bomb_squad", "Bomb Squad", "Defuse the bomb",
			[](const Context& c) { return c.did("Defuse Bomb"); }},

		{"failsafe", "Failsafe", "Disarm the failsafe device",
			[](const Context& c) { return c.did("Disarm Failsafe Device"); }},

		{"chain_of_custody", "Chain of Custody", "Locate the chemical barrels",
			[](const Context& c) { return c.did("Locate Chemical Barrels"); }},

		{"precious_cargo", "Precious Cargo", "Secure a hostage",
			[](const Context& c) { return c.did("Secure Hostage"); }},

		{"ding_chavez_is_dead", "Ding Chavez Is Dead", "Lose Chavez, and the mission with him",
			[](const Context& c) { return c.lost() && c.failure_mentions("Chavez"); }},

		{"wiped_out", "Wiped Out", "Lose the whole team",
			[](const Context& c) { return c.lost() && c.failure_mentions("Wiped Out"); }},

		{"century", "Century", "One hundred confirmed kills",
			[](const Context& c) { return c.career_kills >= 100; }},
	};

	/// Reads the game, keeps the baselines, remembers what has been earned.
	class Tracker
	{
	public:
		explicit Tracker(std::string path) : m_path(std::move(path)) { Load(); }

		/// Newly earned achievements, in the order they were earned. Empty
		/// almost always, which is the point.
		std::vector<std::string> Poll(pine::Client& mem)
		{
			std::vector<std::string> earned;

			Snapshot now;
			if (!Read(mem, now) || !now.in_mission)
				return earned;

			// A new level, or the mission clock running backwards, is a fresh
			// attempt: rebase, because the game's own counters never reset.
			if (now.level != m_level || now.seconds < m_seconds)
			{
				m_level = now.level;
				Rebase(now);
			}
			m_seconds = now.seconds;

			Context c{now};
			for (int i = 0; i < now.team_size; i++)
			{
				const Snapshot::Operative& op = now.team[i];
				const int kills = op.kills - m_base_kills[i];
				c.team_kills += kills;
				if (kills > 0)
					c.scorers++;
				if (i == 0)
				{
					c.kills = kills;
					c.shots = op.shots - m_base_shots[i];
					c.hits = op.hits - m_base_hits[i];
					c.player_alive = op.alive;
				}
				else if (!op.alive)
				{
					c.mates_down++;
				}
			}
			c.career_kills = now.team_size > 0 ? now.team[0].kills : 0;

			for (const Rule& rule : RULES)
			{
				if (m_earned.count(rule.id) || !rule.test(c))
					continue;
				m_earned.insert(rule.id);
				earned.emplace_back(std::string(rule.name) + " - " + rule.note);
				Append(rule.id);
			}
			return earned;
		}

		size_t Earned() const { return m_earned.size(); }
		static size_t Total() { return std::size(RULES); }

	private:
		void Rebase(const Snapshot& now)
		{
			for (int i = 0; i < games::r6three::TEAM_SIZE; i++)
			{
				m_base_kills[i] = i < now.team_size ? now.team[i].kills : 0;
				m_base_shots[i] = i < now.team_size ? now.team[i].shots : 0;
				m_base_hits[i] = i < now.team_size ? now.team[i].hits : 0;
			}
		}

		void Load()
		{
			std::ifstream in(m_path);
			std::string line;
			while (std::getline(in, line))
			{
				const size_t eq = line.find('=');
				if (eq == std::string::npos)
					continue;
				std::string id = line.substr(0, eq);
				while (!id.empty() && (id.back() == ' ' || id.back() == '\t'))
					id.pop_back();
				if (!id.empty())
					m_earned.insert(id);
			}
		}

		void Append(const std::string& id)
		{
			std::ofstream out(m_path, std::ios::app);
			if (out)
				out << id << " = " << std::time(nullptr) << "\n";
		}

		std::string m_path;
		std::set<std::string> m_earned;
		std::string m_level;
		int64_t m_seconds = 0;
		int m_base_kills[games::r6three::TEAM_SIZE] = {};
		int m_base_shots[games::r6three::TEAM_SIZE] = {};
		int m_base_hits[games::r6three::TEAM_SIZE] = {};

		static bool Read(pine::Client& mem, Snapshot& out);
	};

	/// One pass over the chain the presence already uses. No object-table
	/// walk: every number here is a pointer chase or two from a fixed
	/// address, so polling this every second costs nothing worth measuring.
	inline bool Tracker::Read(pine::Client& mem, Snapshot& out)
	{
		using namespace games::r6three;

		out.level = games::ToUpper(mem.ReadString(ADDR_LEVEL_PACKAGE, 63));
		if (out.level.empty() || out.level == "MENU" || out.level == "MENUOFF" ||
			out.level == "ENTRY" || out.level == "COMMON")
			return false;

		const uint32_t pawn = Chase(mem, ADDR_PLAYER_PAWN);
		const uint32_t level = pawn ? Chase(mem, pawn + OFF_PAWN_LEVEL) : 0;
		const uint32_t game = level ? Chase(mem, level + OFF_LEVEL_GAME) : 0;
		const uint32_t controller = game ? Chase(mem, game + OFF_GAME_CONTROLLER) : 0;
		if (!controller)
			return false;

		out.in_mission = true;
		out.mode = ReadGameMode(mem);
		out.seconds = ReadMissionSeconds(mem);
		if (const char* d = ReadDifficulty(mem); d)
			out.difficulty = (std::string(d) == "Elite") ? 3 : (std::string(d) == "Veteran" ? 2 : 1);

		if (const uint32_t mgr = Chase(mem, game + OFF_GAME_OBJECTIVES); mgr)
		{
			uint32_t result = 0;
			if (mem.ReadU32(mgr + OFF_MGR_RESULT, result) && result <= 2)
				out.result = static_cast<int>(result);

			const uint32_t list = Chase(mem, mgr + OFF_MGR_LIST);
			uint32_t count = 0;
			if (list && mem.ReadU32(mgr + OFF_MGR_COUNT, count) && count <= 32)
			{
				for (uint32_t i = 0; i < count; i++)
				{
					const uint32_t objective = Chase(mem, list + i * 4);
					uint32_t flags = 0;
					if (!objective || !mem.ReadU32(objective + OFF_OBJ_FLAGS, flags))
						continue;

					const std::string task = SpaceWords(ReadFString(mem, objective + OFF_OBJ_DESCRIPTION));
					if (flags & OBJ_FLAG_DONE)
						out.done.push_back(task);
					if (flags & OBJ_FLAG_FAILED)
					{
						out.failed.push_back(task);
						if (out.failure.empty())
						{
							const std::string why = SpaceWords(ReadFString(mem, objective + OFF_OBJ_FAILURE));
							out.failure = why.empty() ? task : why;
						}
					}
				}
			}
		}

		const uint32_t team = Chase(mem, controller + OFF_CTRL_TEAM);
		for (uint32_t slot = 0; team && slot < TEAM_SIZE; slot++)
		{
			const uint32_t body = Chase(mem, team + OFF_TEAM_PAWNS + slot * 4);
			if (!body)
				continue;

			Snapshot::Operative& op = out.team[out.team_size];
			uint32_t wounds = 0, flags = 0, kills = 0, shots = 0, hits = 0;
			mem.ReadU32(body + OFFSET_PAWN_HEALTH, wounds);
			mem.ReadU32(body + OFF_PAWN_FLAGS, flags);
			mem.ReadU32(body + OFF_OP_KILLS, kills);
			mem.ReadU32(body + OFF_OP_SHOTS, shots);
			mem.ReadU32(body + OFF_OP_HITS, hits);
			op.health = static_cast<int>(wounds);
			op.alive = op.health > 0 && (flags & PAWN_FLAG_BODY) == 0;
			op.kills = static_cast<int>(kills);
			op.shots = static_cast<int>(shots);
			op.hits = static_cast<int>(hits);

			const uint32_t identity = Chase(mem, body + OFF_PAWN_IDENTITY);
			const uint32_t klass = identity ? Chase(mem, identity + OFFSET_OBJ_CLASS) : 0;
			const uint32_t label = klass ? Chase(mem, klass + OFFSET_OBJ_NAME) : 0;
			const std::string who = label ? mem.ReadString(label, 48) : std::string();
			for (const Operative& known : OPERATIVES)
			{
				if (who == known.identity)
				{
					op.name = known.name;
					break;
				}
			}
			out.team_size++;
		}
		return true;
	}

} // namespace achievements
