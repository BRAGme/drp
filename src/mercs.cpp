// SPDX-License-Identifier: MIT
//
// Reading Mercenaries' own data files, so the presence uses the game's wording.

#include "mercs.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace
{
	std::string Trim(const std::string& s)
	{
		size_t a = 0, b = s.size();
		while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
			a++;
		while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
			b--;
		return s.substr(a, b - a);
	}

	/// UTF-16LE to UTF-8, enough for the Latin-1 range these files actually use.
	std::string Narrow(const std::wstring& w)
	{
		std::string out;
		out.reserve(w.size());
		for (const wchar_t c : w)
		{
			if (c < 0x80)
				out.push_back(static_cast<char>(c));
			else if (c < 0x800)
			{
				out.push_back(static_cast<char>(0xC0 | (c >> 6)));
				out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
			}
			else
			{
				out.push_back(static_cast<char>(0xE0 | (c >> 12)));
				out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
			}
		}
		return out;
	}
} // namespace

std::map<std::string, std::string> mercs::LoadLocalisation(const std::string& path)
{
	std::map<std::string, std::string> out;

	std::ifstream in(path, std::ios::binary);
	if (!in)
		return out;

	std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (raw.size() < 2)
		return out;

	size_t start = 0;
	if (static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE)
		start = 2;

	std::wstring wide;
	wide.reserve((raw.size() - start) / 2);
	for (size_t i = start; i + 1 < raw.size(); i += 2)
	{
		wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(raw[i]) |
											(static_cast<unsigned char>(raw[i + 1]) << 8)));
	}

	std::wstringstream ws(wide);
	std::wstring line;
	while (std::getline(ws, line))
	{
		const std::string narrow = Narrow(line);
		const size_t tab = narrow.find('\t');
		if (tab == std::string::npos)
			continue;
		const size_t tab2 = narrow.find('\t', tab + 1);
		const std::string key = Trim(narrow.substr(0, tab));
		const std::string english = Trim(narrow.substr(tab + 1, (tab2 == std::string::npos) ? std::string::npos : tab2 - tab - 1));
		if (!key.empty())
			out[key] = english;
	}
	return out;
}

std::map<std::string, mercs::Mission> mercs::LoadMissions(const std::string& data_root)
{
	std::map<std::string, Mission> out;

	const std::map<std::string, std::string> loc = LoadLocalisation(data_root + "\\language\\MissionInfo.txt");

	std::ifstream in(data_root + "\\config\\mission.ini");
	if (!in)
		return out;

	const auto resolve = [&loc](const std::string& key) -> std::string {
		auto it = loc.find(key);
		return (it == loc.end()) ? std::string() : it->second;
	};

	std::string line;
	Mission current;
	bool in_mission = false;

	const auto flush = [&]() {
		if (in_mission && !current.id.empty() && !current.map.empty())
			out[current.id] = current;
		current = Mission();
		in_mission = false;
	};

	while (std::getline(in, line))
	{
		line = Trim(line);
		if (line.empty() || line[0] == ';' || line[0] == '#')
			continue;

		if (line.front() == '[' && line.back() == ']')
		{
			flush();
			const std::string name = line.substr(1, line.size() - 2);
			// The list sections are not missions; a mission section is one that
			// carries a MissionTitleDbKey, which the loop below decides.
			current.id = name;
			in_mission = true;
			continue;
		}
		if (!in_mission)
			continue;

		std::istringstream ls(line);
		std::string key;
		ls >> key;
		std::string value;
		std::getline(ls, value);
		value = Trim(value);

		if (key == "MissionTitleDbKey")
			current.title = resolve(value);
		else if (key == "ShortDescriptionDbKey")
			current.description = resolve(value);
		else if (key == "Chapter")
			current.chapter = resolve(value);
		else if (key == "Faction")
			current.faction = resolve(value);
		else if (key == "Map")
			current.map = value;
		else if (key == "Number")
			current.number = std::atoi(value.c_str());
	}
	flush();

	return out;
}


