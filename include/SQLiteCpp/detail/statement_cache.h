#pragma once

#include <algorithm>
#include <cctype>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
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
			bool in_use = false;
		};

		// Bounded LRU cache of prepared statements.
		//
		// Statements are compiled once with SQLITE_PREPARE_PERSISTENT and then
		// reset + cleared on every reuse, which removes the per-execution
		// compile cost that dominates repetitive workloads. A statement that is
		// still executing (the same SQL re-entered from a row callback, say) is
		// never handed out twice: the caller gets an uncached statement instead,
		// so correctness never depends on cache state.
		class statement_cache {
		public:
			statement_cache(std::shared_ptr<sqlite3> db, std::size_t capacity):
				_db(std::move(db)), _capacity(capacity ? capacity : 1) {}

			std::shared_ptr<statement_slot> acquire(str_ref sql) {
				const std::string key(sql);
				auto found = _index.find(key);
				if(found != _index.end()) {
					if(!found->second->slot->in_use) {
						_lru.splice(_lru.begin(), _lru, found->second);
						found->second->slot->in_use = true;
						reset(found->second->slot);
						return found->second->slot;
					}
					return borrow_uncached(sql);
				}
				if(_index.size() < _capacity) {
					return store(key, sql);
				}
				// Full: reclaim the least recently used idle statement.
				for(auto it = _lru.end(); it != _lru.begin();) {
					--it;
					if(!it->slot->in_use) {
						_index.erase(it->key);
						_lru.erase(it);
						return store(key, sql);
					}
				}
				return borrow_uncached(sql);
			}

			// Called when the owning binder is done with the statement. Cached
			// slots stay resident; uncached ones are finalized when the last
			// owner drops them.
			void release(const std::shared_ptr<statement_slot>& slot) {
				if(!slot) return;
				slot->in_use = false;
				reset(slot);
			}

			std::size_t capacity() const { return _capacity; }
			std::size_t size() const { return _index.size(); }

		private:
			struct entry {
				std::string key;
				std::shared_ptr<statement_slot> slot;
			};

			static void reset(const std::shared_ptr<statement_slot>& slot) {
				if(slot->stmt) {
					sqlite3_reset(slot->stmt.get());
					sqlite3_clear_bindings(slot->stmt.get());
				}
			}

			std::shared_ptr<statement_slot> store(const std::string& key, str_ref sql) {
				auto slot = prepare(sql);
				_lru.push_front(entry{key, slot});
				_index.emplace(_lru.front().key, _lru.begin());
				slot->in_use = true;
				return slot;
			}

			std::shared_ptr<statement_slot> borrow_uncached(str_ref sql) {
				auto slot = prepare(sql);
				slot->in_use = true;
				return slot;
			}

			std::shared_ptr<statement_slot> prepare(str_ref sql) {
				sqlite3_stmt* raw = nullptr;
				const char* tail = nullptr;
				int rc = sqlite3_prepare_v3(_db.get(), sql.data(), static_cast<int>(sql.size()),
				                            SQLITE_PREPARE_PERSISTENT, &raw, &tail);
				if(rc != SQLITE_OK) {
#if SQLITE_VERSION_NUMBER >= 3038000
					const int offset = sqlite3_error_offset(_db.get());
#else
					const int offset = -1;
#endif
					errors::throw_sqlite_error(rc, sql, sqlite3_errmsg(_db.get()), offset);
				}
				if(raw && !std::all_of(tail, sql.data() + sql.size(),
				                       [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); })) {
					sqlite3_finalize(raw);
					throw errors::more_statements("Multiple semicolon separated statements are unsupported", sql);
				}
				auto slot = std::make_shared<statement_slot>();
				slot->stmt.reset(raw);
				return slot;
			}

			std::shared_ptr<sqlite3> _db;
			std::size_t _capacity;
			std::list<entry> _lru;                                            // front = most recently used
			std::unordered_map<std::string, std::list<entry>::iterator> _index;
		};
	}

} // namespace sqlite
