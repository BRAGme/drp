// SPDX-License-Identifier: MIT
//
// A minimal client for PCSX2's PINE IPC server.
//
// PINE is built into every PCSX2 since 1.7 and is off by default; the user turns
// it on under Settings -> Advanced -> Enable PINE. On Windows the server is a
// TCP listener on 127.0.0.1:<slot> (28011 unless changed), on other platforms a
// unix socket. Only the Windows transport is implemented here.
//
// Framing, from PCSX2's PINE.cpp:
//
//   request  u32 total_size (counts itself) then one or more commands
//   command  u8 opcode, then the opcode's arguments
//   reply    u32 total_size (counts itself), u8 result (0 ok / 0xFF fail), payload
//
// Several commands can share one request, and their payloads come back
// concatenated in order. That matters here: PINE has no bulk memory read, so a
// string is fetched as a batch of eight-byte reads in a single round trip.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

namespace pine
{
	enum Command : uint8_t
	{
		MsgRead8 = 0,
		MsgRead16 = 1,
		MsgRead32 = 2,
		MsgRead64 = 3,
		MsgVersion = 8,
		MsgTitle = 0x0B,
		MsgID = 0x0C,
		MsgUUID = 0x0D,
		MsgGameVersion = 0x0E,
		MsgStatus = 0x0F,
	};

	enum Status : uint32_t
	{
		Running = 0,
		Paused = 1,
		Shutdown = 2,
	};

	constexpr int DEFAULT_SLOT = 28011;

	class Client
	{
	public:
		explicit Client(int slot = DEFAULT_SLOT)
			: m_slot(slot)
		{
		}

		~Client() { Disconnect(); }

		bool IsConnected() const { return m_sock != INVALID_SOCKET; }

