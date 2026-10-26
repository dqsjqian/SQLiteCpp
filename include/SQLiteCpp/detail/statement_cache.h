#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <functional>
#include <map>
#include <utility>
#include <sqlite3.h>
#include "../types.h"
#include "../errors.h"

namespace sqlite {

	namespace detail {
		// A prepared statement plus its reuse state, shared between the cache
		// and every binder that currently borrows it.
		struct statement_slot {
			std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt{nullptr, sqlite3_finalize};
			// Claimed under the cache lock, handed back without it: the last
			// thing a borrower does is store false with release ordering.
			std::atomic<bool> in_use{false};
		};

		// Holds the connection's own mutex. In serialized mode (SQLite's
		// default) that makes the cache exactly as thread-safe as the
		// connection; a NOMUTEX connection has no mutex and pays nothing.
		class connection_lock {
		public:
			explicit connection_lock(sqlite3 *db) noexcept: _mutex(sqlite3_db_mutex(db)) {
				sqlite3_mutex_enter(_mutex); // a no-op for a null mutex
			}
			~connection_lock() { sqlite3_mutex_leave(_mutex); }
			connection_lock(const connection_lock &) = delete;
			connection_lock &operator=(const connection_lock &) = delete;
		private:
			sqlite3_mutex *_mutex;
		};

		// Bounded LRU cache of prepared statements.
		//
		// Statements are compiled once with SQLITE_PREPARE_PERSISTENT and reset
		// on release, which removes the per-execution compile cost that
		// dominates repetitive workloads. Bindings are not cleared here: the
		// next borrower overwrites the parameters it binds and NULLs the rest
		// before stepping, which costs nothing when every parameter is bound.
		//
		// A statement that is still executing (the same SQL re-entered from a
		// row callback, or running on another thread) is never handed out
		// twice: the caller gets an uncached statement instead, so correctness
		// never depends on cache state. A capacity of 0 disables caching.
		class statement_cache {
		public:
			statement_cache(std::shared_ptr<sqlite3> db, std::size_t capacity):
				_db(std::move(db)), _capacity(capacity) {}

			// Returns the slot plus whether the cache retains ownership. A slot
			// the cache does not own belongs to the caller and must be released
			// by it (the binder stores it in a unique_ptr).
			std::pair<statement_slot *, bool> acquire(str_ref sql) {
				if(_capacity == 0) return borrow_uncached(sql);
				const connection_lock lock(_db.get());
				// std::less<> makes the lookup transparent: no temporary
				// std::string is built on the hot path.
				auto found = _index.find(sql);
				if(found != _index.end()) {
					statement_slot *slot = found->second->slot.get();
					if(!slot->in_use.load(std::memory_order_acquire)) {
						_lru.splice(_lru.begin(), _lru, found->second);
						slot->in_use.store(true, std::memory_order_relaxed);
						return {slot, true};
					}
					return borrow_uncached(sql);
				}
				if(_index.size() < _capacity) {
					return store(std::string(sql), sql);
				}
				// Full: reclaim the least recently used idle statement.
				for(auto it = _lru.end(); it != _lru.begin();) {
					--it;
					if(!it->slot->in_use.load(std::memory_order_acquire)) {
						_index.erase(it->key);
						_lru.erase(it);
						return store(std::string(sql), sql);
					}
				}
				return borrow_uncached(sql);
			}

			// Called when the owning binder is done with the statement. Cached
			// slots stay resident; uncached ones are finalized when the last
			// owner drops them.
			// `needs_reset` is false when the owner already reset the statement
			// (execute() does) or never stepped it.
			void release(statement_slot *slot, bool needs_reset = true) {
				if(!slot) return;
				// A statement left in the DONE state keeps SQLite from opening
				// savepoints or committing ("SQL statements in progress"), so
				// this must happen exactly once per execution.
				if(needs_reset) reset(slot);
				// Last access: after this store another thread may reuse or
				// evict the slot.
				slot->in_use.store(false, std::memory_order_release);
			}

			sqlite3 *connection() const { return _db.get(); }

			std::size_t capacity() const { return _capacity; }
			std::size_t size() const {
				const connection_lock lock(_db.get());
				return _index.size();
			}

		private:
			struct entry {
				std::string key;
				std::unique_ptr<statement_slot> slot;
			};

			// Reset only: the streaming binder binds every parameter on the
			// next use, and SQLite treats an unbound parameter as NULL.
			static void reset(statement_slot *slot) {
				if(slot && slot->stmt) sqlite3_reset(slot->stmt.get());
			}

			std::pair<statement_slot *, bool> store(const std::string& key, str_ref sql) {
				auto slot = prepare(sql);
				slot->in_use.store(true, std::memory_order_relaxed);
				auto *raw = slot.get();
				_lru.push_front(entry{key, std::move(slot)});
				_index.emplace(_lru.front().key, _lru.begin());
				return {raw, true};
			}

			std::pair<statement_slot *, bool> borrow_uncached(str_ref sql) {
				auto slot = prepare(sql);
				slot->in_use.store(true, std::memory_order_relaxed);
				return {slot.release(), false};
			}

			std::unique_ptr<statement_slot> prepare(str_ref sql) {
				sqlite3_stmt* raw = nullptr;
				const char* tail = nullptr;
				int rc = sqlite3_prepare_v3(_db.get(), sql.data(), static_cast<int>(sql.size()),
				                            SQLITE_PREPARE_PERSISTENT, &raw, &tail);
				if(rc != SQLITE_OK)
					errors::throw_sqlite_error(rc, sql, sqlite3_errmsg(_db.get()),
					                           sqlite3_error_offset(_db.get()));
				if(raw && !std::all_of(tail, sql.data() + sql.size(),
				                       [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); })) {
					sqlite3_finalize(raw);
					throw errors::more_statements("Multiple semicolon separated statements are unsupported", sql);
				}
				auto slot = std::make_unique<statement_slot>();
				slot->stmt.reset(raw);
				return slot;
			}

			std::shared_ptr<sqlite3> _db;
			std::size_t _capacity;
			std::list<entry> _lru;                                            // front = most recently used
			// std::less<> enables heterogenous lookup with string_view.
			std::map<std::string, std::list<entry>::iterator, std::less<>> _index;
		};
	}

} // namespace sqlite