// ---------------------------------------------------------------------------
// Reading the live game
// ---------------------------------------------------------------------------

const char* mercs::ClassifyState(const procmem::Process& proc, uintptr_t object,
	const VtableEntry* table, size_t count)
{
	if (object == 0)
		return nullptr;

	const uint32_t vt = proc.ReadU32(object);
	if (vt == 0)
		return nullptr;

	for (size_t i = 0; i < count; i++)
	{
		if (vt == static_cast<uint32_t>(proc.ModuleBase() + table[i].rva))
			return table[i].name;
	}
	return nullptr;
}

const char* mercs::CurrentState(const procmem::Process& proc, uintptr_t machine,
	const VtableEntry* table, size_t count, uint32_t window)
{
	if (machine == 0)
		return nullptr;

	std::vector<uint8_t> buf(window);
	if (!proc.Read(machine, buf.data(), buf.size()))
		return nullptr;

	// The state objects live inside the machine, so skip the first word (the
	// machine's own vtable) and take the first pointer that lands on one.
	for (uint32_t off = 4; off + 4 <= window; off += 4)
	{
		uint32_t value;
		std::memcpy(&value, &buf[off], 4);
		if (value < 0x10000)
			continue;
		if (const char* name = ClassifyState(proc, value, table, count))
			return name;
	}
	return nullptr;
}

namespace
{
	/// The engine writes button glyphs into HUD text as `[support]`, `[pda]`,
	/// and a line break as `[n]`. None of that means anything to Discord.
	std::string StripGlyphTokens(const std::string& in)
	{
		std::string out;
		out.reserve(in.size());
		bool in_token = false;
		for (const char c : in)
		{
			if (c == '[')
			{
				in_token = true;
				continue;
			}
			if (c == ']')
			{
				in_token = false;
				if (!out.empty() && out.back() != ' ')
					out.push_back(' ');
				continue;
			}
			if (!in_token)
				out.push_back(c);
		}

		// collapse the runs of spaces the stripping leaves behind
		std::string tidy;
		bool space = false;
		for (const char c : out)
		{
			if (c == ' ' || c == '\t')
			{
				space = true;
				continue;
			}
			if (space && !tidy.empty())
				tidy.push_back(' ');
			space = false;
			tidy.push_back(c);
		}
		return tidy;
	}

	bool LooksLikeSlot(const std::vector<uint8_t>& buf, size_t off)
	{
		if (off + 512 > buf.size())
			return false;
		// Printable up to a NUL inside the slot. An unused slot holds " ".
		for (size_t i = 0; i < 512; i++)
		{
			const uint8_t c = buf[off + i];
			if (c == 0)
				return i > 0;
			if (c < 0x20 || c >= 0x7F)
				return false;
		}
		return false;
	}
} // namespace

std::vector<std::string> mercs::ObjectiveLines(const procmem::Process& proc, uintptr_t tray,
	uint32_t& buffer_offset)
{
	std::vector<std::string> out;
	if (tray == 0)
		return out;

	constexpr uint32_t WINDOW = 0x40 + 3 * 512 + 0x20;
	std::vector<uint8_t> buf(WINDOW);
	if (!proc.Read(tray, buf.data(), buf.size()))
		return out;

	// _aszBuffer is three 512-byte char arrays in a row. Rather than lay the
	// five base classes out by hand, look for the first offset where all three
	// slots read as text; the stride is what identifies it.
	if (buffer_offset == 0 || !LooksLikeSlot(buf, buffer_offset))
	{
		buffer_offset = 0;
		for (uint32_t off = 4; off + 3 * 512 <= WINDOW; off += 4)
		{
			if (LooksLikeSlot(buf, off) && LooksLikeSlot(buf, off + 512) && LooksLikeSlot(buf, off + 1024))
			{
				buffer_offset = off;
				break;
			}
		}
	}
	if (buffer_offset == 0)
		return out;

	for (int slot = 0; slot < 3; slot++)
	{
		const char* text = reinterpret_cast<const char*>(&buf[buffer_offset + slot * 512]);
		std::string line = StripGlyphTokens(std::string(text, strnlen(text, 511)));
		// An empty slot is a single space, not an empty string.
		if (!line.empty() && line.find_first_not_of(' ') != std::string::npos)
			out.push_back(line);
	}
	return out;
}

