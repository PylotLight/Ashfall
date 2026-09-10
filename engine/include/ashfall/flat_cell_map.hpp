#pragma once

// Minimal open-addressing hash map for the simulation hot path.
//
// std::unordered_map costs a node allocation per insert plus pointer chasing
// per lookup; step() performs ~100k of each per tick at 512x512. This table
// stores keys/values inline in power-of-two slot arrays, so inserts and finds
// stay in cache and clearing is a memset. Only what step() needs is
// implemented: reserve/clear/insert-or-assign/find/contains/iteration.
// Erase uses tombstones: erase serves edit paths (set_cell/fill_rect writing
// air, set_temperature(0), thermal prune of temps on air); step()'s
// steady-state movement path uses clear+swap of scratch tables instead of
// erase.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ashfall {

template <typename Key, typename Value, typename Hash>
class FlatMap {
public:
    // Workload probe accounting (F4): write-only instrumentation. The sim
    // never reads these counters; they exist so benchmarks can measure
    // probe-length distributions on representative worlds.
    struct ProbeCounters {
        std::uint64_t hits = 0, misses = 0, inserts = 0, updates = 0;
        std::uint64_t hit_probes = 0, miss_probes = 0, insert_probes = 0, update_probes = 0;
        std::uint64_t max_probes = 0;
        std::uint64_t histogram[8] = {}; // buckets: 1,2,3-4,5-8,9-16,17-32,33-64,65+
    };

    FlatMap() = default;

    void reserve_for(std::size_t expected_size) {
        std::size_t cap = 64;
        while (cap < expected_size * 2 + 16) cap *= 2;
        // Never shrinks: per-tick reserve_for calls with varying sizes must
        // not thrash capacity up and down. Shrinking is explicit via
        // shrink_to_fit() (load/reset paths only).
        if (cap <= capacity_) return;
        rehash(cap);
    }

    // Rehash to the smallest power-of-two capacity holding size_ (min 64).
    // Load/reset paths only: it re-inserts every entry, so never call it
    // per-tick. Never grows: if the table already fits, this is a no-op.
    void shrink_to_fit() {
        std::size_t cap = 64;
        while (cap < size_ * 2 + 16) cap *= 2;
        if (cap >= capacity_) return;
        rehash(cap);
    }

    // Read-only capacity observer for tests/benchmarks sizing the workload
    // (it never feeds sim decisions).
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // Counting rules: every examined slot counts, including the terminating
    // empty slot on a miss. find/contains walks record hit or miss.
    // insert_or_assign walks record insert or update at completion.
    // operator[] records its find walk plus (on miss) its insert walk.
    // erase's walk records hit or miss. rehash internal re-inserts do NOT
    // record: they measure table history (growth/shrink choices), not the
    // workload's probe distribution.
    [[nodiscard]] ProbeCounters probe_counters() const noexcept { return probes_; }
    void reset_probe_counters() noexcept { probes_ = ProbeCounters{}; }

    void clear() {
        std::fill(occupied_.begin(), occupied_.end(), 0);
        size_ = 0;
        tombstones_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] bool contains(const Key& key) const {
        return find_slot(key) >= 0;
    }

    [[nodiscard]] Value* find(const Key& key) {
        const std::ptrdiff_t slot = find_slot(key);
        return slot < 0 ? nullptr : &values_[static_cast<std::size_t>(slot)];
    }

    [[nodiscard]] const Value* find(const Key& key) const {
        const std::ptrdiff_t slot = find_slot(key);
        return slot < 0 ? nullptr : &values_[static_cast<std::size_t>(slot)];
    }

    void insert_or_assign(const Key& key, Value value) {
        if (capacity_ == 0) rehash(64);
        if ((size_ + tombstones_) * 2 >= capacity_) rehash(capacity_ * 2);
        std::size_t mask = capacity_ - 1;
        std::size_t slot = static_cast<std::size_t>(hasher_(key)) & mask;
        std::ptrdiff_t first_tomb = -1;
        std::uint64_t examined = 0;
        for (;;) {
            ++examined;
#ifndef NDEBUG
            // Debug-only probe bound: an all-tombstones table with no empty
            // slot would otherwise spin forever; fail loudly instead. (F1)
            assert(examined <= capacity_);
#endif
            const std::uint8_t occ = occupied_[slot];
            if (occ == 0) {
                const std::size_t target =
                    first_tomb >= 0 ? static_cast<std::size_t>(first_tomb) : slot;
                keys_[target] = key;
                values_[target] = std::move(value);
                occupied_[target] = 1;
                if (first_tomb >= 0) --tombstones_;
                ++size_;
                record_write_walk(examined, true);
                return;
            }
            if (occ == 1 && keys_[slot] == key) {
                values_[slot] = std::move(value);
                record_write_walk(examined, false);
                return;
            }
            if (occ == 2 && first_tomb < 0) first_tomb = static_cast<std::ptrdiff_t>(slot);
            slot = (slot + 1) & mask;
        }
    }

