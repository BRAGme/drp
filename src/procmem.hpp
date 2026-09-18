// SPDX-License-Identifier: MIT
//
// Reading another process's memory, and finding objects in it by vtable.
//
// For a native game there is no emulator to ask, so the tool attaches to the
// process with PROCESS_VM_READ and reads it directly. Nothing is ever written.
//
// The trick that makes this workable without a map of the game's globals: the
// binary was built with RTTI, so every class's vtable sits at a known offset
// inside the image. A live object of class C is any address whose first word is
// (module base + C's vtable RVA). Scanning for that finds singletons without
// knowing a single global's address, and it survives ASLR because the base is
// read at runtime.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace procmem
{
	class Process
	{
	public:
		~Process() { Detach(); }

		bool IsAttached() const { return m_handle != nullptr; }
		uintptr_t ModuleBase() const { return m_module_base; }
		uint32_t ModuleSize() const { return m_module_size; }
		const std::string& ExeName() const { return m_exe_name; }

		/// Attaches to the first process with this image name, e.g.
		/// "Mercenaries.exe". Returns false when it is not running, or when the
		/// handle cannot be opened.
		bool Attach(const std::string& exe_name)
		{
			Detach();

			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (snap == INVALID_HANDLE_VALUE)
				return false;

			DWORD pid = 0;
			PROCESSENTRY32 pe = {sizeof(pe)};
			if (Process32First(snap, &pe))
			{
				do
				{
					if (_stricmp(pe.szExeFile, exe_name.c_str()) == 0)
					{
						pid = pe.th32ProcessID;
						break;
					}
				} while (Process32Next(snap, &pe));
			}
			CloseHandle(snap);
			if (pid == 0)
				return false;

			m_handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
			if (!m_handle)
				return false;

			if (!FindModule(pid, exe_name))
			{
				Detach();
				return false;
			}

			m_pid = pid;
			m_exe_name = exe_name;
			return true;
		}

		void Detach()
		{
			if (m_handle)
			{
				CloseHandle(m_handle);
				m_handle = nullptr;
			}
			m_pid = 0;
			m_module_base = 0;
			m_module_size = 0;
		}

		/// The full path of the running exe, so callers can find the game's data
		/// next to it instead of being told where it is.
		std::string ImagePath() const
		{
			if (!m_handle)
				return {};
			char buf[MAX_PATH] = {};
			DWORD size = MAX_PATH;
			if (!QueryFullProcessImageNameA(m_handle, 0, buf, &size))
				return {};
			return std::string(buf, size);
		}

		/// The folder the exe sits in, with a trailing slash.
		std::string ImageDir() const
		{
			const std::string path = ImagePath();
			const size_t slash = path.find_last_of("\\/");
			return (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
		}

		/// True while the process is still alive.
		bool IsAlive() const
		{
			if (!m_handle)
				return false;
			DWORD code = 0;
			return GetExitCodeProcess(m_handle, &code) && code == STILL_ACTIVE;
		}

		bool Read(uintptr_t address, void* dst, size_t size) const
		{
			if (!m_handle || address == 0)
				return false;
			SIZE_T got = 0;
			return ReadProcessMemory(m_handle, reinterpret_cast<LPCVOID>(address), dst, size, &got) && got == size;
		}

		uint32_t ReadU32(uintptr_t address, uint32_t fallback = 0) const
		{
			uint32_t v = fallback;
			return Read(address, &v, sizeof(v)) ? v : fallback;
		}

		uint8_t ReadU8(uintptr_t address, uint8_t fallback = 0) const
		{
			uint8_t v = fallback;
			return Read(address, &v, sizeof(v)) ? v : fallback;
		}

		float ReadFloat(uintptr_t address, float fallback = 0.0f) const
		{
			float v = fallback;
			return Read(address, &v, sizeof(v)) ? v : fallback;
		}

		/// A NUL-terminated ASCII string. Empty unless every byte is printable,
		/// so a stale pointer cannot put binary into a presence line.
		std::string ReadString(uintptr_t address, size_t max_length = 64) const
		{
			std::vector<char> buf(max_length + 1, 0);
			if (!Read(address, buf.data(), max_length))
			{
				// A short read at the end of a page is normal; try byte by byte.
				std::string out;
				for (size_t i = 0; i < max_length; i++)
				{
					char c = 0;
					if (!Read(address + i, &c, 1))
						return {};
					if (c == 0)
						return out;
					if (c < 0x20 || c >= 0x7F)
						return {};
					out.push_back(c);
				}
				return {};
			}

			std::string out;
			for (size_t i = 0; i < max_length; i++)
			{
				const char c = buf[i];
				if (c == 0)
					return out;
				if (c < 0x20 || c >= 0x7F)
					return {};
				out.push_back(c);
			}
			return {};
		}

		/// Every committed, readable region, as (base, size).
		///
		/// Guard pages and no-access regions are skipped, so a scan over these
		/// never trips the target. There is deliberately no size cap: this engine
		/// allocates one very large pool up front and an object we want lives
		/// inside it, so capping the region size loses the object entirely.
		/// Callers read these in chunks rather than whole.
		std::vector<std::pair<uintptr_t, size_t>> ReadableRegions(size_t max_region = SIZE_MAX) const
		{
			std::vector<std::pair<uintptr_t, size_t>> out;
			if (!m_handle)
				return out;

			MEMORY_BASIC_INFORMATION mbi = {};
			uintptr_t address = 0;
			while (VirtualQueryEx(m_handle, reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
			{
				const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
				if (next <= address)
					break;

				const DWORD prot = mbi.Protect & 0xFF;
				const bool readable = (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
										  prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE) &&
									  !(mbi.Protect & PAGE_GUARD);

				if (mbi.State == MEM_COMMIT && readable && mbi.RegionSize <= max_region)
					out.emplace_back(reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize);

				address = next;
			}
			return out;
		}


		/// Walks every readable region in bounded chunks, calling `fn(base, data,
		/// size)` for each. Signature-scanning callers use this so that a
		/// several-hundred-megabyte pool never becomes a buffer that size.
		template <typename Fn>
		void ForEachChunk(Fn&& fn, size_t overlap = 0) const
		{
			constexpr size_t CHUNK = 8u << 20;
			std::vector<uint8_t> buf(CHUNK);

			for (const auto& [base, size] : ReadableRegions())
			{
				for (size_t done = 0; done < size;)
				{
					const size_t want = (std::min)(CHUNK, size - done);
					SIZE_T got = 0;
					if (!ReadProcessMemory(m_handle, reinterpret_cast<LPCVOID>(base + done), buf.data(), want, &got) ||
						got == 0)
					{
						done += want;
						continue;
					}

					if (!fn(base + done, buf.data(), static_cast<size_t>(got)))
						return;

					// Step back a little so a signature spanning the seam is
					// still seen whole by the next chunk.
					done += (got > overlap) ? (got - overlap) : got;
				}
			}
		}

		/// Addresses whose first word equals `value`. This is how an object of a
		/// known class is found: pass (module base + vtable RVA).
		///
		/// `limit` stops the sweep once that many have been found, which for a
		/// singleton means it usually stops in the first heap region it hits.
		std::vector<uintptr_t> FindPointersTo(uint32_t value, size_t limit = 64) const
		{
			const std::map<uint32_t, std::vector<uintptr_t>> hits = FindPointersToAny(&value, 1, limit);
			const auto it = hits.find(value);
			return (it == hits.end()) ? std::vector<uintptr_t>() : it->second;
		}

		/// One sweep for several vtables at once, as {value: addresses}.
		///
		/// Reading a multi-hundred-megabyte region in one call would need a
		/// buffer that size, so regions are walked in chunks with a four-byte
		/// overlap so a value straddling a chunk boundary is still seen.
		std::map<uint32_t, std::vector<uintptr_t>> FindPointersToAny(const uint32_t* values, size_t count,
			size_t limit_each = 64) const
		{
			constexpr size_t CHUNK = 8u << 20;

			std::map<uint32_t, std::vector<uintptr_t>> hits;
			std::vector<uint8_t> buf(CHUNK);

			for (const auto& [base, size] : ReadableRegions())
			{
				for (size_t done = 0; done < size;)
				{
					const size_t want = std::min(CHUNK, size - done);
					SIZE_T got = 0;
					if (!ReadProcessMemory(m_handle, reinterpret_cast<LPCVOID>(base + done), buf.data(), want,
							&got) ||
						got < 4)
					{
						done += want;
						continue;
					}

					for (size_t off = 0; off + 4 <= got; off += 4)
					{
						uint32_t word;
						std::memcpy(&word, &buf[off], 4);
						for (size_t i = 0; i < count; i++)
						{
							if (word != values[i])
								continue;
							std::vector<uintptr_t>& list = hits[word];
							if (list.size() < limit_each)
								list.push_back(base + done + off);
						}
					}
					done += got;
				}
			}
			return hits;
		}

	private:
		bool FindModule(DWORD pid, const std::string& exe_name)
		{
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
			if (snap == INVALID_HANDLE_VALUE)
				return false;

			bool found = false;
			MODULEENTRY32 me = {sizeof(me)};
			if (Module32First(snap, &me))
			{
				do
				{
					if (_stricmp(me.szModule, exe_name.c_str()) == 0)
					{
						m_module_base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
						m_module_size = me.modBaseSize;
						found = true;
						break;
					}
				} while (Module32Next(snap, &me));
			}
			CloseHandle(snap);
			return found;
		}

		HANDLE m_handle = nullptr;
		DWORD m_pid = 0;
		uintptr_t m_module_base = 0;
		uint32_t m_module_size = 0;
		std::string m_exe_name;
	};

	/// A singleton located by its vtable, remembered so the expensive sweep only
	/// happens once. Each tick re-checks the cached address and only rescans if
	/// the object moved or died.
	class VtableSingleton
	{
	public:
		VtableSingleton(uint32_t vtable_rva, const char* label)
			: m_rva(vtable_rva), m_label(label)
		{
		}

		void Forget() { m_address = 0; }
		const char* Label() const { return m_label; }

		/// The object's address, or 0 when there is not exactly one.
		uintptr_t Get(const Process& proc, bool allow_rescan = true)
		{
			const uint32_t want = static_cast<uint32_t>(proc.ModuleBase() + m_rva);

			if (m_address != 0 && proc.ReadU32(m_address) == want)
				return m_address;

			m_address = 0;
			if (!allow_rescan)
				return 0;

			const std::vector<uintptr_t> hits = proc.FindPointersTo(want, 8);
			if (hits.size() == 1)
				m_address = hits[0];
			else if (hits.size() > 1)
				m_address = hits[0]; // several: take the first and let the caller judge
			return m_address;
		}

	private:
		uint32_t m_rva;
		const char* m_label;
		uintptr_t m_address = 0;
	};
} // namespace procmem
