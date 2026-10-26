#pragma once

#include <string>
#include <stdexcept>

#include "types.h"
#include <sqlite3.h>

namespace sqlite {

	class sqlite_exception: public std::runtime_error {
	public:
		sqlite_exception(const char* msg, str_ref sql, int code = -1, int offset = -1):
			runtime_error(build_message(msg, code, offset)), code(code), offset(offset), sql(sql) {}
		sqlite_exception(int code, str_ref sql, const char *msg = nullptr, int offset = -1):
			runtime_error(build_message(msg ? msg : sqlite3_errstr(code), code, offset)), code(code), offset(offset), sql(sql) {}
		int get_code() const {return code & 0xFF;}
		int get_extended_code() const {return code;}
		// Byte offset into the SQL text where the problem was detected, or -1
		// when unknown (requires SQLite 3.38 or newer).
		int get_error_offset() const {return offset;}
		std::string get_sql() const {return sql;}
		const char *errstr() const {return code == -1 ? "Unknown error" : sqlite3_errstr(code);}
	private:
		static std::string build_message(const char* msg, int code, int offset) {
			std::string message = msg ? msg : (code == -1 ? "Unknown error" : sqlite3_errstr(code));
			if(offset >= 0) {
				message += " [byte offset ";
				message += std::to_string(offset);
				message += "]";
			}
			return message;
		}
		int code;
		int offset;
		std::string sql;
	};

	namespace errors {
		//One more or less trivial derived error class for each SQLITE error.
		//Note the following are not errors so have no classes:
		//SQLITE_OK, SQLITE_NOTICE, SQLITE_WARNING, SQLITE_ROW, SQLITE_DONE
		//
		//Note these names are exact matches to the names of the SQLITE error codes.
#define SQLITE_CPP_ERROR_CODE(NAME,name,derived) \
		class name: public sqlite_exception { using sqlite_exception::sqlite_exception; };\
		derived
#define SQLITE_CPP_ERROR_CODE_EXTENDED(BASE,SUB,base,sub) \
		class base ## _ ## sub: public base { using base::base; };
#include "lists/error_codes.h"
#undef SQLITE_CPP_ERROR_CODE_EXTENDED
#undef SQLITE_CPP_ERROR_CODE

		//Some additional errors are here for the C++ interface
		class more_rows: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class no_rows: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class more_statements: public sqlite_exception { using sqlite_exception::sqlite_exception; }; // Prepared statements can only contain one statement
		class invalid_utf16: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class unknown_binding: public sqlite_exception { using sqlite_exception::sqlite_exception; };

		[[noreturn]] inline void throw_sqlite_error(const int& error_code, str_ref sql = "", const char *errmsg = nullptr, int offset = -1) {
			// Materialise the offset suffix once; every exception below copies
			// the message immediately, so the buffer outlives only this frame.
			std::string annotated;
			if(offset >= 0) {
				annotated = errmsg ? errmsg : sqlite3_errstr(error_code);
				annotated += " [byte offset ";
				annotated += std::to_string(offset);
				annotated += "]";
				errmsg = annotated.c_str();
			}
			switch(error_code & 0xFF) {
#define SQLITE_CPP_ERROR_CODE(NAME,name,derived)     \
				case SQLITE_ ## NAME: switch(error_code) {          \
					derived                                           \
					case SQLITE_ ## NAME:                             \
					default: throw name(error_code, sql, errmsg, offset);             \
				}

#define SQLITE_CPP_ERROR_CODE_EXTENDED(BASE,SUB,base,sub) \
					case SQLITE_ ## BASE ## _ ## SUB: throw base ## _ ## sub(error_code, sql, errmsg, offset);
#include "lists/error_codes.h"
#undef SQLITE_CPP_ERROR_CODE_EXTENDED
#undef SQLITE_CPP_ERROR_CODE
				default: throw sqlite_exception(error_code, sql, errmsg, offset);
			}
		}
	}
	namespace exceptions = errors;
}