    // C++17-compatible operator[]: default-constructs on miss.
    Value& operator[](const Key& key) {
        Value* found = find(key);
        if (found != nullptr) return *found;
        insert_or_assign(key, Value{});
        // Unrecorded trailing lookup: the find walk (miss) and the insert
        // walk above already account for this operator[]; recording the
        // locator would triple-count the miss path.
        std::uint64_t ignored = 0;
        const std::ptrdiff_t slot = find_slot_impl(key, ignored);
        assert(slot >= 0);
        return values_[static_cast<std::size_t>(slot)];
    }

    void erase(const Key& key) {
        const std::ptrdiff_t slot = find_slot(key);
        if (slot < 0) return;
        occupied_[static_cast<std::size_t>(slot)] = 2;
        --size_;
        ++tombstones_;
    }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (std::size_t slot = 0; slot < capacity_; ++slot) {
            if (occupied_[slot] == 1) fn(keys_[slot], values_[slot]);
        }
    }

private:
    [[nodiscard]] std::ptrdiff_t find_slot(const Key& key) const {
        std::uint64_t examined = 0;
        const std::ptrdiff_t slot = find_slot_impl(key, examined);
        record_read_walk(examined, slot >= 0);
        return slot;
    }

    [[nodiscard]] std::ptrdiff_t find_slot_impl(const Key& key, std::uint64_t& examined) const {
        if (capacity_ == 0) return -1;
        const std::size_t mask = capacity_ - 1;
        std::size_t slot = static_cast<std::size_t>(hasher_(key)) & mask;
        for (;;) {
            ++examined;
#ifndef NDEBUG
            // Debug-only probe bound, symmetric with insert_or_assign. (F1)
            assert(examined <= capacity_);
#endif
            const std::uint8_t occ = occupied_[slot];
            if (occ == 0) return -1;
            if (occ == 1 && keys_[slot] == key) return static_cast<std::ptrdiff_t>(slot);
            slot = (slot + 1) & mask;
        }
    }

    static std::size_t probe_bucket(std::uint64_t examined) noexcept {
        if (examined <= 1) return 0;
        if (examined == 2) return 1;
        if (examined <= 4) return 2;
        if (examined <= 8) return 3;
        if (examined <= 16) return 4;
        if (examined <= 32) return 5;
        if (examined <= 64) return 6;
        return 7;
    }

    void record_probe_length(std::uint64_t examined) const noexcept {
        if (examined > probes_.max_probes) probes_.max_probes = examined;
        ++probes_.histogram[probe_bucket(examined)];
    }

    void record_read_walk(std::uint64_t examined, bool hit) const noexcept {
        if (hit) {
            ++probes_.hits;
            probes_.hit_probes += examined;
        } else {
            ++probes_.misses;
            probes_.miss_probes += examined;
        }
        record_probe_length(examined);
    }

    void record_write_walk(std::uint64_t examined, bool inserted) const noexcept {
        if (inserted) {
            ++probes_.inserts;
            probes_.insert_probes += examined;
        } else {
            ++probes_.updates;
            probes_.update_probes += examined;
        }
        record_probe_length(examined);
    }

    void rehash(std::size_t new_capacity) {
        assert(new_capacity >= std::size_t{64} && (new_capacity & (new_capacity - 1)) == 0);
        Hash saved = hasher_;
        // Counter save/restore: the re-inserts below run through
        // insert_or_assign (which records), but they are table-history churn,
        // not workload probes, so they must not pollute the counters.
        const ProbeCounters saved_probes = probes_;
        FlatMap fresh;
        fresh.capacity_ = new_capacity;
        // Place entries with the live hasher, not a default-constructed one,
        // so stateful hashes stay consistent between placement and lookup.
        // (With the stateless production hash this is a no-op.)
        fresh.hasher_ = saved;
        fresh.keys_.assign(new_capacity, Key{});
        fresh.values_.assign(new_capacity, Value{});
        fresh.occupied_.assign(new_capacity, 0);
        for_each([&](const Key& key, const Value& value) { fresh.insert_or_assign(key, value); });
        *this = std::move(fresh);
        hasher_ = saved;
        probes_ = saved_probes;
    }

    Hash hasher_;
    std::vector<Key> keys_;
    std::vector<Value> values_;
    std::vector<std::uint8_t> occupied_; // 0 = empty, 1 = filled, 2 = tombstone
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
    std::size_t tombstones_ = 0;
    // Write-only workload instrumentation; sim code must never read it.
    mutable ProbeCounters probes_ = {};
};

} // namespace ashfall
