#pragma once

// Compile-time SQL literals: "SELECT 1"_sql carries its length without a
// strlen, and keeps pointing at static storage so the view never dangles.

#include <cstddef>
#include <string_view>

#include "types.h"

namespace sqlite {

	class sql_text {
	public:
		constexpr sql_text(const char *text, std::size_t length) noexcept:
			_text(text), _length(length) {}

		constexpr std::string_view view() const noexcept { return {_text, _length}; }
		constexpr operator str_ref() const noexcept { return str_ref(_text, _length); }

	private:
		const char *_text;
		std::size_t _length;
	};

	namespace literals {
		// The length is computed by the compiler, so no strlen runs and the
		// literal keeps pointing at static storage.
		constexpr sql_text operator""_sql(const char *text, std::size_t length) noexcept {
			return sql_text(text, length);
		}
	} // namespace literals

} // namespace sqlite
