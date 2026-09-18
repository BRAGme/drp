// SPDX-License-Identifier: MIT
//
// mercs-drp - descriptive Discord Rich Presence for the Mercenaries:
// Playground of Destruction PC port.
//
// The port is somebody else's binary, so there is nothing to compile into it.
// This attaches to the running process, reads (never writes) its memory, works
// out what the player is doing from the game's own state machines, and tells
// Discord. See mercs.hpp for how the addresses were found.

#include "procmem.hpp"

#include "discord.hpp"
#include "mercs.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
	constexpr const char* DEFAULT_CLIENT_ID = "1458595419499139094";
	constexpr const char* SEPARATOR = " \xE2\x80\xA2 ";

	constexpr int MIN_PUSH_INTERVAL_MS = 5000;
	constexpr int POLL_INTERVAL_MS = 1000;
	constexpr int RECONNECT_INTERVAL_MS = 5000;

	// How far into a state-machine object to look for its current-state pointer.
	constexpr uint32_t MACHINE_WINDOW = 0x600;

	struct Options
	{
		std::string client_id = DEFAULT_CLIENT_ID;
		std::string data_root = "D:\\MercenariesPC\\datapc";
		bool quiet = false;
	};

	int64_t UnixNow()
	{
		return std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count();
	}

	int64_t MonotonicMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
			.count();
	}

	std::string Join(const std::string& a, const std::string& b)
	{
		if (a.empty())
			return b;
		if (b.empty())
			return a;
		return a + SEPARATOR + b;
	}

	void Say(const Options& opt, const std::string& line)
	{
		if (!opt.quiet)
			std::printf("%s\n", line.c_str());
		std::fflush(stdout);
	}

	std::string Lower(std::string s)
	{
		for (char& c : s)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	/// "MAFIA" -> "the Mafia", for reading aloud in a sentence.
	std::string FactionPhrase(const std::string& faction)
	{
		if (faction.empty())
			return {};
		std::string f = Lower(faction);
		if (!f.empty())
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

	// -------------------------------------------------------------------------
	// What the game is doing, read once per tick
	// -------------------------------------------------------------------------

	struct Reading
	{
		std::string merc;      // "Chris Jacobs"
		std::string where;     // "North West Korea"
		std::string activity;  // "Playing" / "In the DataPod" / ...
		std::string stance;    // "On foot" / "Driving" / "On the gun"
		std::string doing;     // "Verifying a target" / "Planting charges"
		std::string mission;   // contract id, lower case, empty when free roaming
		std::string objective; // the live HUD objective line
	};

	class Watcher
	{
	public:
		explicit Watcher(const Options& opt)
			: m_missions(mercs::LoadMissions(opt.data_root))
		{
		}

		size_t MissionCount() const { return m_missions.size(); }

		const mercs::Mission* MissionFor(const std::string& id) const
		{
			for (const auto& [key, m] : m_missions)
			{
				if (Lower(key) == id)
					return &m;
			}
			return nullptr;
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

		Reading Read(procmem::Process& proc)
		{
			Reading r;

			if (m_game_options == 0 || !mercs::IsMercCrc(proc.ReadU32(m_game_options)))
				m_game_options = mercs::FindGameOptions(proc);
			if (m_game_options)
			{
				if (const char* name = mercs::MercNameFor(proc.ReadU32(m_game_options)))
					r.merc = name;
			}

			// The level only changes across a load, so this expensive sweep runs
			// at most every ten seconds and only while nothing is known.
			const int64_t now = MonotonicMs();
			if (m_level_hash == 0 && (now - m_level_checked) > 10000)
			{
				m_level_checked = now;
				m_level_hash = mercs::CurrentLevelHash(proc);
			}
			if (const mercs::LevelName* lv = mercs::LevelFor(m_level_hash))
				r.where = lv->label;

			if (const uintptr_t machine = m_state_game.Get(proc))
			{
				if (const char* s = mercs::CurrentState(proc, machine, mercs::GAME_STATES,
						std::size(mercs::GAME_STATES), MACHINE_WINDOW))
					r.activity = s;
			}

			if (const uintptr_t ai = m_ai_player.Get(proc))
			{
				if (const char* s = mercs::CurrentState(proc, ai, mercs::PLAYER_STATES,
						std::size(mercs::PLAYER_STATES), MACHINE_WINDOW))
					r.stance = s;
			}

			if (const uintptr_t actor = m_player_actor.Get(proc))
			{
				if (const char* s = mercs::CurrentState(proc, actor, mercs::ACTION_STATES,
						std::size(mercs::ACTION_STATES), MACHINE_WINDOW))
					r.doing = s;
			}

			if (const uintptr_t tray = m_objective_tray.Get(proc))
			{
				const std::vector<std::string> lines =
					mercs::ObjectiveLines(proc, tray, m_tray_buffer_offset);
				if (!lines.empty())
					r.objective = lines.front();
			}

			if (m_mission_data == 0)
				m_mission_data = mercs::FindCurrentMissionData(proc, m_missions);
			if (m_mission_data)
				r.mission = mercs::CurrentMissionId(proc, m_mission_data);

			return r;
		}

		/// The level sweep is the only thing that has to be re-done on a load.
		void LevelMayHaveChanged() { m_level_hash = 0; }

	private:
		std::map<std::string, mercs::Mission> m_missions;

		procmem::VtableSingleton m_state_game{mercs::RVA_STATE_GAME, "RsMainStateGame"};
		procmem::VtableSingleton m_ai_player{mercs::RVA_AI_PLAYER, "RsAiPlayer"};
		procmem::VtableSingleton m_player_actor{mercs::RVA_PLAYER_ACTOR, "RsActorVehicleHumanPlayer"};
		procmem::VtableSingleton m_objective_tray{mercs::RVA_OBJECTIVE_TRAY, "RsHudObjectiveTray"};

		uint32_t m_tray_buffer_offset = 0;
		uintptr_t m_game_options = 0;
		uintptr_t m_mission_data = 0;
		uint32_t m_level_hash = 0;
		int64_t m_level_checked = 0;
	};

	/// Turn a reading into the two lines Discord shows.
	///
	/// The one judgement call here: the game keeps the last contract's record
	/// loaded even in the front end, so the record alone cannot say whether a
	/// contract is running. What does say so is the HUD objective tray — the
	/// engine only writes to it while a mission script is driving.
	void Compose(const Watcher& watcher, const Reading& r, discord::Activity& out)
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
			const mercs::Mission* m = watcher.MissionFor(r.mission);
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

		out.details = Join(r.merc.empty() ? "Mercenaries" : r.merc, headline);

		// Line two: the most specific thing there is, then how and where.
		std::string detail = r.doing;
		if (detail.empty() && on_contract)
			detail = r.objective;

		std::string second = Join(detail, r.stance);
		if (in_world && !r.where.empty())
			second = Join(second, "in " + r.where);
		out.state = second;

		if (out.details.empty())
			out.details = "Mercenaries: Playground of Destruction";
	}

	// -------------------------------------------------------------------------
	// probe: everything it can see, printed as it changes
	// -------------------------------------------------------------------------

	int Probe(const Options& opt)
	{
		procmem::Process proc;
		std::printf("waiting for %s ...\n", mercs::EXE_NAME);
		std::fflush(stdout);

		while (!proc.Attach(mercs::EXE_NAME))
			std::this_thread::sleep_for(std::chrono::seconds(2));

		std::printf("attached: base 0x%08X size 0x%08X\n", (unsigned)proc.ModuleBase(), proc.ModuleSize());
		std::fflush(stdout);

		Watcher watcher(opt);
		std::printf("contracts read from data: %zu\n", watcher.MissionCount());

		struct Target
		{
			const char* label;
			uint32_t rva;
		};
		const Target targets[] = {
			{"RsMainStateGame", mercs::RVA_STATE_GAME},
			{"RsMainStateShell2", mercs::RVA_STATE_SHELL},
			{"RsAiPlayer", mercs::RVA_AI_PLAYER},
			{"RsActorVehicleHumanPlayer", mercs::RVA_PLAYER_ACTOR},
			{"RsHudObjectiveTray", mercs::RVA_OBJECTIVE_TRAY},
			{"RsWorld", mercs::RVA_WORLD},
		};

		std::printf("--- one-off object sweep ---\n");
		for (const Target& t : targets)
		{
			const auto hits = proc.FindPointersTo(static_cast<uint32_t>(proc.ModuleBase() + t.rva), 16);
			std::printf("  %-26s x%zu%s", t.label, hits.size(), hits.empty() ? "" : "  at");
			for (size_t i = 0; i < hits.size() && i < 4; i++)
				std::printf(" 0x%08X", (unsigned)hits[i]);
			std::printf("\n");
		}
		std::printf("  RsGameOptions._uiCharacter  0x%08X\n", (unsigned)mercs::FindGameOptions(proc));
		std::printf("  level hash                  0x%08X\n", mercs::CurrentLevelHash(proc));
		std::fflush(stdout);

		std::string last;
		for (;;)
		{
			if (!proc.IsAlive())
			{
				std::printf("game exited\n");
				return 0;
			}

			const Reading r = watcher.Read(proc);
			char line[1024];
			std::snprintf(line, sizeof(line),
				"merc='%s' where='%s' activity='%s' stance='%s' doing='%s' mission='%s'\n  objective='%s'",
				r.merc.c_str(), r.where.c_str(), r.activity.c_str(), r.stance.c_str(), r.doing.c_str(),
				r.mission.c_str(), r.objective.c_str());

			if (line != last)
			{
				std::printf("%s\n", line);
				std::fflush(stdout);
				last = line;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		}
	}

	// -------------------------------------------------------------------------
	// normal operation
	// -------------------------------------------------------------------------

	int Run(const Options& opt)
	{
		procmem::Process proc;
		discord::Client rpc(opt.client_id);
		Watcher watcher(opt);

		Say(opt, "mercs-drp: " + std::to_string(watcher.MissionCount()) + " contracts read from " + opt.data_root);
		Say(opt, "Waiting for " + std::string(mercs::EXE_NAME) + ". Leave this running; Ctrl+C to stop.");

		int64_t session_start = 0;
		int64_t last_push = 0;
		int64_t next_attach = 0;
		int64_t next_rpc = 0;
		std::string last_level_label;
		discord::Activity pending;

		for (;;)
		{
			const int64_t now = MonotonicMs();

			if ((!proc.IsAttached() || !proc.IsAlive()) && now >= next_attach)
			{
				next_attach = now + RECONNECT_INTERVAL_MS;
				proc.Detach();
				watcher.Forget();
				if (proc.Attach(mercs::EXE_NAME))
				{
					session_start = UnixNow();
					Say(opt, "Attached to Mercenaries.");
				}
			}

			discord::Activity want;
			if (proc.IsAttached() && proc.IsAlive())
			{
				const Reading r = watcher.Read(proc);

				// A load screen means the level may be a different one now.
				if (r.activity == "Loading" && !last_level_label.empty())
				{
					watcher.LevelMayHaveChanged();
					last_level_label.clear();
				}
				else if (!r.where.empty())
				{
					last_level_label = r.where;
				}

				want.large_image = "4k-pcsx2";
				want.large_image_text = "Mercenaries: Playground of Destruction";
				want.start_timestamp = session_start;
				Compose(watcher, r, want);
			}

			if (want != pending)
			{
				pending = want;
				Say(opt, pending.details.empty() ? std::string("(nothing to show)")
												 : ("-> " + pending.details + " | " + pending.state));
			}

			if (!rpc.IsConnected() && !pending.details.empty() && now >= next_rpc)
			{
				next_rpc = now + RECONNECT_INTERVAL_MS;
				if (rpc.Connect())
					Say(opt, "Connected to Discord.");
			}

			if (rpc.IsConnected() && !rpc.SameAsCurrent(pending) && (now - last_push) >= MIN_PUSH_INTERVAL_MS)
			{
				if (!rpc.SetActivity(pending))
					Say(opt, "Lost the connection to Discord.");
				last_push = now;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
		}
	}

	void Usage()
	{
		std::printf(
			"mercs-drp - descriptive Discord Rich Presence for the Mercenaries PC port\n"
			"\n"
			"  mercs-drp [options]        watch the game and update Discord\n"
			"  mercs-drp probe            diagnostic: everything it can see\n"
			"\n"
			"Options:\n"
			"  --data <path>       the port's datapc folder (default D:\\MercenariesPC\\datapc)\n"
			"  --client-id <id>    Discord application id\n"
			"  --quiet             no console output\n");
	}
} // namespace

int main(int argc, char** argv)
{
	Options opt;
	bool probe = false;

	for (int i = 1; i < argc; i++)
	{
		const std::string arg = argv[i];
		const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };

		if (arg == "probe")
			probe = true;
		else if (arg == "--data")
			opt.data_root = next();
		else if (arg == "--client-id")
			opt.client_id = next();
		else if (arg == "--quiet")
			opt.quiet = true;
		else
		{
			Usage();
			return (arg == "--help" || arg == "-h") ? 0 : 1;
		}
	}

	return probe ? Probe(opt) : Run(opt);
}
