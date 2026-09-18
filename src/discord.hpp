// SPDX-License-Identifier: MIT
//
// Discord Rich Presence over the local IPC socket, without discord-rpc.
//
// The desktop client listens on \\.\pipe\discord-ipc-0 .. -9 (one per running
// client). Frames are `u32 opcode, u32 length, UTF-8 JSON`:
//
//   op 0  HANDSHAKE   {"v":1,"client_id":"..."}      -> op 1 READY
//   op 1  FRAME       {"cmd":"SET_ACTIVITY", ...}
//   op 2  CLOSE
//   op 3  PING                                       -> op 4 PONG
//
// Replies are read and discarded apart from noticing that the pipe died; there
// is nothing in them this tool needs.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace discord
{
	/// JSON string escaping, enough for the characters a presence line can hold.
	inline std::string Escape(const std::string& in)
	{
		std::string out;
		out.reserve(in.size() + 8);
		for (const char c : in)
		{
			switch (c)
			{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (static_cast<unsigned char>(c) < 0x20)
						out += ' ';
					else
						out += c;
					break;
			}
		}
		return out;
	}

	struct Activity
	{
		std::string details;         // line one
		std::string state;           // line two
		std::string large_image;     // asset key on the Discord application
		std::string large_image_text;
		std::string small_image;     // the badge over the corner of the large one
		std::string small_image_text;
		int64_t start_timestamp = 0; // unix seconds, 0 to omit the elapsed timer

		bool operator==(const Activity& o) const
		{
			return details == o.details && state == o.state && large_image == o.large_image &&
				   large_image_text == o.large_image_text && small_image == o.small_image &&
				   small_image_text == o.small_image_text && start_timestamp == o.start_timestamp;
		}
		bool operator!=(const Activity& o) const { return !(*this == o); }
	};

	class Client
	{
	public:
		explicit Client(std::string client_id)
			: m_client_id(std::move(client_id))
		{
		}

		~Client() { Disconnect(); }

		bool IsConnected() const { return m_pipe != INVALID_HANDLE_VALUE; }

		/// Tries each of the ten well-known pipes and handshakes with the first
		/// that answers.
		bool Connect()
		{
			Disconnect();

			for (int i = 0; i < 10; i++)
			{
				char path[64];
				std::snprintf(path, sizeof(path), "\\\\.\\pipe\\discord-ipc-%d", i);

				HANDLE pipe = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
				if (pipe == INVALID_HANDLE_VALUE)
					continue;

				m_pipe = pipe;
				if (Write(0, "{\"v\":1,\"client_id\":\"" + Escape(m_client_id) + "\"}") && ReadFrame())
					return true;

				Disconnect();
			}
			return false;
		}

		void Disconnect()
		{
			if (m_pipe != INVALID_HANDLE_VALUE)
			{
				CloseHandle(m_pipe);
				m_pipe = INVALID_HANDLE_VALUE;
			}
			m_current = {};
			m_have_current = false;
		}

		/// Pushes the activity, or clears it when `activity` has no text at all.
		/// Returns false when the pipe died and the caller should reconnect.
		bool SetActivity(const Activity& activity)
		{
			if (!IsConnected())
				return false;

			std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + std::to_string(++m_nonce) +
								  "\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId());

			if (activity.details.empty() && activity.state.empty() && activity.large_image.empty() &&
				activity.small_image.empty())
			{
				payload += ",\"activity\":null}}";
			}
			else
			{
				payload += ",\"activity\":{";
				bool first = true;
				const auto field = [&](const char* name, const std::string& value) {
					if (value.empty())
						return;
					if (!first)
						payload += ',';
					first = false;
					payload += std::string("\"") + name + "\":\"" + Escape(value) + "\"";
				};

				field("details", activity.details);
				field("state", activity.state);

				if (activity.start_timestamp != 0)
				{
					if (!first)
						payload += ',';
					first = false;
					payload += "\"timestamps\":{\"start\":" + std::to_string(activity.start_timestamp) + "}";
				}

				if (!activity.large_image.empty() || !activity.small_image.empty())
				{
					if (!first)
						payload += ',';
					first = false;
					payload += "\"assets\":{";
					bool first_asset = true;
					const auto asset = [&](const char* key, const std::string& value) {
						if (value.empty())
							return;
						if (!first_asset)
							payload += ',';
						first_asset = false;
						payload += std::string("\"") + key + "\":\"" + Escape(value) + "\"";
					};
					asset("large_image", activity.large_image);
					asset("large_text", activity.large_image_text);
					asset("small_image", activity.small_image);
					asset("small_text", activity.small_image_text);
					payload += "}";
				}

				payload += "}}}";
			}

			if (!Write(1, payload))
			{
				Disconnect();
				return false;
			}

			// Drain whatever the client says back so the pipe does not fill up.
			DrainReplies();

			m_current = activity;
			m_have_current = true;
			return true;
		}

		/// The activity the client is currently showing, so a caller can avoid
		/// pushing the same thing twice.
		bool SameAsCurrent(const Activity& activity) const { return m_have_current && m_current == activity; }

	private:
		bool Write(uint32_t opcode, const std::string& payload)
		{
			if (m_pipe == INVALID_HANDLE_VALUE)
				return false;

			std::string frame;
			frame.resize(8 + payload.size());
			const uint32_t length = static_cast<uint32_t>(payload.size());
			std::memcpy(&frame[0], &opcode, 4);
			std::memcpy(&frame[4], &length, 4);
			std::memcpy(&frame[8], payload.data(), payload.size());

			DWORD written = 0;
			return WriteFile(m_pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) &&
				   written == frame.size();
		}

		bool ReadFrame()
		{
			uint32_t header[2] = {};
			DWORD read = 0;
			if (!ReadFile(m_pipe, header, sizeof(header), &read, nullptr) || read != sizeof(header))
				return false;
			if (header[1] > (1u << 20))
				return false;

			std::string body(header[1], '\0');
			if (header[1] > 0 && (!ReadFile(m_pipe, body.data(), header[1], &read, nullptr) || read != header[1]))
				return false;

			return true;
		}

		/// Reads any frames already waiting, without blocking on an empty pipe.
		void DrainReplies()
		{
			for (;;)
			{
				DWORD available = 0;
				if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &available, nullptr))
				{
					Disconnect();
					return;
				}
				if (available < 8)
					return;
				if (!ReadFrame())
				{
					Disconnect();
					return;
				}
			}
		}

		std::string m_client_id;
		HANDLE m_pipe = INVALID_HANDLE_VALUE;
		uint64_t m_nonce = 0;
		Activity m_current;
		bool m_have_current = false;
	};
} // namespace discord
