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
		/// How long to wait for a reply Discord owes us before giving up and
		/// reconnecting. Short: a healthy client answers in microseconds, and
		/// everything else in the tool is stopped while we wait.
		static constexpr DWORD kReplyTimeoutMs = 2000;

	public:
		explicit Client(std::string client_id)
			: m_client_id(std::move(client_id))
		{
		}

		~Client()
		{
			Disconnect();
			if (m_event != nullptr)
			{
				CloseHandle(m_event);
				m_event = nullptr;
			}
		}

		bool IsConnected() const { return m_pipe != INVALID_HANDLE_VALUE; }

		/// Tries each of the ten well-known pipes and handshakes with the first
		/// that answers.
		bool Connect()
		{
			Disconnect();

			if (m_event == nullptr)
			{
				m_event = CreateEventA(nullptr, TRUE, FALSE, nullptr); // manual reset
				if (m_event == nullptr)
					return false;
			}

			for (int i = 0; i < 10; i++)
			{
				char path[64];
				std::snprintf(path, sizeof(path), "\\\\.\\pipe\\discord-ipc-%d", i);

				// FILE_FLAG_OVERLAPPED so every read and write can be given a
				// deadline. Without it these are blocking calls with no timeout
				// of any kind, and one unresponsive pipe stops the whole tool.
				HANDLE pipe = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
					FILE_FLAG_OVERLAPPED, nullptr);
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
		/// One overlapped read or write, with a deadline. On timeout the request
		/// is cancelled and the caller drops the connection, which is always
		/// recoverable: the next tick reconnects.
		bool TimedIo(bool writing, void* buffer, DWORD length, DWORD timeout_ms)
		{
			if (m_pipe == INVALID_HANDLE_VALUE || m_event == nullptr || length == 0)
				return false;

			OVERLAPPED ov = {};
			ov.hEvent = m_event;
			ResetEvent(m_event);

			DWORD done = 0;
			const BOOL ok = writing
								? WriteFile(m_pipe, buffer, length, &done, &ov)
								: ReadFile(m_pipe, buffer, length, &done, &ov);
			if (!ok)
			{
				if (GetLastError() != ERROR_IO_PENDING)
					return false;
				if (WaitForSingleObject(m_event, timeout_ms) != WAIT_OBJECT_0)
				{
					CancelIoEx(m_pipe, &ov);
					WaitForSingleObject(m_event, 1000); // let the cancellation land
					return false;
				}
				if (!GetOverlappedResult(m_pipe, &ov, &done, FALSE))
					return false;
			}
			return done == length;
		}

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

			return TimedIo(true, frame.data(), static_cast<DWORD>(frame.size()), kReplyTimeoutMs);
		}

		bool ReadFrame()
		{
			uint32_t header[2] = {};
			if (!TimedIo(false, header, sizeof(header), kReplyTimeoutMs))
				return false;
			if (header[1] > (1u << 20))
				return false;

			std::string body(header[1], '\0');
			if (header[1] > 0 && !TimedIo(false, body.data(), header[1], kReplyTimeoutMs))
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
		HANDLE m_event = nullptr; // reused by every overlapped read and write
		uint64_t m_nonce = 0;
		Activity m_current;
		bool m_have_current = false;
	};
} // namespace discord
