// SPDX-License-Identifier: MIT
//
// The two lines a presence shows, and the small helpers both watchers use to
// build them. Keeping this separate is what lets one process carry several
// games without either game's code knowing about Discord.

#pragma once

#include <cctype>
#include <cstdint>
#include <string>

namespace presence
{
	/// The separator Discord shows between parts of a line, written out as
	/// explicit UTF-8 so it does not depend on the source file's encoding.
	inline constexpr const char* SEPARATOR = " \xE2\x80\xA2 ";

	struct Lines
	{
		std::string details;    // line one
		std::string state;      // line two
		std::string large_image;// artwork key on the Discord application
		int64_t start_timestamp = 0;

		bool empty() const { return details.empty() && state.empty(); }

		bool operator==(const Lines& o) const
		{
			return details == o.details && state == o.state && large_image == o.large_image &&
				   start_timestamp == o.start_timestamp;
		}
		bool operator!=(const Lines& o) const { return !(*this == o); }
	};

	inline std::string Join(const std::string& a, const std::string& b)
	{
		if (a.empty())
			return b;
		if (b.empty())
			return a;
		return a + SEPARATOR + b;
	}

	inline std::string Lower(std::string s)
	{
		for (char& c : s)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}
} // namespace presence
