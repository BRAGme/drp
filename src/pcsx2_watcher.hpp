// SPDX-License-Identifier: MIT
//
// Watching PCSX2 over its PINE server, and turning what the running disc says
// into the two lines a presence shows.

#pragma once

#include "pine.hpp"

#include "games.hpp"
#include "presence.hpp"

#include <cctype>
#include <chrono>
#include <string>
#include <utility>

namespace pcsx2
{
	/// Case-insensitive substring test, for matching a build by its version.
	inline bool ContainsNoCase(const std::string& haystack, const std::string& needle)
	{
		if (needle.empty() || needle.size() > haystack.size())
			return false;
		const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
		for (size_t i = 0; i + needle.size() <= haystack.size(); i++)
		{
			size_t j = 0;
			while (j < needle.size() && lower(haystack[i + j]) == lower(needle[j]))
				j++;
			if (j == needle.size())
				return true;
		}
		return false;
	}

	class Watcher
	{
	public:
		/// skip_version: stand aside when the emulator's version string contains
		/// this, because that build shows a detailed presence of its own and two
		/// of us would fight over the same Discord application. Empty disables
		/// the check.
		explicit Watcher(int slot, std::string skip_version = std::string())
			: m_emu(slot), m_slot(slot), m_skip_version(std::move(skip_version))
		{
		}

		int Slot() const { return m_slot; }
		bool Connected() const { return m_emu.IsConnected(); }
		pine::Client& Emu() { return m_emu; }

		/// One line worth logging when it changes; empty when there is nothing
		/// to say. Mirrors the Mercenaries watcher.
		const std::string& Note() const { return m_note; }

		/// True when PCSX2 is there with a game running. Cheap when it is not:
		/// a refused loopback connect, no more.
		bool Poll(presence::Lines& out)
		{
			if (!m_emu.IsConnected())
			{
				// Anything learned about the old connection is stale now.
				m_version.clear();
				m_stand_aside = false;
				m_note.clear();

				if (!m_emu.Connect())
				{
					m_serial.clear();
					return false;
				}
			}

			// Ask once per connection which build this is. A build with its own
			// detailed presence gets left alone: it and drp would otherwise
			// connect to Discord as the same application and overwrite each
			// other's card every few seconds.
			if (m_version.empty())
			{
				if (!m_emu.GetVersion(m_version) || m_version.empty())
				{
					m_emu.Disconnect();
					m_serial.clear();
					return false;
				}

				m_stand_aside = ContainsNoCase(m_version, m_skip_version);
				m_note = m_stand_aside
							 ? (m_version + " shows its own detailed presence, so drp is leaving it alone.")
							 : std::string();
			}

			if (m_stand_aside)
			{
				m_serial.clear();
				m_provider = nullptr;
				return false;
			}

			pine::Status status = pine::Shutdown;
			if (!m_emu.GetStatus(status))
			{
				m_emu.Disconnect();
				m_serial.clear();
				return false;
			}

			std::string serial;
			if (status == pine::Shutdown || !m_emu.GetSerial(serial) || serial.empty())
			{
				m_serial.clear();
				m_provider = nullptr;
				return false;
			}

			if (serial != m_serial)
			{
				m_serial = serial;
				m_provider = games::Find(serial);
				if (m_provider && m_provider->name)
					m_title = m_provider->name;
				else if (!m_emu.GetTitle(m_title))
					m_title = serial;
				m_started = std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::system_clock::now().time_since_epoch())
								.count();
			}

			out.details = m_title;
			out.state.clear();
			out.large_image_text = m_title;
			out.start_timestamp = m_started;

			games::Presence p;
			if (m_provider && m_provider->read(m_emu, p) && !p.empty())
			{
				out.details = presence::Join(m_title, p.mode);
				out.state = p.map.empty() ? p.activity : presence::Join(p.map, p.session);
			}
			return true;
		}

		const std::string& Serial() const { return m_serial; }
		const std::string& Title() const { return m_title; }
		bool Supported() const { return m_provider != nullptr; }

		const std::string& Version() const { return m_version; }
		bool StandingAside() const { return m_stand_aside; }

	private:
		pine::Client m_emu;
		int m_slot;
		std::string m_skip_version;
		std::string m_version;
		std::string m_note;
		bool m_stand_aside = false;
		std::string m_serial;
		std::string m_title;
		const games::Provider* m_provider = nullptr;
		int64_t m_started = 0;
	};
} // namespace pcsx2
