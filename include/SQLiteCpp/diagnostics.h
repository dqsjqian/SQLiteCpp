#pragma once

// File-level diagnostics that do not need a connection: decode the 100-byte
// SQLite database header (https://www.sqlite.org/fileformat.html).

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <string>

#include <sqlite3.h>

#include "types.h"
#include "errors.h"

namespace sqlite {

	struct header_info {
		std::array<unsigned char, 16> signature{};
		std::uint32_t page_size = 0;
		std::uint8_t write_version = 0;
		std::uint8_t read_version = 0;
		std::uint32_t change_counter = 0;
		std::uint32_t database_pages = 0;
		std::uint32_t text_encoding = 0;
		std::uint32_t user_version = 0;
		std::uint32_t application_id = 0;

		// True when the file starts with "SQLite format 3".
		bool valid() const {
			static constexpr unsigned char expected[16] = {
				'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f', 'o', 'r', 'm', 'a', 't', ' ', '3', '\0'};
			return std::equal(signature.begin(), signature.end(), expected);
		}
	};

	namespace detail {
		inline std::uint32_t read_be32(const unsigned char *bytes) {
			return (static_cast<std::uint32_t>(bytes[0]) << 24)
			     | (static_cast<std::uint32_t>(bytes[1]) << 16)
			     | (static_cast<std::uint32_t>(bytes[2]) << 8)
			     | static_cast<std::uint32_t>(bytes[3]);
		}
	} // namespace detail

	// Reads and decodes the header of a database file without opening it
	// through SQLite.
	inline header_info read_header(str_ref path) {
		std::ifstream file(std::string(path), std::ios::binary);
		if(!file)
			errors::throw_sqlite_error(SQLITE_CANTOPEN, "read_header", "cannot open the file");
		std::array<unsigned char, 100> raw{};
		file.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
		if(file.gcount() < static_cast<std::streamsize>(raw.size()))
			errors::throw_sqlite_error(SQLITE_NOTADB, "read_header",
			                           "file is shorter than the 100-byte header");

		header_info info;
		std::copy(raw.begin(), raw.begin() + 16, info.signature.begin());
		info.page_size = detail::read_be32(raw.data() + 16);
		if(info.page_size == 1) info.page_size = 65536; // encoded as 1 in the header
		info.write_version = raw[18];
		info.read_version = raw[19];
		info.change_counter = detail::read_be32(raw.data() + 24);
		info.database_pages = detail::read_be32(raw.data() + 28);
		info.text_encoding = detail::read_be32(raw.data() + 56);
		info.user_version = detail::read_be32(raw.data() + 60);
		info.application_id = detail::read_be32(raw.data() + 68);
		return info;
	}

	// Cheap "is this a SQLite file" check; never throws.
	inline bool has_sqlite_header(str_ref path) {
		try {
			return read_header(path).valid();
		} catch(...) {
			return false;
		}
	}

} // namespace sqlite
