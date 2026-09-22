// SPDX-License-Identifier: MIT
//
// A tray balloon, so an achievement has a moment rather than a log line.
//
// Deliberately the oldest, dullest mechanism Windows has: a notification-area
// icon and Shell_NotifyIcon. It needs no manifest, no AppUserModelID, no
// packaged identity and no WinRT, all of which modern toasts want before they
// will show anything for an unsigned exe run from a folder.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <shellapi.h>

#include <string>

namespace toast
{
	class Tray
	{
	public:
		/// Fails quietly: a presence tool that cannot show a balloon should
		/// still show a presence.
		bool Open(const std::string& tooltip)
		{
			WNDCLASSEXA wc = {};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = DefWindowProcA;
			wc.hInstance = GetModuleHandleA(nullptr);
			wc.lpszClassName = "drp_tray";
			RegisterClassExA(&wc);

			// Message-only: no taskbar button, nothing on screen, just an owner
			// for the icon.
			m_window = CreateWindowExA(0, "drp_tray", "drp", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
				wc.hInstance, nullptr);
			if (!m_window)
				return false;

			m_icon.cbSize = sizeof(m_icon);
			m_icon.hWnd = m_window;
			m_icon.uID = 1;
			m_icon.uFlags = NIF_ICON | NIF_TIP;
			m_icon.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
			lstrcpynA(m_icon.szTip, tooltip.c_str(), sizeof(m_icon.szTip));
			m_open = Shell_NotifyIconA(NIM_ADD, &m_icon) != FALSE;
			return m_open;
		}

		/// The balloon queue is the shell's, not ours, so two unlocks a second
		/// apart still both appear - it shows them one after another.
		void Show(const std::string& title, const std::string& text)
		{
			if (!m_open)
				return;

			m_icon.uFlags = NIF_INFO;
			m_icon.dwInfoFlags = NIIF_INFO;
			lstrcpynA(m_icon.szInfoTitle, title.c_str(), sizeof(m_icon.szInfoTitle));
			lstrcpynA(m_icon.szInfo, text.c_str(), sizeof(m_icon.szInfo));
			Shell_NotifyIconA(NIM_MODIFY, &m_icon);
		}

		/// The shell talks to the icon's owner window, and an owner that never
		/// reads its queue is a window that stops working. Called once a tick.
		void Pump()
		{
			MSG msg;
			while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageA(&msg);
			}
		}

		~Tray()
		{
			if (m_open)
				Shell_NotifyIconA(NIM_DELETE, &m_icon);
			if (m_window)
				DestroyWindow(m_window);
		}

	private:
		HWND m_window = nullptr;
		NOTIFYICONDATAA m_icon = {};
		bool m_open = false;
	};
} // namespace toast
