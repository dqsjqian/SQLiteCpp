#pragma once

#include <exception>

namespace sqlite {
	namespace utility {
		// Remembers how many exceptions were in flight when it was created, so
		// a destructor can tell whether it runs because of stack unwinding.
		// Copies keep the original count: a moved binder still compares with
		// the moment the statement was first created, and moving costs no call
		// into the C++ runtime.
		class UncaughtExceptionDetector {
		public:
			UncaughtExceptionDetector() noexcept = default;
			UncaughtExceptionDetector(const UncaughtExceptionDetector &) noexcept = default;
			UncaughtExceptionDetector &operator=(const UncaughtExceptionDetector &) noexcept = default;
			~UncaughtExceptionDetector() = default;

			explicit operator bool() const noexcept {
				return count != std::uncaught_exceptions();
			}
		private:
			int count = std::uncaught_exceptions();
		};
	}
}
