#pragma once

#include <string_view>

namespace sqlite {

	// String types accepted by statements and SQL functions. string_view
	// avoids copies on the way in; results are always copied out.
	using str_ref = const std::string_view;
	using u16str_ref = const std::u16string_view;

} // namespace sqlite
