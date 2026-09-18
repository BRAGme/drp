// SPDX-License-Identifier: MIT
//
// Watching PCSX2 over its PINE server, and turning what the running disc says
// into the two lines a presence shows.

#pragma once

#include "pine.hpp"

#include "games.hpp"
#include "presence.hpp"

#include <chrono>
#include <string>

namespace pcsx2
{
	class Watcher
	{
	public:
		explicit Watcher(int slot)
			: m_emu(slot), m_slot(slot)
		{
		}

		int Slot() const { return m_slot; }
		bool Connected() const { return m_emu.IsConnected(); }
		pine::Client& Emu() { return m_emu; }

		/// True when PCSX2 is there with a game running. Cheap when it is not:
		/// a refused loopback connect, no more.
		bool Poll(presence::Lines& out)
		{
			if (!m_emu.IsConnected() && !m_emu.Connect())
			{
				m_serial.clear();
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
				if (!m_emu.GetTitle(m_title))
					m_title = serial;
				m_started = std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::system_clock::now().time_since_epoch())
								.count();
			}

			out.details = m_title;
			out.state.clear();
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

	private:
		pine::Client m_emu;
		int m_slot;
		std::string m_serial;
		std::string m_title;
		const games::Provider* m_provider = nullptr;
		int64_t m_started = 0;
	};
} // namespace pcsx2
