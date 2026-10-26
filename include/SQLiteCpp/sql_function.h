#pragma once

#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <sqlite3.h>
#include "fwd.h"
#include "types.h"
#include "errors.h"
#include "detail/value_traits.h"
#include "detail/function_traits.h"

namespace sqlite {

	namespace sql_function_binder {
		template<
			typename    ContextType,
			std::size_t Count,
			typename    Functions
		>
		inline void step(
				sqlite3_context* db,
				int              count,
				sqlite3_value**  vals
		);

		template<
			std::size_t Count,
			typename    Functions,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) && sizeof...(Values) < Count), void>::type step(
				sqlite3_context* db,
				int              count,
				sqlite3_value**  vals,
				Values&&...      values
		);

		template<
			std::size_t Count,
			typename    Functions,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) == Count), void>::type step(
				sqlite3_context* db,
				int,
				sqlite3_value**,
				Values&&...      values
		);

		template<
			typename    ContextType,
			typename    Functions
		>
		inline void final(sqlite3_context* db);

		template<typename ContextType, std::size_t Count, typename Functions>
		inline void window_step(sqlite3_context *db, int count, sqlite3_value **vals);

		template<typename ContextType, std::size_t Count, typename Functions>
		inline void window_inverse(sqlite3_context *db, int count, sqlite3_value **vals);

		template<typename ContextType, typename Functions>
		inline void window_value(sqlite3_context *db);

		template<typename ContextType, typename Functions>
		inline void window_final(sqlite3_context *db);

		template<
			std::size_t Count,
			typename    Function,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) < Count), void>::type scalar(
				sqlite3_context* db,
				int              count,
				sqlite3_value**  vals,
				Values&&...      values
		);

		template<
			std::size_t Count,
			typename    Function,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) == Count), void>::type scalar(
				sqlite3_context* db,
				int,
				sqlite3_value**,
				Values&&...      values
		);
	}

	namespace sql_function_binder {
		template<class T>
		struct AggregateCtxt {
			T obj;
			bool constructed = true;
		};

		template<
			typename ContextType,
			std::size_t Count,
			typename    Functions
		>
		inline void step(
				sqlite3_context* db,
				int              count,
				sqlite3_value**  vals
		) {
			auto ctxt = static_cast<AggregateCtxt<ContextType>*>(sqlite3_aggregate_context(db, sizeof(AggregateCtxt<ContextType>)));
			if(!ctxt) return;
			try {
				if(!ctxt->constructed) new(ctxt) AggregateCtxt<ContextType>();
				step<Count, Functions>(db, count, vals, ctxt->obj);
				return;
			} catch(const sqlite_exception &e) {
				sqlite3_result_error_code(db, e.get_code());
				sqlite3_result_error(db, e.what(), -1);
			} catch(const std::exception &e) {
				sqlite3_result_error(db, e.what(), -1);
			} catch(...) {
				sqlite3_result_error(db, "Unknown error", -1);
			}
			if(ctxt && ctxt->constructed)
				ctxt->~AggregateCtxt();
		}

		template<
			std::size_t Count,
			typename    Functions,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) && sizeof...(Values) < Count), void>::type step(
				sqlite3_context* db,
				int              count,
				sqlite3_value**  vals,
				Values&&...      values
		) {
			using arg_type = typename std::remove_cv<
					typename std::remove_reference<
							typename utility::function_traits<
									typename Functions::first_type
							>::template argument<sizeof...(Values)>
					>::type
			>::type;

			step<Count, Functions>(
					db,
					count,
					vals,
					std::forward<Values>(values)...,
					get_val_from_db(vals[sizeof...(Values) - 1], result_type<arg_type>()));
		}

		template<
			std::size_t Count,
			typename    Functions,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) == Count), void>::type step(
				sqlite3_context* db,
				int,
				sqlite3_value**,
				Values&&...      values
		) {
			static_cast<Functions*>(sqlite3_user_data(db))->first(std::forward<Values>(values)...);
		}

		template<
			typename    ContextType,
			typename    Functions
		>
		inline void final(sqlite3_context* db) {
			auto ctxt = static_cast<AggregateCtxt<ContextType>*>(sqlite3_aggregate_context(db, sizeof(AggregateCtxt<ContextType>)));
			try {
				if(!ctxt) return;
				if(!ctxt->constructed) new(ctxt) AggregateCtxt<ContextType>();
				store_result_in_db(db,
						static_cast<Functions*>(sqlite3_user_data(db))->second(ctxt->obj));
			} catch(const sqlite_exception &e) {
				sqlite3_result_error_code(db, e.get_code());
				sqlite3_result_error(db, e.what(), -1);
			} catch(const std::exception &e) {
				sqlite3_result_error(db, e.what(), -1);
			} catch(...) {
				sqlite3_result_error(db, "Unknown error", -1);
			}
			if(ctxt && ctxt->constructed)
				ctxt->~AggregateCtxt();
		}

		template<
			std::size_t Count,
			typename    Function,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) < Count), void>::type scalar(
				sqlite3_context* db,
				int              count,
				sqlite3_value**  vals,
				Values&&...      values
		) {
			using arg_type = typename std::remove_cv<
					typename std::remove_reference<
							typename utility::function_traits<Function>::template argument<sizeof...(Values)>
					>::type
			>::type;

			scalar<Count, Function>(
					db,
					count,
					vals,
					std::forward<Values>(values)...,
					get_val_from_db(vals[sizeof...(Values)], result_type<arg_type>()));
		}

		template<
			std::size_t Count,
			typename    Function,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) == Count), void>::type scalar(
				sqlite3_context* db,
				int,
				sqlite3_value**,
				Values&&...      values
		) {
			try {
				store_result_in_db(db,
						(*static_cast<Function*>(sqlite3_user_data(db)))(std::forward<Values>(values)...));
			} catch(const sqlite_exception &e) {
				sqlite3_result_error_code(db, e.get_code());
				sqlite3_result_error(db, e.what(), -1);
			} catch(const std::exception &e) {
				sqlite3_result_error(db, e.what(), -1);
			} catch(...) {
				sqlite3_result_error(db, "Unknown error", -1);
			}
		}

		// ---- window functions -------------------------------------------------
		//
		// A window function has four callbacks: xStep (row enters the frame),
		// xInverse (row leaves the frame), xValue (report the current value) and
		// xFinal (flush). Everything is stored in one tuple as user data and the
		// aggregate state lives in sqlite3_aggregate_context.

		template<typename Callable, std::size_t Index>
		using window_argument = typename std::remove_cv<typename std::remove_reference<
				typename utility::function_traits<Callable>::template argument<Index>>::type>::type;

		template<typename ContextType, std::size_t Count, typename Callable, std::size_t... Index>
		inline void window_dispatch(Callable &fn, ContextType &context, sqlite3_value **vals,
		                            std::index_sequence<Index...>) {
			fn(context, get_val_from_db(vals[Index],
			                            result_type<window_argument<Callable, Index + 1>>())...);
		}

		template<typename ContextType, std::size_t Count, typename Functions, std::size_t Slot>
		inline void window_row(sqlite3_context *db, sqlite3_value **vals) {
			auto *ctxt = static_cast<AggregateCtxt<ContextType>*>(
					sqlite3_aggregate_context(db, sizeof(AggregateCtxt<ContextType>)));
			if(!ctxt) return;
			try {
				if(!ctxt->constructed) new(ctxt) AggregateCtxt<ContextType>();
				auto &fn = std::get<Slot>(*static_cast<Functions*>(sqlite3_user_data(db)));
				window_dispatch<ContextType, Count>(fn, ctxt->obj, vals,
				                                    std::make_index_sequence<Count>{});
			} catch(const sqlite_exception &e) {
				sqlite3_result_error_code(db, e.get_code());
				sqlite3_result_error(db, e.what(), -1);
			} catch(const std::exception &e) {
				sqlite3_result_error(db, e.what(), -1);
			} catch(...) {
				sqlite3_result_error(db, "Unknown error", -1);
			}
			if(ctxt && ctxt->constructed)
				ctxt->~AggregateCtxt();
		}

		template<typename ContextType, std::size_t Count, typename Functions>
		inline void window_step(sqlite3_context *db, int, sqlite3_value **vals) {
			window_row<ContextType, Count, Functions, 0>(db, vals);
		}

		template<typename ContextType, std::size_t Count, typename Functions>
		inline void window_inverse(sqlite3_context *db, int, sqlite3_value **vals) {
			window_row<ContextType, Count, Functions, 1>(db, vals);
		}

		template<typename ContextType, typename Functions>
		inline void window_value(sqlite3_context *db) {
			auto *ctxt = static_cast<AggregateCtxt<ContextType>*>(
					sqlite3_aggregate_context(db, 0));
			try {
				auto &fn = std::get<2>(*static_cast<Functions*>(sqlite3_user_data(db)));
				if(ctxt) {
					if(!ctxt->constructed) new(ctxt) AggregateCtxt<ContextType>();
					store_result_in_db(db, fn(ctxt->obj));
				} else {
					// An empty frame reports the value of a default state.
					ContextType empty{};
					store_result_in_db(db, fn(empty));
				}
			} catch(const sqlite_exception &e) {
				sqlite3_result_error_code(db, e.get_code());
				sqlite3_result_error(db, e.what(), -1);
			} catch(const std::exception &e) {
				sqlite3_result_error(db, e.what(), -1);
			} catch(...) {
				sqlite3_result_error(db, "Unknown error", -1);
			}
		}

		template<typename ContextType, typename Functions>
		inline void window_final(sqlite3_context *db) {
			auto *ctxt = static_cast<AggregateCtxt<ContextType>*>(
					sqlite3_aggregate_context(db, 0));
			try {
				auto &fn = std::get<3>(*static_cast<Functions*>(sqlite3_user_data(db)));
				if(ctxt) {
					if(!ctxt->constructed) new(ctxt) AggregateCtxt<ContextType>();
					store_result_in_db(db, fn(ctxt->obj));
				}
			} catch(const sqlite_exception &e) {
				sqlite3_result_error_code(db, e.get_code());
				sqlite3_result_error(db, e.what(), -1);
			} catch(const std::exception &e) {
				sqlite3_result_error(db, e.what(), -1);
			} catch(...) {
				sqlite3_result_error(db, "Unknown error", -1);
			}
			if(ctxt && ctxt->constructed)
				ctxt->~AggregateCtxt();
		}
	}

} // namespace sqlite
