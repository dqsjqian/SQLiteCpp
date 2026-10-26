#pragma once

// UTF-16 to UTF-8 conversion without <codecvt>, which is deprecated since
// C++17 and warns under strict warning sets.

#include <cstdint>
#include <string>
#include <string_view>

#include "../types.h"
#include "../errors.h"

namespace sqlite {
	namespace detail {
		inline void append_utf8(std::string &out, char32_t code_point) {
			if(code_point <= 0x7F) {
				out.push_back(static_cast<char>(code_point));
			} else if(code_point <= 0x7FF) {
				out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
				out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
			} else if(code_point <= 0xFFFF) {
				out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
				out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
			} else {
				out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
				out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
			}
		}
	} // namespace detail

	namespace utility {
		// Strict UTF-16 to UTF-8. Lone surrogates and truncated pairs throw
		// errors::invalid_utf16. Embedded NUL code units are preserved.
		inline std::string utf16_to_utf8(u16str_ref input) {
			std::string out;
			out.reserve(input.size() * 3 / 2 + 4);
			for(std::size_t i = 0; i < input.size(); ++i) {
				char32_t code_point = input[i];
				if(code_point >= 0xD800 && code_point <= 0xDBFF) {
					if(i + 1 >= input.size())
						throw errors::invalid_utf16("Truncated surrogate pair", "");
					const char32_t low = input[i + 1];
					if(low < 0xDC00 || low > 0xDFFF)
						throw errors::invalid_utf16("Invalid low surrogate", "");
					code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
					++i;
				} else if(code_point >= 0xDC00 && code_point <= 0xDFFF) {
					throw errors::invalid_utf16("Unexpected low surrogate", "");
				}
				detail::append_utf8(out, code_point);
			}
			return out;
		}
	} // namespace utility
} // namespace sqlite
