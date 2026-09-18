// SPDX-License-Identifier: MIT
//
// Watching the Mercenaries PC port: attach, read its state machines, and turn
// what they say into the two lines a presence shows.

#pragma once

#include "procmem.hpp"

#include "mercs.hpp"
#include "presence.hpp"

#include <cctype>
#include <chrono>
#include <fstream>
#include <string>

namespace mercs
{
	/// One tick's worth of what the game is doing.
	struct Reading
	{
		std::string merc;      // "Chris Jacobs"
		std::string where;     // "North West Korea"
		std::string activity;  // "Playing" / "In the DataPod" / ...
		std::string stance;    // "On foot" / "Driving" / "On the gun"
		std::string doing;     // "Verifying a target" / "Planting charges"
		std::string mission;   // contract id, lower case
		std::string objective; // the live HUD objective line
	};

	class Watcher
	{
	public:
		/// `data_root` is a fallback. Normally the game's own folder is used,
		/// which is the only way this works on a machine that is not mine.
		explicit Watcher(std::string data_root)
			: m_data_root(std::move(data_root))
		{
		}

		size_t MissionCount() const { return m_missions.size(); }
		const std::string& DataRoot() const { return m_data_root; }
		bool Attached() const { return m_proc.IsAttached() && m_proc.IsAlive(); }
		procmem::Process& Proc() { return m_proc; }

		/// Attaches if the game is running. Cheap when it is not.
		bool Poll(presence::Lines& out)
		{
			if (!Attached())
			{
				Forget();
				m_proc.Detach();
				if (!m_proc.Attach(EXE_NAME))
					return false;

				const std::string beside = m_proc.ImageDir() + "datapc";
				if (!beside.empty() && std::ifstream(beside + "\\config\\mission.ini"))
					m_data_root = beside;
				m_missions = LoadMissions(m_data_root);

				m_known_build = (m_proc.ModuleSize() == KNOWN_IMAGE_SIZE);
				m_note.clear();
				if (!m_known_build)
				{
					m_note = "This is not the build these addresses came from (image is " +
							 std::to_string(m_proc.ModuleSize()) + " bytes, expected " +
							 std::to_string(KNOWN_IMAGE_SIZE) +
							 "), so only the game's name will be shown.";
				}

				m_started = std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::system_clock::now().time_since_epoch())
								.count();
			}

			if (!m_known_build)
			{
				out = presence::Lines();
				out.details = "Mercenaries: Playground of Destruction";
				out.large_image_text = out.details;
				out.start_timestamp = m_started;
				return true;
			}

			const Reading r = Read();

			// A load screen means the level may be a different one afterwards.
			if (r.activity == "Loading")
				m_level_hash = 0;

			Compose(r, out);
			out.large_image_text = "Mercenaries: Playground of Destruction";
			out.small_image = MercBadgeFor(r.merc);
			out.small_image_text = r.merc;
			out.start_timestamp = m_started;
			return true;
		}

		void Forget()
		{
			m_state_game.Forget();
			m_ai_player.Forget();
			m_player_actor.Forget();
			m_objective_tray.Forget();
			m_tray_buffer_offset = 0;
			m_game_options = 0;
			m_mission_data = 0;
			m_level_hash = 0;
			m_level_checked = 0;
		}

		Reading Read()
		{
			Reading r;
			procmem::Process& proc = m_proc;

			if (m_game_options == 0 || !IsMercCrc(proc.ReadU32(m_game_options)))
				m_game_options = FindGameOptions(proc);
			if (m_game_options)
			{
				if (const char* name = MercNameFor(proc.ReadU32(m_game_options)))
					r.merc = name;
			}

			// The level only changes across a load, so this expensive sweep runs
			// at most every ten seconds, and only while nothing is known.
			const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
									.count();
			if (m_level_hash == 0 && (now - m_level_checked) > 10000)
			{
				m_level_checked = now;
				m_level_hash = CurrentLevelHash(proc);
			}
			if (const LevelName* lv = LevelFor(m_level_hash))
				r.where = lv->label;

			if (const uintptr_t machine = m_state_game.Get(proc))
			{
				if (const char* s = CurrentState(proc, machine, GAME_STATES, std::size(GAME_STATES)))
					r.activity = s;
			}

			if (const uintptr_t ai = m_ai_player.Get(proc))
			{
				if (const char* s = CurrentState(proc, ai, PLAYER_STATES, std::size(PLAYER_STATES)))
					r.stance = s;
			}

			if (const uintptr_t actor = m_player_actor.Get(proc))
			{
				if (const char* s = CurrentState(proc, actor, ACTION_STATES, std::size(ACTION_STATES)))
					r.doing = s;
			}

			if (const uintptr_t tray = m_objective_tray.Get(proc))
			{
				const std::vector<std::string> lines = ObjectiveLines(proc, tray, m_tray_buffer_offset);
				if (!lines.empty())
					r.objective = lines.front();
			}

			if (m_mission_data == 0)
				m_mission_data = FindCurrentMissionData(proc, m_missions);
			if (m_mission_data)
				r.mission = CurrentMissionId(proc, m_mission_data);

			return r;
		}

