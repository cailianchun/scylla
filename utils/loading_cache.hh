/*
 * Copyright (C) 2016 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <chrono>
#include <unordered_map>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/unordered_set.hpp>

#include <seastar/core/timer.hh>
#include <seastar/core/gate.hh>

#include "utils/exceptions.hh"
#include "utils/loading_shared_values.hh"

namespace bi = boost::intrusive;

namespace utils {

using loading_cache_clock_type = seastar::lowres_clock;
using auto_unlink_list_hook = bi::list_base_hook<bi::link_mode<bi::auto_unlink>>;

template<typename Tp, typename Key, typename EntrySize , typename Hash, typename EqualPred, typename LoadingSharedValuesStats>
class timestamped_val {
public:
    using value_type = Tp;
    using loading_values_type = typename utils::loading_shared_values<Key, timestamped_val, Hash, EqualPred, LoadingSharedValuesStats, 256>;
    class lru_entry;

private:
    value_type _value;
    loading_cache_clock_type::time_point _loaded;
    loading_cache_clock_type::time_point _last_read;
    lru_entry* _lru_entry_ptr = nullptr; /// MRU item is at the front, LRU - at the back
    size_t _size = 0;

public:
    timestamped_val(value_type val)
        : _value(std::move(val))
        , _loaded(loading_cache_clock_type::now())
        , _last_read(_loaded)
        , _size(EntrySize()(_value))
    {}
    timestamped_val() = delete;
    timestamped_val(timestamped_val&&) = default;

    timestamped_val& operator=(value_type new_val) {
        assert(_lru_entry_ptr);

        _value = std::move(new_val);
        _loaded = loading_cache_clock_type::now();
        _lru_entry_ptr->cache_size() -= _size;
        _size = EntrySize()(_value);
        _lru_entry_ptr->cache_size() += _size;
        return *this;
    }

    value_type& value() noexcept {
        touch();
        return _value;
    }

    loading_cache_clock_type::time_point last_read() const noexcept {
        return _last_read;
    }

    loading_cache_clock_type::time_point loaded() const noexcept {
        return _loaded;
    }

    size_t size() const {
        return _size;
    }

    bool ready() const noexcept {
        return _lru_entry_ptr;
    }

private:
    void touch() noexcept {
        assert(_lru_entry_ptr);
        _last_read = loading_cache_clock_type::now();
        _lru_entry_ptr->touch();
    }

    void set_anchor_back_reference(lru_entry* lru_entry_ptr) noexcept {
        _lru_entry_ptr = lru_entry_ptr;
    }
};

template <typename Tp>
struct simple_entry_size {
    size_t operator()(const Tp& val) {
        return 1;
    }
};

/// \brief This is and LRU list entry which is also an anchor for a loading_cache value.
template<typename Tp, typename Key, typename EntrySize , typename Hash, typename EqualPred, typename LoadingSharedValuesStats>
class timestamped_val<Tp, Key, EntrySize, Hash, EqualPred, LoadingSharedValuesStats>::lru_entry : public auto_unlink_list_hook {
private:
    using ts_value_type = timestamped_val<Tp, Key, EntrySize, Hash, EqualPred, LoadingSharedValuesStats>;
    using loading_values_type = typename ts_value_type::loading_values_type;

public:
    using lru_list_type = bi::list<lru_entry, bi::constant_time_size<false>>;
    using timestamped_val_ptr = typename loading_values_type::entry_ptr;

private:
    timestamped_val_ptr _ts_val_ptr;
    lru_list_type& _lru_list;
    size_t& _cache_size;

public:
    lru_entry(timestamped_val_ptr ts_val, lru_list_type& lru_list, size_t& cache_size)
        : _ts_val_ptr(std::move(ts_val))
        , _lru_list(lru_list)
        , _cache_size(cache_size)
    {
        _ts_val_ptr->set_anchor_back_reference(this);
        _cache_size += _ts_val_ptr->size();
    }

    lru_entry() = delete;

    ~lru_entry() {
        _cache_size -= _ts_val_ptr->size();
        _ts_val_ptr->set_anchor_back_reference(nullptr);
    }

    size_t& cache_size() noexcept {
        return _cache_size;
    }

    /// Set this item as the most recently used item.
    /// The MRU item is going to be at the front of the _lru_list, the LRU item - at the back.
    void touch() noexcept {
        auto_unlink_list_hook::unlink();
        _lru_list.push_front(*this);
    }

    const Key& key() const noexcept {
        return loading_values_type::to_key(_ts_val_ptr);
    }

    timestamped_val& timestamped_value() noexcept { return *_ts_val_ptr; }
    const timestamped_val& timestamped_value() const noexcept { return *_ts_val_ptr; }
    timestamped_val_ptr timestamped_value_ptr() noexcept { return _ts_val_ptr; }
};

enum class loading_cache_reload_enabled { no, yes };

/// \brief Loading cache is a cache that loads the value into the cache using the given asynchronous callback.
///
/// Each cached value is reloaded after the "refresh" time period since it was loaded for the last time.
///
/// The values are going to be evicted from the cache if they are not accessed during the "expiration" period or haven't
/// been reloaded even once during the same period.
///
/// If "expiration" is set to zero - the caching is going to be disabled and get_XXX(...) is going to call the "loader" callback
/// every time in order to get the requested value.
///
/// \note In order to avoid the eviction of cached entries due to "aging" of the contained value the user has to choose
/// the "expiration" to be at least ("refresh" + "load latency"). This way the value is going to stay in the cache and is going to be
/// read in a non-blocking way as long as it's frequently accessed.
///
/// The cache is also limited in size and if adding the next value is going
/// to exceed the cache size limit the least recently used value(s) is(are) going to be evicted until the size of the cache
/// becomes such that adding the new value is not going to break the size limit. If the new entry's size is greater than
/// the cache size then the get_XXX(...) method is going to return a future with the loading_cache::entry_is_too_big exception.
///
/// The size of the cache is defined as a sum of sizes of all cached entries.
/// The size of each entry is defined by the value returned by the \tparam EntrySize predicate applied on it.
///
/// The get(key) or get_ptr(key) methods ensures that the "loader" callback is called only once for each cached entry regardless of how many
/// callers are calling for the get_XXX(key) for the same "key" at the same time. Only after the value is evicted from the cache
/// it's going to be "loaded" in the context of get_XXX(key). As long as the value is cached get_XXX(key) is going to return the
/// cached value immediately and reload it in the background every "refresh" time period as described above.
///
/// \tparam Key type of the cache key
/// \tparam Tp type of the cached value
/// \tparam EntrySize predicate to calculate the entry size
/// \tparam Hash hash function
/// \tparam EqualPred equality predicate
/// \tparam LoadingSharedValuesStats statistics incrementing class (see utils::loading_shared_values)
/// \tparam Alloc elements allocator
template<typename Key,
         typename Tp,
         typename EntrySize = simple_entry_size<Tp>,
         typename Hash = std::hash<Key>,
         typename EqualPred = std::equal_to<Key>,
         typename LoadingSharedValuesStats = utils::do_nothing_loading_shared_values_stats,
         typename Alloc = std::allocator<typename timestamped_val<Tp, Key, EntrySize, Hash, EqualPred, LoadingSharedValuesStats>::lru_entry>>
class loading_cache {
private:
    using ts_value_type = timestamped_val<Tp, Key, EntrySize, Hash, EqualPred, LoadingSharedValuesStats>;
    using loading_values_type = typename ts_value_type::loading_values_type;
    using timestamped_val_ptr = typename loading_values_type::entry_ptr;
    using ts_value_lru_entry = typename ts_value_type::lru_entry;
    using set_iterator = typename loading_values_type::iterator;
    using lru_list_type = typename ts_value_lru_entry::lru_list_type;

public:
    using value_type = Tp;
    using key_type = Key;

    class entry_is_too_big : public std::exception {};

    template<typename Func>
    loading_cache(size_t max_size, std::chrono::milliseconds expiry, std::chrono::milliseconds refresh, logging::logger& logger, Func&& load)
                : _max_size(max_size)
                , _expiry(expiry)
                , _refresh(refresh)
                , _logger(logger)
                , _load(std::forward<Func>(load)) {

        // If expiration period is zero - caching is disabled
        if (!caching_enabled()) {
            return;
        }

        // Sanity check: if expiration period is given then non-zero refresh period and maximal size are required
        if (_refresh == std::chrono::milliseconds(0) || _max_size == 0) {
            throw exceptions::configuration_exception("loading_cache: caching is enabled but refresh period and/or max_size are zero");
        }

        _timer_period = std::min(_expiry, _refresh);
        _timer.set_callback([this] { on_timer(); });
        _timer.arm(_timer_period);
    }

    loading_cache() = delete;

    ~loading_cache() {
        _lru_list.erase_and_dispose(_lru_list.begin(), _lru_list.end(), [] (ts_value_lru_entry* ptr) { loading_cache::destroy_ts_value(ptr); });
    }

    future<Tp> get(const Key& k) {
        // If caching is disabled - always load in the foreground
        if (!caching_enabled()) {
            return _load(k);
        }

        return _loading_values.get_or_load(k, [this] (const Key& k) {
            return _load(k).then([this] (value_type val) {
                return ts_value_type(std::move(val));
            });
        }).then([this, k] (timestamped_val_ptr ts_val_ptr) {
            // check again since it could have already been inserted and initialized
            if (!ts_val_ptr->ready()) {
                _logger.trace("{}: storing the value for the first time", k);

                if (ts_val_ptr->size() > _max_size) {
                    return make_exception_future<Tp>(entry_is_too_big());
                }

                ts_value_lru_entry* new_lru_entry = Alloc().allocate(1);
                new(new_lru_entry) ts_value_lru_entry(std::move(ts_val_ptr), _lru_list, _current_size);

                // This will "touch" the entry and add it to the LRU list.
                return make_ready_future<Tp>(new_lru_entry->timestamped_value_ptr()->value());
            }

            return make_ready_future<Tp>(ts_val_ptr->value());
        });
    }

    future<> stop() {
        return _timer_reads_gate.close().finally([this] { _timer.cancel(); });
    }

private:
    set_iterator set_find(const Key& k) noexcept {
        set_iterator it = _loading_values.find(k);
        set_iterator end_it = set_end();

        if (it == end_it || !it->ready()) {
            return end_it;
        }
        return it;
    }

    set_iterator set_end() noexcept {
        return _loading_values.end();
    }

    set_iterator set_begin() noexcept {
        return _loading_values.begin();
    }

    bool caching_enabled() const {
        return _expiry != std::chrono::milliseconds(0);
    }

    static void destroy_ts_value(ts_value_lru_entry* val) {
        val->~ts_value_lru_entry();
        Alloc().deallocate(val, 1);
    }

    future<> reload(ts_value_lru_entry& lru_entry) {
        return _load(lru_entry.key()).then_wrapped([this, key = lru_entry.key()] (auto&& f) mutable {
            // if the entry has been evicted by now - simply end here
            set_iterator it = set_find(key);
            if (it == set_end()) {
                _logger.trace("{}: entry was dropped during the reload", key);
                return make_ready_future<>();
            }

            // The exceptions are related to the load operation itself.
            // We should ignore them for the background reads - if
            // they persist the value will age and will be reloaded in
            // the forground. If the foreground READ fails the error
            // will be propagated up to the user and will fail the
            // corresponding query.
            try {
                *it = f.get0();
            } catch (std::exception& e) {
                _logger.debug("{}: reload failed: {}", key, e.what());
            } catch (...) {
                _logger.debug("{}: reload failed: unknown error", key);
            }

            return make_ready_future<>();
        });
    }

    void drop_expired() {
        auto now = loading_cache_clock_type::now();
        _lru_list.remove_and_dispose_if([now, this] (const ts_value_lru_entry& lru_entry) {
            using namespace std::chrono;
            // An entry should be discarded if it hasn't been reloaded for too long or nobody cares about it anymore
            const ts_value_type& v = lru_entry.timestamped_value();
            auto since_last_read = now - v.last_read();
            auto since_loaded = now - v.loaded();
            if (_expiry < since_last_read || _expiry < since_loaded) {
                _logger.trace("drop_expired(): {}: dropping the entry: _expiry {},  ms passed since: loaded {} last_read {}", lru_entry.key(), _expiry.count(), duration_cast<milliseconds>(since_loaded).count(), duration_cast<milliseconds>(since_last_read).count());
                return true;
            }
            return false;
        }, [this] (ts_value_lru_entry* p) {
            loading_cache::destroy_ts_value(p);
        });
    }

    // Shrink the cache to the _max_size discarding the least recently used items
    void shrink() {
        while (_current_size > _max_size) {
            using namespace std::chrono;
            ts_value_lru_entry& lru_entry = *_lru_list.rbegin();
            _logger.trace("shrink(): {}: dropping the entry: ms since last_read {}", lru_entry.key(), duration_cast<milliseconds>(loading_cache_clock_type::now() - lru_entry.timestamped_value().last_read()).count());
            loading_cache::destroy_ts_value(&lru_entry);
        }
    }

    // Try to bring the load factors of the _loading_values into a known range.
    void periodic_rehash() noexcept {
        try {
            _loading_values.rehash();
        } catch (...) {
            // if rehashing fails - continue with the current buckets array
        }
    }

    void on_timer() {
        _logger.trace("on_timer(): start");

        // Clean up items that were not touched for the whole _expiry period.
        drop_expired();

        // Remove the least recently used items if map is too big.
        shrink();

        // check if rehashing is needed and do it if it is.
        periodic_rehash();

        // Reload all those which vlaue needs to be reloaded.
        with_gate(_timer_reads_gate, [this] {
            return parallel_for_each(_lru_list.begin(), _lru_list.end(), [this] (ts_value_lru_entry& lru_entry) {
                _logger.trace("on_timer(): {}: checking the value age", lru_entry.key());
                if (lru_entry.timestamped_value().loaded() + _refresh < loading_cache_clock_type::now()) {
                    _logger.trace("on_timer(): {}: reloading the value", lru_entry.key());
                    return this->reload(lru_entry);
                }
                return now();
            }).finally([this] {
                _logger.trace("on_timer(): rearming");
                _timer.arm(loading_cache_clock_type::now() + _timer_period);
            });
        });
    }

    loading_values_type _loading_values;
    lru_list_type _lru_list;
    size_t _current_size = 0;
    size_t _max_size = 0;
    std::chrono::milliseconds _expiry;
    std::chrono::milliseconds _refresh;
    loading_cache_clock_type::duration _timer_period;
    logging::logger& _logger;
    std::function<future<Tp>(const Key&)> _load;
    timer<loading_cache_clock_type> _timer;
    seastar::gate _timer_reads_gate;
};

}

