// SPDX-License-Identifier: MIT
//
// pcsx2-drp - descriptive Discord Rich Presence for PCSX2.
//
// Talks to a running PCSX2 over its built-in PINE server, reads a few known
// globals out of the emulated PS2's memory, and tells Discord what the player is
// actually doing: the game mode, the map, single player or split screen. Works
// with any PCSX2 build, official or otherwise, because nothing here is compiled
// into the emulator.

#include "pine.hpp"

#include "discord.hpp"
#include "games.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{
	// PCSX2's own Discord application, so the card says PCSX2 and carries its
	// artwork. Override with --client-id to use your own.
	constexpr const char* DEFAULT_CLIENT_ID = "1458595419499139094";
	constexpr const char* LARGE_IMAGE_KEY = "4k-pcsx2";
	constexpr const char* LARGE_IMAGE_TEXT = "PCSX2 PS2 Emulator";

	// Discord's IPC starts dropping updates past roughly one every four seconds.
	constexpr int MIN_PUSH_INTERVAL_MS = 5000;
	constexpr int POLL_INTERVAL_MS = 1000;
	constexpr int RECONNECT_INTERVAL_MS = 5000;

	constexpr const char* SEPARATOR = " \xE2\x80\xA2 ";

	struct Options
	{
		int slot = pine::DEFAULT_SLOT;
		std::string client_id = DEFAULT_CLIENT_ID;
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

	void Log(const Options& opt, const std::string& line)
	{
		if (!opt.quiet)
			std::printf("%s\n", line.c_str());
		std::fflush(stdout);
	}

	// -------------------------------------------------------------------------
	// normal operation
	// -------------------------------------------------------------------------

	int Run(const Options& opt)
	{
		pine::Client emu(opt.slot);
		discord::Client rpc(opt.client_id);

		std::string serial;
		std::string title;
		const games::Provider* provider = nullptr;
		int64_t session_start = 0;
		int64_t last_push = 0;
		int64_t next_emu_retry = 0;
		int64_t next_rpc_retry = 0;
		discord::Activity pending;
		bool logged_waiting = false;

		Log(opt, "pcsx2-drp: watching PINE slot " + std::to_string(opt.slot) +
					 ". Leave this running; Ctrl+C to stop.");

		for (;;)
		{
			const int64_t now = MonotonicMs();

			if (!emu.IsConnected() && now >= next_emu_retry)
			{
				next_emu_retry = now + RECONNECT_INTERVAL_MS;
				if (emu.Connect())
				{
					Log(opt, "Connected to PCSX2.");
					logged_waiting = false;
					serial.clear();
					provider = nullptr;
				}
				else if (!logged_waiting)
				{
					Log(opt, "Waiting for PCSX2 with PINE enabled (Settings -> Advanced -> Enable PINE)...");
					logged_waiting = true;
				}
			}

			discord::Activity want;

			if (emu.IsConnected())
			{
				pine::Status status = pine::Shutdown;
				if (!emu.GetStatus(status))
				{
					emu.Disconnect();
					Log(opt, "Lost the connection to PCSX2.");
				}
				else if (status == pine::Shutdown)
				{
					serial.clear();
					provider = nullptr;
				}
				else
				{
					std::string current_serial;
					if (emu.GetSerial(current_serial) && !current_serial.empty())
					{
						if (current_serial != serial)
						{
							serial = current_serial;
							provider = games::Find(serial);
							session_start = UnixNow();
							if (!emu.GetTitle(title))
								title = serial;
							Log(opt, "Game: " + title + " (" + serial + ")" +
										 (provider ? " - detailed presence available" : ""));
						}

						want.large_image = LARGE_IMAGE_KEY;
						want.large_image_text = LARGE_IMAGE_TEXT;
						want.start_timestamp = session_start;
						want.details = title;

						games::Presence presence;
						if (provider && provider->read(emu, presence) && !presence.empty())
						{
							want.details = Join(title, presence.mode);
							want.state = presence.map.empty() ? presence.activity :
															    Join(presence.map, presence.session);
						}
					}
					else
					{
						// Running, but between games - the BIOS, or still booting.
						serial.clear();
						provider = nullptr;
					}
				}
			}

			// Log what we would show whether or not Discord is listening, so the
			// console is useful on its own when something reads wrong.
			if (want != pending)
			{
				pending = want;
				Log(opt, pending.details.empty() ? std::string("(nothing to show)") :
												   ("-> " + pending.details + " | " + pending.state));
			}

			if (!rpc.IsConnected() && !pending.details.empty() && now >= next_rpc_retry)
			{
				next_rpc_retry = now + RECONNECT_INTERVAL_MS;
				if (rpc.Connect())
					Log(opt, "Connected to Discord.");
			}

			if (rpc.IsConnected() && !rpc.SameAsCurrent(pending) && (now - last_push) >= MIN_PUSH_INTERVAL_MS)
			{
				if (!rpc.SetActivity(pending))
					Log(opt, "Lost the connection to Discord.");
				last_push = now;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
		}
	}

	// -------------------------------------------------------------------------
	// scan-menu: find the global that names the menu screen you are on
	//
	// Rainbow Six 3 loads its whole front end at once (57 pages), so the page
	// list cannot say which screen is showing. Somewhere there must be a word
	// pointing at the active page, and a page's name sits at page+0x10. This
	// mode watches every global that points at a string starting with "P_" and
	// prints the ones that change as you move through the menus.
	// -------------------------------------------------------------------------

	int ScanMenu(const Options& opt, uint32_t from, uint32_t to)
	{
		pine::Client emu(opt.slot);
		if (!emu.Connect())
		{
			std::printf("Could not reach PCSX2 on PINE slot %d.\n", opt.slot);
			return 1;
		}

		// Every "P_..." literal in the overlay's rodata. A page object's name is
		// at page+0x10, so a word pointing at page means page+0x10 holds one of
		// these; we look for both shapes.
		// Rainbow Six 3's UI root; games.hpp records how these were found.
		constexpr uint32_t UI_ROOT = 0x006EA578;
		constexpr uint32_t PAGE_FIELD_WINDOW = 0x40;

		std::printf("Reading the overlay's string pool...\n");
		std::fflush(stdout);
		constexpr uint32_t RODATA_FROM = 0x005D0000;
		constexpr uint32_t RODATA_TO = 0x00600000;
		std::vector<uint8_t> rodata;
		if (!emu.ReadBytes(RODATA_FROM, RODATA_TO - RODATA_FROM, rodata))
		{
			std::printf("Could not read the string pool. Is a game running?\n");
			return 1;
		}

		std::map<uint32_t, std::string> page_names;
		for (size_t i = 0; i + 3 < rodata.size(); i++)
		{
			if (rodata[i] != 'P' || rodata[i + 1] != '_')
				continue;
			if (i > 0 && rodata[i - 1] != 0)
				continue;
			size_t end = i;
			while (end < rodata.size() && rodata[end] >= 0x20 && rodata[end] < 0x7F)
				end++;
			if (end >= rodata.size() || rodata[end] != 0 || end - i < 4 || end - i > 40)
				continue;
			page_names[RODATA_FROM + static_cast<uint32_t>(i)] =
				std::string(reinterpret_cast<const char*>(&rodata[i]), end - i);
		}
		std::printf("Found %zu page names.\n", page_names.size());
		if (page_names.empty())
			return 1;

		std::printf("Watching globals 0x%08X..0x%08X. Move through the menus; changes print below.\n"
					"Ctrl+C to stop.\n\n",
			from, to);

		std::map<uint32_t, std::string> last_pointers;
		std::map<std::string, std::vector<uint8_t>> last_pages;

		for (;;)
		{
			// (a) globals that point straight at a page name
			std::vector<uint8_t> block;
			if (!emu.ReadBytes(from, to - from, block))
			{
				std::printf("read failed; retrying\n");
				std::fflush(stdout);
				std::this_thread::sleep_for(std::chrono::seconds(2));
				if (!emu.IsConnected())
					emu.Connect();
				continue;
			}

			std::map<uint32_t, std::string> pointers;
			for (size_t off = 0; off + 4 <= block.size(); off += 4)
			{
				uint32_t value;
				std::memcpy(&value, &block[off], 4);
				value &= 0x1FFFFFFF;
				auto it = page_names.find(value);
				if (it != page_names.end())
					pointers[from + static_cast<uint32_t>(off)] = it->second;
			}

			for (const auto& [addr, name] : pointers)
			{
				auto prev = last_pointers.find(addr);
				if (prev == last_pointers.end() || prev->second != name)
					std::printf("ptr  0x%08X -> %s\n", addr, name.c_str());
			}
			for (const auto& [addr, name] : last_pointers)
			{
				if (pointers.find(addr) == pointers.end())
					std::printf("ptr  0x%08X    (cleared, was %s)\n", addr, name.c_str());
			}
			last_pointers = std::move(pointers);

			// (b) the page objects themselves. The UI root is at 0x006EA578, the
			// page container at *(root+8) with a count at +0x28 and an array of
			// page pointers at +0x2C; a page's name is at page+0x10. Whichever
			// fields change as you move between screens are the ones that say
			// what is on screen.
			uint32_t root = 0, container = 0, count = 0, array = 0;
			if (emu.ReadU32(UI_ROOT, root) && emu.ReadU32(root + 8, container) &&
				emu.ReadU32(container + 0x28, count) && emu.ReadU32(container + 0x2C, array) && count > 0 &&
				count <= 256)
			{
				std::vector<uint8_t> pointer_block;
				if (emu.ReadBytes(array, count * 4, pointer_block))
				{
					for (uint32_t i = 0; i < count; i++)
					{
						uint32_t page = 0;
						std::memcpy(&page, &pointer_block[i * 4], 4);
						if (page == 0)
							continue;

						const std::string name = emu.ReadString(page + 0x10, 40);
						if (name.empty())
							continue;

						std::vector<uint8_t> fields;
						if (!emu.ReadBytes(page, PAGE_FIELD_WINDOW, fields))
							continue;

						auto prev = last_pages.find(name);
						if (prev != last_pages.end() && prev->second.size() == fields.size())
						{
							std::string changed;
							for (size_t off = 0; off < fields.size(); off++)
							{
								if (fields[off] != prev->second[off])
									changed += " +" + std::to_string(off);
							}
							if (!changed.empty())
								std::printf("page %-34s changed at%s\n", name.c_str(), changed.c_str());
						}
						last_pages[name] = std::move(fields);
					}
				}
			}

			std::fflush(stdout);
			std::this_thread::sleep_for(std::chrono::milliseconds(750));
		}
	}

	void Usage()
	{
		std::printf(
			"pcsx2-drp - descriptive Discord Rich Presence for PCSX2\n"
			"\n"
			"  pcsx2-drp [options]              watch PCSX2 and update Discord\n"
			"  pcsx2-drp scan-menu [options]    diagnostic: watch menu-page globals\n"
			"\n"
			"Options:\n"
			"  --slot N           PINE slot PCSX2 is listening on (default %d)\n"
			"  --client-id ID     Discord application id (default: PCSX2's own)\n"
			"  --quiet            no console output\n"
			"  --from HEX --to HEX  scan-menu range (default 0x00600000..0x00780000)\n"
			"\n"
			"In PCSX2, turn on Settings -> Advanced -> Enable PINE, and turn OFF\n"
			"Settings -> Interface -> Enable Discord Presence so the two do not\n"
			"both push a presence.\n",
			pine::DEFAULT_SLOT);
	}
} // namespace

int main(int argc, char** argv)
{
	WSADATA wsa = {};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		std::printf("WSAStartup failed.\n");
		return 1;
	}

	Options opt;
	bool scan = false;
	uint32_t scan_from = 0x00600000;
	uint32_t scan_to = 0x00780000;

	for (int i = 1; i < argc; i++)
	{
		const std::string arg = argv[i];
		const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };

		if (arg == "scan-menu")
			scan = true;
		else if (arg == "--slot")
			opt.slot = std::atoi(next().c_str());
		else if (arg == "--client-id")
			opt.client_id = next();
		else if (arg == "--quiet")
			opt.quiet = true;
		else if (arg == "--from")
			scan_from = std::strtoul(next().c_str(), nullptr, 0);
		else if (arg == "--to")
			scan_to = std::strtoul(next().c_str(), nullptr, 0);
		else
		{
			Usage();
			return arg == "--help" || arg == "-h" ? 0 : 1;
		}
	}

	return scan ? ScanMenu(opt, scan_from, scan_to) : Run(opt);
}