		const Mission* MissionFor(const std::string& id) const
		{
			for (const auto& [key, m] : m_missions)
			{
				if (presence::Lower(key) == id)
					return &m;
			}
			return nullptr;
		}

		/// Turn a reading into the two lines Discord shows.
		///
		/// The one judgement call: the game keeps the last contract's record
		/// loaded even in the front end, so that record alone cannot say whether
		/// a contract is running. What does say so is the HUD objective tray —
		/// the engine only writes to it while a mission script is driving.
		void Compose(const Reading& r, presence::Lines& out) const
		{
			const bool in_world = !r.activity.empty();
			const bool on_contract = in_world && !r.objective.empty() && !r.mission.empty();

			std::string headline;
			if (!in_world)
			{
				headline = "In the menus";
			}
			else if (r.activity != "Playing")
			{
				headline = r.activity;
			}
			else if (on_contract)
			{
				const Mission* m = MissionFor(r.mission);
				const std::string title = m ? (m->title.empty() ? m->description : m->title) : std::string();
				headline = title.empty() ? "On contract" : ("On contract: " + title);

				if (m)
				{
					const std::string who = FactionPhrase(m->faction);
					if (!who.empty())
						headline += " for " + who;
				}
			}
			else
			{
				headline = "Free roaming";
			}

			out.details = presence::Join(r.merc.empty() ? "Mercenaries" : r.merc, headline);

			std::string detail = r.doing;
			if (detail.empty() && on_contract)
				detail = r.objective;

			// The player's AI comes up a moment before the game state settles
			// during a load, so without this the line reads "In the menus • On
			// foot" for a second.
			std::string second = in_world ? presence::Join(detail, r.stance) : std::string();
			if (in_world && !r.where.empty())
				second = presence::Join(second, "in " + r.where);
			out.state = second;

			if (out.details.empty())
				out.details = "Mercenaries: Playground of Destruction";
		}

		/// "MAFIA" -> "the Mafia", so it reads inside a sentence.
		static std::string FactionPhrase(const std::string& faction)
		{
			if (faction.empty())
				return {};
			std::string f = presence::Lower(faction);
			f[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(f[0])));
			if (f == "Sk")
				return "the South Koreans";
			if (f == "Allies")
				return "the Allies";
			if (f == "China")
				return "the Chinese";
			if (f == "Mafia")
				return "the Mafia";
			return f;
		}

		/// Empty unless the running game is a build this cannot read.
		const std::string& Note() const { return m_note; }
		bool KnownBuild() const { return m_known_build; }

	private:
		std::string m_note;
		bool m_known_build = true;
		std::string m_data_root;
		std::map<std::string, Mission> m_missions;
		procmem::Process m_proc;

		procmem::VtableSingleton m_state_game{RVA_STATE_GAME, "RsMainStateGame"};
		procmem::VtableSingleton m_ai_player{RVA_AI_PLAYER, "RsAiPlayer"};
		procmem::VtableSingleton m_player_actor{RVA_PLAYER_ACTOR, "RsActorVehicleHumanPlayer"};
		procmem::VtableSingleton m_objective_tray{RVA_OBJECTIVE_TRAY, "RsHudObjectiveTray"};

		uint32_t m_tray_buffer_offset = 0;
		uintptr_t m_game_options = 0;
		uintptr_t m_mission_data = 0;
		uint32_t m_level_hash = 0;
		int64_t m_level_checked = 0;
		int64_t m_started = 0;
	};
} // namespace mercs