uintptr_t mercs::FindGameOptions(const procmem::Process& proc)
{
	// _uiDefaultCharacter and _uiCharacter sit next to each other and both hold
	// a merc template CRC, which nothing else in the image does.
	uintptr_t found = 0;
	proc.ForEachChunk(
		[&](uintptr_t base, const uint8_t* data, size_t size) {
			for (size_t off = 0; off + 8 <= size; off += 4)
			{
				uint32_t a, b;
				std::memcpy(&a, data + off, 4);
				std::memcpy(&b, data + off + 4, 4);
				if (IsMercCrc(a) && IsMercCrc(b))
				{
					found = base + off + 4; // the address of _uiCharacter
					return false;
				}
			}
			return true;
		},
		8);
	return found;
}

uintptr_t mercs::FindCurrentMissionData(const procmem::Process& proc,
	const std::map<std::string, Mission>& missions)
{
	if (missions.empty())
		return 0;

	uintptr_t found = 0;
	proc.ForEachChunk(
		[&](uintptr_t base, const uint8_t* data, size_t size) {
		for (size_t off = 0; off + 64 + 8 <= size; off += 4)
		{
			const char* name = reinterpret_cast<const char*>(data + off);
			const size_t len = strnlen(name, 63);
			if (len < 4 || len > 40)
				continue;

			std::string id(name, len);
			// The shipped scripts sometimes prefix the id; take it either way.
			if (id.rfind("BORK: ", 0) == 0)
				id = id.substr(6);
			for (char& c : id)
				c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

			bool known = false;
			for (const auto& [key, m] : missions)
			{
				std::string lower = key;
				for (char& c : lower)
					c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
				if (lower == id)
				{
					known = true;
					break;
				}
			}
			if (!known)
				continue;

			// _acMissionTitle follows _acName[64], and it is already localised.
			if (off + 64 + 4 > size)
				continue;
			const char* title = reinterpret_cast<const char*>(data + off + 64);
			const size_t tlen = strnlen(title, 255);
			if (tlen < 3)
				continue;

			found = base + off;
			return false;
		}
		return true;
		},
		512);
	return found;
}

std::string mercs::CurrentMissionId(const procmem::Process& proc, uintptr_t mission_data)
{
	if (mission_data == 0)
		return {};

	std::string id = proc.ReadString(mission_data, 63);
	if (id.rfind("BORK: ", 0) == 0)
		id = id.substr(6);
	for (char& c : id)
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	return id;
}


uint32_t mercs::CurrentLevelHash(const procmem::Process& proc)
{
	const uintptr_t base = proc.ModuleBase();
	const uintptr_t end = base + proc.ModuleSize();
	if (base == 0)
		return 0;

	int counts[std::size(LEVELS)] = {};

	proc.ForEachChunk([&](uintptr_t chunk_base, const uint8_t* data, size_t size) {
		if (chunk_base < base || chunk_base >= end)
			return true;

		for (size_t off = 0; off + 4 <= size; off += 4)
		{
			uint32_t v;
			std::memcpy(&v, data + off, 4);
			for (size_t i = 0; i < std::size(LEVELS); i++)
			{
				if (LEVELS[i].hash == v)
					counts[i]++;
			}
		}
		return true;
	});

	// Every level appears once in the level table, so a plain maximum is only
	// meaningful when something holds the current one as well.
	size_t best = 0;
	for (size_t i = 1; i < std::size(LEVELS); i++)
	{
		if (counts[i] > counts[best])
			best = i;
	}
	return (counts[best] >= 2) ? LEVELS[best].hash : 0;
}
