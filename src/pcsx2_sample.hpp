// SPDX-License-Identifier: MIT
//
// A research harness, not part of the presence: watching a struct in emulated
// memory while somebody plays, and reporting only what changed. See the README.

#pragma once

#include "discord.hpp"
#include "games.hpp"
#include "pine.hpp"
#include "presence.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pcsx2
{
	// -------------------------------------------------------------------------
	// sample: find live fields by watching them move
	//
	// Reading the overlay statically keeps landing on FName tables and page
	// constructors rather than on values - the objective text, the terrorist
	// count and the team roster all live behind manager objects. Watching is
	// cheaper than chasing: play a mission normally and a counter that steps
	// 12, 11, 10 as you clear the map is unmistakable, while a frame timer is
	// obviously noise.
	//
	// So this samples a window of memory a few times a second and prints an
	// offset the first few times it changes. An offset that keeps changing is
	// marked noisy and goes quiet, which is what separates "terrorists left"
	// from "milliseconds elapsed". A tally at the end ranks every offset by how
	// often it moved, quietest first, because the interesting fields are the
	// ones that changed a handful of times and no more.
	// -------------------------------------------------------------------------

	struct SampleField
	{
		uint32_t changes = 0;
		uint8_t last = 0;
		bool noisy = false;
	};

	/// Ctrl+C is how this mode ends, and the ranked tally is the whole point of
	/// running it, so the handler has to stop the loop rather than kill the
	/// process mid-session.
	inline volatile bool g_sample_stop = false;

	inline BOOL WINAPI SampleCtrlHandler(DWORD type)
	{
		if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT)
		{
			g_sample_stop = true;
			return TRUE; // handled: do not let the default handler kill us
		}
		return FALSE;
	}

	/// Quietest first: a field that moved three times is a far better candidate
	/// for "terrorists left" than one that moved three thousand.
	inline void SampleSummary(const std::vector<SampleField>& fields)
	{
		std::vector<std::pair<uint32_t, uint32_t>> moved; // changes, offset
		for (uint32_t i = 0; i < fields.size(); i++)
			if (fields[i].changes > 0)
				moved.emplace_back(fields[i].changes, i);

		std::sort(moved.begin(), moved.end());

		std::printf("\noffsets that moved, quietest first:\n");
		if (moved.empty())
		{
			std::printf("  (nothing moved)\n");
			return;
		}
		for (const auto& [changes, offset] : moved)
		{
			std::printf("  +0x%03x  %6u change%s   last value %3u\n", offset, changes,
				changes == 1 ? " " : "s", fields[offset].last);
		}
		std::printf("\nA counter you can use steps a handful of times and stops.\n"
					"Anything in the thousands is a timer or a pointer.\n");
	}

	// -------------------------------------------------------------------------
	// Keeping the card alive while a diagnostic runs
	//
	// PINE serves one client at a time, so a sampling run starves drp's own
	// connection and the presence goes blank for exactly as long as you are
	// playing - which is when you want it most. Rather than make that a choice,
	// the sampler pushes the card itself, off the connection it already holds.
	//
	// Deliberately a cut-down copy of pcsx2::Watcher's composition rather than a
	// refactor of it: a research harness must not be able to break the presence
	// everybody actually uses. It also skips the skip_pcsx2_version check, since
	// you asked for this build by running the diagnostic against it.
	// -------------------------------------------------------------------------

	class SamplePresence
	{
	public:
		SamplePresence(std::string client_id, std::string large_image)
			: m_client_id(std::move(client_id)), m_large_image(std::move(large_image))
		{
		}

		/// Call once per sampling tick. It rate-limits itself, so the extra PINE
		/// traffic is a handful of reads every five seconds rather than four
		/// times a second on top of the sampling.
		void Update(pine::Client& emu)
		{
			if (m_client_id.empty())
				return;

			const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
									.count();
			if (m_last_push != 0 && (now - m_last_push) < PUSH_INTERVAL_MS)
				return;
			m_last_push = now;

			presence::Lines lines;
			if (!Compose(emu, lines) || lines.empty())
				return;

			if (!m_rpc)
			{
				auto client = std::make_unique<discord::Client>(m_client_id);
				if (!client->Connect())
					return;
				m_rpc = std::move(client);
				std::printf("-- Discord card connected as application %s --\n", m_client_id.c_str());
			}

			discord::Activity activity;
			activity.details = lines.details;
			activity.state = lines.state;
			activity.large_image = lines.large_image;
			activity.large_image_text = lines.large_image_text;
			activity.start_timestamp = lines.start_timestamp;

			if (m_rpc->SameAsCurrent(activity))
				return;
			std::printf("-- card: %s | %s --\n", activity.details.c_str(), activity.state.c_str());
			if (!m_rpc->SetActivity(activity))
				m_rpc.reset(); // next tick reconnects
		}

	private:
		static constexpr int64_t PUSH_INTERVAL_MS = 5000; // Discord drops faster updates

		bool Compose(pine::Client& emu, presence::Lines& out)
		{
			pine::Status status = pine::Shutdown;
			if (!emu.GetStatus(status) || status == pine::Shutdown)
				return false;

			std::string serial;
			if (!emu.GetSerial(serial) || serial.empty())
				return false;

			if (serial != m_serial)
			{
				m_serial = serial;
				m_provider = games::Find(serial);
				if (m_provider && m_provider->name)
					m_title = m_provider->name;
				else if (!emu.GetTitle(m_title))
					m_title = serial;
				m_started = std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::system_clock::now().time_since_epoch())
								.count();
			}

			out.details = m_title;
			out.large_image = m_large_image;
			out.large_image_text = m_title;
			out.start_timestamp = m_started;

			games::Presence p;
			if (m_provider && m_provider->read(emu, p) && !p.empty())
			{
				out.details = presence::Join(m_title, p.mode);
				out.state = p.map.empty() ? p.activity : presence::Join(p.map, p.session);
			}
			return true;
		}

		std::string m_client_id;
		std::string m_large_image;
		std::unique_ptr<discord::Client> m_rpc;
		std::string m_serial;
		std::string m_title;
		const games::Provider* m_provider = nullptr;
		int64_t m_started = 0;
		int64_t m_last_push = 0;
	};

	/// at   0 means "follow the game manager pointer", anything else is a fixed
	///      address sampled directly.
	/// len  window size in bytes.
	inline int Sample(int slot, uint32_t at, uint32_t len, uint32_t seconds = 0,
		std::string client_id = std::string(), std::string large_image = std::string())
	{
		// Unbuffered, because this is meant to be read while it runs. Redirected
		// to a file the CRT buffers 4 KB at a time, which holds findings back for
		// minutes and makes a working run look like a dead one.
		std::setvbuf(stdout, nullptr, _IONBF, 0);

		pine::Client emu(slot);
		if (!emu.Connect())
		{
			std::printf("Could not reach PCSX2 on PINE slot %d.\n"
						"Is it running with a game loaded, and is drp itself stopped?\n"
						"PINE serves one client at a time, so drp holds it while it runs.\n",
				slot);
			return 1;
		}

		// How many times an offset may change before it is written off as a
		// timer or a pointer and silenced.
		constexpr uint32_t NOISY_AFTER = 6;
		constexpr int INTERVAL_MS = 250;

		const bool follow = (at == 0);
		std::printf("Sampling %u bytes %s, every %d ms.\n", len,
			follow ? "from the game manager" : "from a fixed address", INTERVAL_MS);
		std::printf("Play normally. An offset is reported the first %u times it moves, then\n"
					"silenced; the tally at the end ranks everything by how often it moved.\n",
			NOISY_AFTER);
		if (!client_id.empty())
			std::printf("The Discord card is pushed from here too, so the presence does not\n"
					"go blank while you play.\n");
		if (seconds)
			std::printf("Stopping after %u seconds.\n\n", seconds);
		else
			std::printf("Ctrl+C when you are done.\n\n");

		SamplePresence card(std::move(client_id), std::move(large_image));
		std::vector<SampleField> fields;
		std::vector<uint8_t> now;
		uint32_t base = 0;
		std::string last_level;
		bool primed = false;
		bool read_failed = false;

		// Local time, to match drp.log: these get read side by side.
		const auto stamp = []() {
			SYSTEMTIME st;
			GetLocalTime(&st);
			std::printf("%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond);
		};

		SetConsoleCtrlHandler(SampleCtrlHandler, TRUE);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds ? seconds : 0);

		while (!g_sample_stop)
		{
			if (seconds && std::chrono::steady_clock::now() >= deadline)
				break;

			std::this_thread::sleep_for(std::chrono::milliseconds(INTERVAL_MS));

			if (!emu.IsConnected() && !emu.Connect())
				continue;

			// Ahead of anything that can continue: the card should keep ticking
			// even on a tick where the window itself could not be read.
			card.Update(emu);

			// Context, so a log line says what was happening when it moved.
			const std::string level = emu.ReadString(games::r6three::ADDR_LEVEL_PACKAGE, 40);
			if (level != last_level)
			{
				last_level = level;
				stamp();
				std::printf("-- level is now %s --\n", level.empty() ? "(none)" : level.c_str());
				primed = false; // a new level relays the struct; do not report the churn
			}

			uint32_t address = at;
			if (follow)
			{
				uint32_t manager = 0;
				if (!emu.ReadU32(games::r6three::ADDR_GAME_MANAGER, manager) || manager == 0)
					continue;
				address = manager;
				if (manager != base)
				{
					base = manager;
					stamp();
					std::printf("-- game manager moved to %08x, baseline reset --\n", manager);
					primed = false;
				}
			}

			if (!emu.ReadBytes(address, len, now) || now.size() != len)
			{
				// Silence here was hiding a dead read and looking like "nothing
				// ever changes", so say it out loud, once per run of failures.
				if (!read_failed)
				{
					read_failed = true;
					stamp();
					std::printf("!! read of %u bytes at %08x failed; reconnecting\n", len, address);
				}
				emu.Disconnect();
				continue;
			}
			read_failed = false;

			if (!primed)
			{
				fields.assign(len, SampleField());
				for (uint32_t i = 0; i < len; i++)
					fields[i].last = now[i];
				primed = true;
				stamp();
				std::printf("-- baseline taken at %08x, %u bytes --\n", address, len);
				continue;
			}

			for (uint32_t i = 0; i < len; i++)
			{
				if (now[i] == fields[i].last)
					continue;

				const uint8_t was = fields[i].last;
				fields[i].last = now[i];
				fields[i].changes++;

				if (fields[i].noisy)
					continue;
				if (fields[i].changes > NOISY_AFTER)
				{
					fields[i].noisy = true;
					stamp();
					std::printf("+0x%03x  changing constantly, silencing it\n", i);
					continue;
				}

				stamp();
				std::printf("+0x%03x  %3u -> %-3u  (change %u)\n", i, was, now[i], fields[i].changes);
			}
		}

		SampleSummary(fields);
		return 0;
	}

} // namespace pcsx2
