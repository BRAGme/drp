// SPDX-License-Identifier: MIT
//
// drp - one background program that keeps Discord's Rich Presence describing
// whatever you are actually playing.
//
// It watches two things at once and shows whichever is running:
//
//   PCSX2            over the emulator's own PINE server
//   Mercenaries:     by reading the PC port's process memory
//   Playground of
//   Destruction
//
// Neither is a fork or a patch. Start it once — or let it start with Windows —
// and forget it; it attaches when a game appears and lets go when it exits.
//
// Built as a windowed program so it has no console of its own, but it attaches
// to the terminal that launched it, so running it by hand still prints.

#include "pine.hpp"
#include "procmem.hpp"

#include "discord.hpp"
#include "mercs_watcher.hpp"
#include "pcsx2_sample.hpp"
#include "pcsx2_scanmenu.hpp"
#include "pcsx2_watcher.hpp"
#include "presence.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace
{
	/// The executable name of whatever window is in front, lower-cased, or
	/// empty when it cannot be told. Used only to settle which game is being
	/// played when two of them are running.
	std::string ForegroundExe()
	{
		const HWND window = GetForegroundWindow();
		if (!window)
			return {};

		DWORD pid = 0;
		GetWindowThreadProcessId(window, &pid);
		if (!pid)
			return {};

		const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!process)
			return {};

		char path[MAX_PATH] = {};
		DWORD length = MAX_PATH;
		const bool ok = QueryFullProcessImageNameA(process, 0, path, &length) != 0;
		CloseHandle(process);
		if (!ok)
			return {};

		std::string name(path, length);
		const size_t slash = name.find_last_of("\\/");
		return presence::Lower(slash == std::string::npos ? name : name.substr(slash + 1));
	}

	// This runs all day, so it looks for a game rarely and only watches closely
	// once one is there. The idle check is a process-list snapshot and a refused
	// loopback connect, but there is no reason to do either every second.
	constexpr int POLL_INTERVAL_MS = 1000;
	constexpr int IDLE_INTERVAL_MS = 5000;
	constexpr int MIN_PUSH_INTERVAL_MS = 5000; // Discord drops faster updates
	constexpr int RECONNECT_INTERVAL_MS = 5000;

	constexpr const char* RUN_KEY = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
	constexpr const char* RUN_VALUE = "drp";

	// PCSX2's own Discord application, so its card says PCSX2 and carries PCSX2
	// artwork. That is the honest answer there and the wrong one anywhere else,
	// which is why the Mercenaries side has no default.
	constexpr const char* PCSX2_CLIENT_ID = "1458595419499139094";
	constexpr const char* PCSX2_IMAGE = "4k-pcsx2";

	// The shared "Mercenaries - POD" application, so everyone running this shows
	// the same card with the same artwork. An application id is public; override
	// it in drp.ini to use one of your own.
	constexpr const char* MERCS_CLIENT_ID = "1550527998908956763";
	constexpr const char* MERCS_IMAGE = "cover";

	// The RTX Remix fork of PCSX2 has this same feature built in, so drp leaves
	// that build alone rather than fighting it for the same Discord card. Its
	// PINE version reads "PCSX2 remix-preview-...", a release build's
	// "PCSX2 v2.9.78".
	constexpr const char* SKIP_PCSX2_VERSION = "remix";

	struct Config
	{
		int pine_slot = pine::DEFAULT_SLOT;
		std::string skip_pcsx2_version = SKIP_PCSX2_VERSION;
		std::string pcsx2_client_id = PCSX2_CLIENT_ID;
		std::string pcsx2_image = PCSX2_IMAGE;
		std::string mercs_client_id = MERCS_CLIENT_ID;
		std::string mercs_image = MERCS_IMAGE;
		std::string mercs_data; // normally found beside the running game
		bool log_to_file = true;
	};

	bool g_console = false;
	std::string g_log_path;

	std::string ExeDir()
	{
		char buf[MAX_PATH] = {};
		if (!GetModuleFileNameA(nullptr, buf, MAX_PATH))
			return {};
		std::string path(buf);
		const size_t slash = path.find_last_of("\\/");
		return (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
	}

	std::string ExePath()
	{
		char buf[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, buf, MAX_PATH);
		return buf;
	}

	void Say(const std::string& line)
	{
		char stamp[32];
		SYSTEMTIME st;
		GetLocalTime(&st);
		std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);

		if (g_console)
		{
			std::printf("%s%s\n", stamp, line.c_str());
			std::fflush(stdout);
		}
		if (!g_log_path.empty())
		{
			std::ofstream log(g_log_path, std::ios::app);
			if (log)
				log << stamp << line << "\n";
		}
	}

	std::string Trim(const std::string& s)
	{
		size_t a = 0, b = s.size();
		while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
			a++;
		while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
			b--;
		return s.substr(a, b - a);
	}

	/// drp.ini next to the exe. Everything in it is optional.
	void LoadConfig(Config& cfg)
	{
		std::ifstream in(ExeDir() + "drp.ini");
		if (!in)
			return;

		std::string line;
		while (std::getline(in, line))
		{
			line = Trim(line);
			if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[')
				continue;
			const size_t eq = line.find('=');
			if (eq == std::string::npos)
				continue;

			const std::string key = presence::Lower(Trim(line.substr(0, eq)));
			const std::string value = Trim(line.substr(eq + 1));

			// "key =" with nothing after it means "I did not set this", not "set
			// this to nothing". Without this an old template file silently
			// removes the Discord application everyone shares.
			if (value.empty())
				continue;

			if (key == "pine_slot")
				cfg.pine_slot = std::atoi(value.c_str());
			else if (key == "skip_pcsx2_version")
				cfg.skip_pcsx2_version = value; // empty means "never stand aside"
			else if (key == "pcsx2_client_id")
				cfg.pcsx2_client_id = value;
			else if (key == "pcsx2_image")
				cfg.pcsx2_image = value;
			else if (key == "mercs_client_id")
				cfg.mercs_client_id = value;
			else if (key == "mercs_image")
				cfg.mercs_image = value;
			else if (key == "mercs_data")
				cfg.mercs_data = value;
			else if (key == "log")
				cfg.log_to_file = (value != "0" && presence::Lower(value) != "off");
		}
	}

	/// Writes a drp.ini with everything commented out, so there is something to
	/// edit rather than a format to remember.
	void WriteTemplateConfig(const Config& cfg)
	{
		const std::string path = ExeDir() + "drp.ini";
		if (std::ifstream(path))
			return;

		std::ofstream out(path);
		if (!out)
			return;
		out << "; drp settings.\n"
			   ";\n"
			   "; Nothing in here needs setting. Every line is commented out and shows the\n"
			   "; default it already uses; uncomment one only if you want to change it.\n"
			   ";\n"
			   "; The game's data folder is found next to the running game, so this is\n"
			   "; normally not needed at all.\n"
			   "; mercs_data      = D:\\MercenariesPC\\datapc\n"
			   ";\n"
			   "; Discord names a presence card after whichever application connects to it.\n"
			   "; The default is the shared \"Mercenaries - POD\" application, so everyone\n"
			   "; running this shows the same card with the same artwork. To use one of\n"
			   "; your own instead, make it at\n"
			   ";   https://discord.com/developers/applications\n"
			   "; (Discord allows 32 characters in the name), upload artwork under\n"
			   "; Rich Presence -> Art Assets, and put the Application ID here.\n"
			   "; mercs_client_id = "
			<< MERCS_CLIENT_ID
			<< "\n"
			   "; mercs_image     = "
			<< MERCS_IMAGE
			<< "\n"
			   ";\n"
			   "; PCSX2 has an application of its own, so this needs nothing either.\n"
			   "; pine_slot       = 28011\n"
			   "; pcsx2_client_id = "
			<< PCSX2_CLIENT_ID
			<< "\n"
			   ";\n"
			   "; The RTX Remix fork of PCSX2 shows this same detailed presence\n"
			   "; itself, so drp stands aside when the emulator's version contains\n"
			   "; the text below - otherwise both would connect to Discord as the\n"
			   "; same application and keep overwriting each other's card. Set it\n"
			   "; empty to watch every build regardless.\n"
			   "; skip_pcsx2_version = "
			<< SKIP_PCSX2_VERSION
			<< "\n"
			   ";\n"
			   "; log = 0   turns off drp.log\n";
	}

	// -------------------------------------------------------------------------
	// starting with Windows
	// -------------------------------------------------------------------------

	bool InstallStartup(bool install)
	{
		HKEY key = nullptr;
		if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
			return false;

		LSTATUS rc;
		if (install)
		{
			const std::string command = "\"" + ExePath() + "\"";
			rc = RegSetValueExA(key, RUN_VALUE, 0, REG_SZ,
				reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>(command.size() + 1));
		}
		else
		{
			rc = RegDeleteValueA(key, RUN_VALUE);
			if (rc == ERROR_FILE_NOT_FOUND)
				rc = ERROR_SUCCESS;
		}
		RegCloseKey(key);
		return rc == ERROR_SUCCESS;
	}

	bool StartupInstalled()
	{
		HKEY key = nullptr;
		if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
			return false;
		const bool present = RegQueryValueExA(key, RUN_VALUE, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
		RegCloseKey(key);
		return present;
	}

	/// Only one copy should be pushing a presence.
	bool AlreadyRunning()
	{
		HANDLE mutex = CreateMutexA(nullptr, TRUE, "Local\\drp-presence-singleton");
		return mutex && GetLastError() == ERROR_ALREADY_EXISTS;
	}

	// -------------------------------------------------------------------------

	int Run(const Config& cfg)
	{
		if (AlreadyRunning())
		{
			Say("Another copy of drp is already running; leaving it to it.");
			return 0;
		}

		pcsx2::Watcher pcsx2_watcher(cfg.pine_slot, cfg.skip_pcsx2_version);
		mercs::Watcher mercs_watcher(cfg.mercs_data);

		Say("drp started. Watching PCSX2 (PINE slot " + std::to_string(cfg.pine_slot) + ") and " +
			std::string(mercs::EXE_NAME) + ".");
		if (cfg.mercs_client_id.empty())
			Say("No mercs_client_id, so Mercenaries will be read but not shown on Discord.");

		std::unique_ptr<discord::Client> rpc;
		std::string rpc_client_id; // which application rpc is connected as
		std::string source;        // which game is driving, "" for none
		std::string last_note;       // so a build warning is said once, not every second
		std::string last_pcsx2_note; // likewise for "standing aside from this build"
		std::string last_package;   // the emulated level, logged as it changes
		uint32_t last_loading = 0xFFFFFFFF; // the loading-screen kind, likewise
		presence::Lines pending;
		int64_t last_push = 0;
		int64_t next_rpc = 0;

		for (;;)
		{
			const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
									.count();

			// Whichever game you are actually looking at wins. Leaving the
			// emulator open behind a native game - or the other way round - used
			// to mean the card described the one you were not playing. With
			// neither window in front, the native game keeps the old precedence.
			presence::Lines lines;
			std::string want_source;
			std::string want_client_id;
			const std::string front = ForegroundExe();
			const bool emulator_in_front = front.find("pcsx2") != std::string::npos;

			if (emulator_in_front && pcsx2_watcher.Poll(lines))
			{
				want_source = "pcsx2";
				want_client_id = cfg.pcsx2_client_id;
				lines.large_image = cfg.pcsx2_image;
			}
			else if (mercs_watcher.Poll(lines))
			{
				if (const std::string& note = mercs_watcher.Note(); note != last_note)
				{
					last_note = note;
					if (!note.empty())
						Say(note);
				}
				want_source = "mercs";
				want_client_id = cfg.mercs_client_id;
				lines.large_image = cfg.mercs_image;
			}
			else if (!emulator_in_front && pcsx2_watcher.Poll(lines))
			{
				want_source = "pcsx2";
				want_client_id = cfg.pcsx2_client_id;
				lines.large_image = cfg.pcsx2_image;
			}

			// Said even though that Poll() returned false: "there is an emulator
			// here and I am deliberately not touching it" is the one case where
			// showing nothing would otherwise look like a fault.
			// The level the disc is on, logged whenever it changes. A presence
			// only says where you are once you have arrived, so this is the one
			// view of what the engine does on the way - and unlike a separate
			// diagnostic it costs nothing, because PINE serves one client at a
			// time and drp is already holding it.
			if (pcsx2_watcher.Connected())
			{
				const std::string package =
					pcsx2_watcher.Emu().ReadString(games::r6three::ADDR_LEVEL_PACKAGE, 63);
				if (package != last_package)
				{
					last_package = package;
					Say("level package -> " + (package.empty() ? std::string("(none)") : package));
				}

				// The loading-screen kind. Its zero case is measured; the non-zero
				// case is inferred from the code and has never been caught in a
				// snapshot, so log it and let a real transition settle it.
				uint32_t loading = 0;
				if (pcsx2_watcher.Emu().ReadU32(games::r6three::ADDR_LOADING_KIND, loading) &&
					loading != last_loading)
				{
					last_loading = loading;
					Say("loading kind -> " + std::to_string(loading) + "  destination " +
						pcsx2_watcher.Emu().ReadString(games::r6three::ADDR_DESTINATION, 63));
				}
			}

			if (const std::string& note = pcsx2_watcher.Note(); note != last_pcsx2_note)
			{
				last_pcsx2_note = note;
				if (!note.empty())
					Say(note);
			}

			if (want_source != source)
			{
				source = want_source;
				Say(source.empty() ? "Nothing running." : ("Now watching " + source + "."));
				if (source == "mercs")
				{
					Say("  " + std::to_string(mercs_watcher.MissionCount()) + " contracts read from " +
						mercs_watcher.DataRoot());
				}
				pending = presence::Lines();
			}

			// Discord takes the card's name from the application, so switching
			// games means reconnecting as a different one.
			if (rpc && rpc_client_id != want_client_id)
			{
				rpc.reset();
				rpc_client_id.clear();
			}

			if (lines != pending)
			{
				pending = lines;
				if (!pending.empty())
				{
					// The badge key goes in the log too, so the log alone shows
					// what was actually sent rather than only what was read.
					std::string line = "-> " + pending.details + " | " + pending.state;
					if (!pending.small_image.empty())
						line += "   [badge " + pending.small_image + "]";
					Say(line);
				}
			}

			if (!want_client_id.empty() && !pending.empty())
			{
				if (!rpc && now >= next_rpc)
				{
					next_rpc = now + RECONNECT_INTERVAL_MS;
					auto client = std::make_unique<discord::Client>(want_client_id);
					if (client->Connect())
					{
						rpc = std::move(client);
						rpc_client_id = want_client_id;
						Say("Connected to Discord.");
					}
				}

				if (rpc && (now - last_push) >= MIN_PUSH_INTERVAL_MS)
				{
					discord::Activity activity;
					activity.details = pending.details;
					activity.state = pending.state;
					activity.large_image = pending.large_image;
					activity.large_image_text = pending.large_image_text;
					activity.small_image = pending.small_image;
					activity.small_image_text = pending.small_image_text;
					activity.start_timestamp = pending.start_timestamp;

					if (!rpc->SameAsCurrent(activity))
					{
						if (!rpc->SetActivity(activity))
						{
							Say("Lost the connection to Discord.");
							rpc.reset();
							rpc_client_id.clear();
						}
						last_push = now;
					}
				}
			}
			else if (rpc && pending.empty())
			{
				rpc->SetActivity(discord::Activity());
			}

			std::this_thread::sleep_for(
				std::chrono::milliseconds(source.empty() ? IDLE_INTERVAL_MS : POLL_INTERVAL_MS));
		}
	}

	int Probe(const Config& cfg)
	{
		mercs::Watcher watcher(cfg.mercs_data);
		std::printf("waiting for %s ...\n", mercs::EXE_NAME);
		std::fflush(stdout);

		presence::Lines lines;
		while (!watcher.Poll(lines))
			std::this_thread::sleep_for(std::chrono::seconds(2));

		std::printf("attached: base 0x%08X size 0x%08X, %zu contracts read\n",
			(unsigned)watcher.Proc().ModuleBase(), watcher.Proc().ModuleSize(), watcher.MissionCount());
		std::fflush(stdout);

		std::string last;
		while (watcher.Attached())
		{
			const mercs::Reading r = watcher.Read();
			char line[1200];
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
		std::printf("game exited\n");
		return 0;
	}

	void Usage()
	{
		std::printf(
			"drp - Discord Rich Presence for PCSX2 and the Mercenaries PC port\n"
			"\n"
			"  drp                    watch for either game and keep Discord up to date\n"
			"  drp --startup on       start automatically when you log in\n"
			"  drp --startup off      stop doing that\n"
			"  drp probe              diagnostic: what it can see in Mercenaries\n"
			"  drp scan-menu          diagnostic: Rainbow Six 3's menu globals\n"
			"  drp sample             diagnostic: watch the game struct change as you play\n"
			"      --at <addr>        a fixed address instead of the game manager\n"
			"      --len <bytes>      how much to watch (default 0x200)\n"
			"      --seconds <n>      stop on its own; Ctrl+C otherwise\n"
			"\n"
			"Stop drp before running a diagnostic: PINE serves one client at a time.\n"
			"\n"
			"Settings live in drp.ini next to this exe; it is written on first run.\n");
	}
} // namespace

int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	// No console of our own. If output was redirected into a pipe or a file we
	// already have a usable stdout; otherwise borrow the console that launched
	// us, so running this by hand behaves like a normal command.
	const HANDLE existing_out = GetStdHandle(STD_OUTPUT_HANDLE);
	g_console = (existing_out != nullptr && existing_out != INVALID_HANDLE_VALUE);
	if (!g_console && AttachConsole(ATTACH_PARENT_PROCESS))
	{
		FILE* dummy = nullptr;
		freopen_s(&dummy, "CONOUT$", "w", stdout);
		freopen_s(&dummy, "CONOUT$", "w", stderr);
		g_console = true;
		std::printf("\n");
	}

	Config cfg;
	LoadConfig(cfg);
	WriteTemplateConfig(cfg);
	LoadConfig(cfg);
	if (cfg.log_to_file)
		g_log_path = ExeDir() + "drp.log";

	bool probe = false;
	bool scan_menu = false;
	uint32_t scan_from = 0x00600000;
	uint32_t scan_to = 0x00780000;
	bool sample = false;
	uint32_t sample_at = 0;      // 0 = follow the game manager pointer
	uint32_t sample_len = 0x200; // enough to cover the struct without drowning in it
	uint32_t sample_seconds = 0; // 0 = until Ctrl+C

	for (int i = 1; i < __argc; i++)
	{
		const std::string arg = __argv[i];
		const auto next = [&]() -> std::string { return (i + 1 < __argc) ? __argv[++i] : std::string(); };

		if (arg == "probe")
			probe = true;
		else if (arg == "scan-menu")
			scan_menu = true;
		else if (arg == "sample")
			sample = true;
		else if (arg == "--at")
			sample_at = std::strtoul(next().c_str(), nullptr, 0);
		else if (arg == "--len")
			sample_len = std::strtoul(next().c_str(), nullptr, 0);
		else if (arg == "--seconds")
			sample_seconds = std::strtoul(next().c_str(), nullptr, 0);
		else if (arg == "--startup")
		{
			const std::string mode = presence::Lower(next());
			const bool on = (mode == "on" || mode == "1" || mode == "yes");
			const bool ok = InstallStartup(on);
			std::printf("%s\n", ok ? (on ? "drp will now start when you log in."
										 : "drp will no longer start when you log in.")
									: "Could not change the startup setting.");
			return ok ? 0 : 1;
		}
		else if (arg == "--data")
			cfg.mercs_data = next();
		else if (arg == "--slot")
			cfg.pine_slot = std::atoi(next().c_str());
		else if (arg == "--from")
			scan_from = std::strtoul(next().c_str(), nullptr, 0);
		else if (arg == "--to")
			scan_to = std::strtoul(next().c_str(), nullptr, 0);
		else
		{
			Usage();
			return (arg == "--help" || arg == "-h") ? 0 : 1;
		}
	}

	if (probe)
		return Probe(cfg);
	if (scan_menu)
		return pcsx2::ScanMenu(cfg.pine_slot, scan_from, scan_to);
	if (sample)
		return pcsx2::Sample(cfg.pine_slot, sample_at, sample_len, sample_seconds,
			cfg.pcsx2_client_id, cfg.pcsx2_image);

	if (g_console && !StartupInstalled())
		std::printf("(tip: `drp --startup on` and you never have to run this again)\n");

	return Run(cfg);
}
