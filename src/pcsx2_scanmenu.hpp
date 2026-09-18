// SPDX-License-Identifier: MIT
//
// A research harness, not part of the presence: watching Rainbow Six 3's menu
// globals while somebody walks through the front end. See the README.

#pragma once

#include "pine.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace pcsx2
{
	// -------------------------------------------------------------------------
// scan-menu: find the global that names the menu screen you are on
//
// Rainbow Six 3 loads its whole front end at once (57 pages), so the page
// list cannot say which screen is showing. Somewhere there must be a word
// pointing at the active page, and a page's name sits at page+0x10. This
// mode watches every global that points at a string starting with "P_" and
// prints the ones that change as you move through the menus.
// -------------------------------------------------------------------------

inline int ScanMenu(int slot, uint32_t from, uint32_t to)
{
	pine::Client emu(slot);
	if (!emu.Connect())
	{
		std::printf("Could not reach PCSX2 on PINE slot %d.\n", slot);
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


} // namespace pcsx2
