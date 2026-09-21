// SPDX-License-Identifier: MIT
//
// Diagnostic: exercise the real pcsx2::Watcher against a running PCSX2 and say
// exactly which step fails. Build with build_probe.bat, run from a console.

#include "pcsx2_watcher.hpp"
#include "presence.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
	const int slot = (argc > 1) ? std::atoi(argv[1]) : pine::DEFAULT_SLOT;
	std::printf("PINE slot %d\n", slot);

	// Deliberately optional, to show what difference it makes: pass "nowsa" to
	// skip it and reproduce what drp itself was doing.
	const bool do_wsa = !(argc > 2 && std::string(argv[2]) == "nowsa");
	if (do_wsa)
	{
		WSADATA wsa = {};
		const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
		std::printf("WSAStartup()   %s (rc=%d)\n", rc == 0 ? "ok" : "FAILED", rc);
	}
	else
	{
		std::printf("WSAStartup()   SKIPPED\n");
	}

	// Raw attempt first, so a failure says which call and which error.
	{
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		std::printf("socket()       %s", s == INVALID_SOCKET ? "FAILED" : "ok");
		if (s == INVALID_SOCKET)
			std::printf(" WSAGetLastError=%d", WSAGetLastError());
		std::printf("\n");
		if (s != INVALID_SOCKET)
		{
			sockaddr_in addr = {};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(static_cast<u_short>(slot));
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			const int rc = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
			std::printf("connect()      %s", rc == 0 ? "ok" : "FAILED");
			if (rc != 0)
				std::printf(" WSAGetLastError=%d", WSAGetLastError());
			std::printf("\n");
			closesocket(s);
		}
	}

	pine::Client emu(slot);
	if (!emu.Connect())
	{
		std::printf("Connect()      FAILED\n");
		return 1;
	}
	std::printf("Connect()      ok\n");

	pine::Status status = pine::Shutdown;
	std::printf("GetStatus()    %s", emu.GetStatus(status) ? "ok" : "FAILED");
	std::printf("  -> status=%d (0 running, 1 paused, 2 shutdown)\n", static_cast<int>(status));

	std::string serial;
	const bool got_serial = emu.GetSerial(serial);
	std::printf("GetSerial()    %s  -> %s (len %zu)\n", got_serial ? "ok" : "FAILED",
		serial.c_str(), serial.size());

	std::string title;
	std::printf("GetTitle()     %s  -> %s\n", emu.GetTitle(title) ? "ok" : "FAILED", title.c_str());

	emu.Disconnect();

	// Third argument is the skip_pcsx2_version match; default "remix" as drp
	// ships, pass "" to watch every build.
	const std::string skip = (argc > 3) ? argv[3] : "remix";
	std::printf("\nskip_pcsx2_version = %s\n", skip.empty() ? "(empty, watch everything)" : skip.c_str());

	pcsx2::Watcher watcher(slot, skip);
	presence::Lines lines;
	const bool polled = watcher.Poll(lines);
	std::printf("  version      = %s\n", watcher.Version().c_str());
	std::printf("  standing aside = %s\n", watcher.StandingAside() ? "YES" : "no");
	if (!watcher.Note().empty())
		std::printf("  note         = %s\n", watcher.Note().c_str());
	std::printf("Watcher::Poll() -> %s\n", polled ? "TRUE (presence would show)" : "FALSE (shows nothing)");
	std::printf("  serial    = %s\n", watcher.Serial().c_str());
	std::printf("  title     = %s\n", watcher.Title().c_str());
	std::printf("  supported = %s\n", watcher.Supported() ? "yes, a provider matched" : "NO PROVIDER for this serial");
	std::printf("  details   = %s\n", lines.details.c_str());
	std::printf("  state     = %s\n", lines.state.c_str());
	return polled ? 0 : 2;
}
