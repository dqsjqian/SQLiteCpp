#pragma once

// Incremental BLOB I/O (sqlite3_blob_open): read and write a single BLOB in
// chunks instead of materialising it entirely in memory.

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

#include <sqlite3.h>

#include "types.h"
#include "errors.h"
#include "database.h"

namespace sqlite {

	class blob {
	public:
		blob(database &db, str_ref table, str_ref column, sqlite3_int64 rowid,
		     bool read_only = false, str_ref schema = "main")
			: _db(db.connection()) {
			// SQLite wants NUL-terminated names; a string_view does not
			// guarantee one.
			const std::string schema_name(schema);
			const std::string table_name(table);
			const std::string column_name(column);
			if(const int rc = sqlite3_blob_open(_db.get(), schema_name.c_str(), table_name.c_str(),
			                                     column_name.c_str(), rowid, read_only ? 0 : 1, &_handle);
			   rc != SQLITE_OK) {
				_handle = nullptr;
				errors::throw_sqlite_error(rc, {}, sqlite3_errmsg(_db.get()));
			}
		}

		blob(const blob &) = delete;
		blob &operator=(const blob &) = delete;

		blob(blob &&other) noexcept:
			_db(std::move(other._db)), _handle(other._handle) {
			other._handle = nullptr;
		}

		~blob() {
			if(_handle) sqlite3_blob_close(_handle);
		}

		// Size of the BLOB in bytes.
		int size() const { return _handle ? sqlite3_blob_bytes(_handle) : 0; }

		// Reads into `bytes` starting at `offset` in the BLOB.
		void read(std::span<unsigned char> bytes, int offset = 0) {
			ensure_open();
			const int rc = sqlite3_blob_read(_handle, bytes.data(),
			                                 static_cast<int>(bytes.size()), offset);
			if(rc != SQLITE_OK) fail(rc, "read");
		}

		// Writes `bytes` starting at `offset`. The BLOB must have been opened
		// for writing and the database must not hold a read transaction.
		void write(std::span<const unsigned char> bytes, int offset = 0) {
			ensure_open();
			const int rc = sqlite3_blob_write(_handle, bytes.data(),
			                                  static_cast<int>(bytes.size()), offset);
			if(rc != SQLITE_OK) fail(rc, "write");
		}

		// Closes the handle early; the destructor becomes a no-op.
		void close() {
			if(!_handle) return;
			sqlite3_blob_close(_handle);
			_handle = nullptr;
		}

	private:
		void ensure_open() const {
			if(!_handle) throw std::logic_error("sqlite::blob: handle is closed");
		}

		[[noreturn]] void fail(int rc, const char *operation) const {
			errors::throw_sqlite_error(rc, operation, sqlite3_errmsg(_db.get()));
		}

		std::shared_ptr<sqlite3> _db;
		sqlite3_blob *_handle = nullptr;
	};

} // namespace sqlite