		bool Connect()
		{
			Disconnect();

			m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (m_sock == INVALID_SOCKET)
				return false;

			sockaddr_in addr = {};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(static_cast<u_short>(m_slot));
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

			if (connect(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
			{
				Disconnect();
				return false;
			}

			// A stalled emulator must not stall us; every call gives up quickly
			// and the caller just retries on the next tick.
			DWORD timeout = 2000;
			setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
			setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
			return true;
		}

		void Disconnect()
		{
			if (m_sock != INVALID_SOCKET)
			{
				closesocket(m_sock);
				m_sock = INVALID_SOCKET;
			}
		}

		/// Emulator status. Unlike every other command this one answers even when
		/// no game is loaded, so it doubles as the "is PCSX2 there" probe.
		bool GetStatus(Status& out)
		{
			std::vector<uint8_t> reply;
			if (!Send({MsgStatus}, reply) || reply.size() < 4)
				return false;

			out = static_cast<Status>(ReadU32(reply, 0));
			return true;
		}

		/// The disc serial, e.g. "SLUS-20883". Fails when no game is running.
		bool GetSerial(std::string& out) { return GetString(MsgID, out); }

		/// The game's name as PCSX2's game list knows it.
		bool GetTitle(std::string& out) { return GetString(MsgTitle, out); }

		bool ReadU8(uint32_t addr, uint8_t& out)
		{
			std::vector<uint8_t> reply;
			std::vector<uint8_t> req = {MsgRead8};
			AppendU32(req, addr);
			if (!Send(req, reply) || reply.size() < 1)
				return false;
			out = reply[0];
			return true;
		}

		bool ReadU32(uint32_t addr, uint32_t& out)
		{
			std::vector<uint8_t> reply;
			std::vector<uint8_t> req = {MsgRead32};
			AppendU32(req, addr);
			if (!Send(req, reply) || reply.size() < 4)
				return false;
			out = ReadU32(reply, 0);
			return true;
		}

		/// `count` bytes starting at `addr`.
		///
		/// PINE has no bulk read, so this batches eight-byte reads: one round
		/// trip fetches up to 320 KB. PCSX2 caps a request at 650,000 bytes and
		/// a reply at 450,000, and silently drops anything larger, hence the
		/// chunk size rather than one giant request.
		bool ReadBytes(uint32_t addr, uint32_t count, std::vector<uint8_t>& out)
		{
			constexpr uint32_t WORDS_PER_REQUEST = 40000; // 200 KB out, 320 KB back

			out.clear();
			out.reserve(count);

			const uint32_t total_words = (count + 7) / 8;
			for (uint32_t done = 0; done < total_words;)
			{
				const uint32_t words = std::min(WORDS_PER_REQUEST, total_words - done);

				std::vector<uint8_t> req;
				req.reserve(words * 5);
				for (uint32_t i = 0; i < words; i++)
				{
					req.push_back(MsgRead64);
					AppendU32(req, addr + (done + i) * 8);
				}

				std::vector<uint8_t> reply;
				if (!Send(req, reply) || reply.size() < words * 8)
					return false;

				out.insert(out.end(), reply.begin(), reply.begin() + words * 8);
				done += words;
			}

			out.resize(count);
			return true;
		}

		/// A NUL-terminated ASCII string out of emulated memory. Anything that is
		/// not plain printable ASCII comes back empty, so a stale or wild pointer
		/// cannot put binary into a presence string.
		std::string ReadString(uint32_t addr, uint32_t max_length)
		{
			std::vector<uint8_t> bytes;
			if (!ReadBytes(addr, max_length, bytes))
				return {};

			std::string out;
			for (const uint8_t ch : bytes)
			{
				if (ch == 0)
					return out;
				if (ch < 0x20 || ch >= 0x7F)
					return {};
				out.push_back(static_cast<char>(ch));
			}
			return {};
		}

	private:
		static void AppendU32(std::vector<uint8_t>& buf, uint32_t value)
		{
			buf.push_back(static_cast<uint8_t>(value));
			buf.push_back(static_cast<uint8_t>(value >> 8));
			buf.push_back(static_cast<uint8_t>(value >> 16));
			buf.push_back(static_cast<uint8_t>(value >> 24));
		}

		static uint32_t ReadU32(const std::vector<uint8_t>& buf, size_t offset)
		{
			return static_cast<uint32_t>(buf[offset]) | (static_cast<uint32_t>(buf[offset + 1]) << 8) |
				   (static_cast<uint32_t>(buf[offset + 2]) << 16) | (static_cast<uint32_t>(buf[offset + 3]) << 24);
		}

		bool GetString(Command cmd, std::string& out)
		{
			std::vector<uint8_t> reply;
			if (!Send({static_cast<uint8_t>(cmd)}, reply) || reply.size() < 4)
				return false;

			const uint32_t size = ReadU32(reply, 0);
			if (size == 0 || size > reply.size() - 4)
				return false;

			out.assign(reinterpret_cast<const char*>(reply.data() + 4), size - 1);
			return true;
		}

		/// Sends one request and returns the payload after the result byte.
		/// Any protocol error drops the connection; the caller reconnects.
		bool Send(const std::vector<uint8_t>& commands, std::vector<uint8_t>& payload)
		{
			if (!IsConnected())
				return false;

			std::vector<uint8_t> request;
			request.reserve(commands.size() + 4);
			AppendU32(request, static_cast<uint32_t>(commands.size() + 4));
			request.insert(request.end(), commands.begin(), commands.end());

			if (!SendAll(request.data(), request.size()))
			{
				Disconnect();
				return false;
			}

			uint8_t header[4];
			if (!RecvAll(header, sizeof(header)))
			{
				Disconnect();
				return false;
			}

			const uint32_t total = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
								   (static_cast<uint32_t>(header[2]) << 16) | (static_cast<uint32_t>(header[3]) << 24);
			if (total < 5 || total > (1u << 22))
			{
				Disconnect();
				return false;
			}

			std::vector<uint8_t> body(total - 4);
			if (!RecvAll(body.data(), body.size()))
			{
				Disconnect();
				return false;
			}

			if (body[0] != 0) // IPC_FAIL: usually just "no game running"
				return false;

			payload.assign(body.begin() + 1, body.end());
			return true;
		}

		bool SendAll(const uint8_t* data, size_t size)
		{
			size_t sent = 0;
			while (sent < size)
			{
				const int n = send(m_sock, reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), 0);
				if (n <= 0)
					return false;
				sent += static_cast<size_t>(n);
			}
			return true;
		}

		bool RecvAll(uint8_t* data, size_t size)
		{
			size_t got = 0;
			while (got < size)
			{
				const int n = recv(m_sock, reinterpret_cast<char*>(data + got), static_cast<int>(size - got), 0);
				if (n <= 0)
					return false;
				got += static_cast<size_t>(n);
			}
			return true;
		}

		int m_slot;
		SOCKET m_sock = INVALID_SOCKET;
	};
} // namespace pine
