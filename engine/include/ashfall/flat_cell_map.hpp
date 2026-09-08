#pragma once

// Minimal open-addressing hash map for the simulation hot path.
//
// std::unordered_map costs a node allocation per insert plus pointer chasing
// per lookup; step() performs ~100k of each per tick at 512x512. This table
// stores keys/values inline in power-of-two slot arrays, so inserts and finds
// stay in cache and clearing is a memset. Only what step() needs is
// implemented: reserve/clear/insert-or-assign/find/contains/iteration.
// Erase uses tombstones (edit paths only; step() never erases).

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ashfall {

template <typename Key, typename Value, typename Hash>
class FlatMap {
public:
    FlatMap() = default;

    void reserve_for(std::size_t expected_size) {
        std::size_t cap = 64;
        while (cap < expected_size * 2 + 16) cap *= 2;
        if (cap == capacity_) return;
        rehash(cap);
    }

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
        for (;;) {
            const std::uint8_t occ = occupied_[slot];
            if (occ == 0) {
                const std::size_t target =
                    first_tomb >= 0 ? static_cast<std::size_t>(first_tomb) : slot;
                keys_[target] = key;
                values_[target] = std::move(value);
                occupied_[target] = 1;
                if (first_tomb >= 0) --tombstones_;
                ++size_;
                return;
            }
            if (occ == 1 && keys_[slot] == key) {
                values_[slot] = std::move(value);
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
        return *find(key);
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
        if (capacity_ == 0) return -1;
        const std::size_t mask = capacity_ - 1;
        std::size_t slot = static_cast<std::size_t>(hasher_(key)) & mask;
        for (;;) {
            const std::uint8_t occ = occupied_[slot];
            if (occ == 0) return -1;
            if (occ == 1 && keys_[slot] == key) return static_cast<std::ptrdiff_t>(slot);
            slot = (slot + 1) & mask;
        }
    }

    void rehash(std::size_t new_capacity) {
        FlatMap fresh;
        fresh.capacity_ = new_capacity;
        fresh.keys_.assign(new_capacity, Key{});
        fresh.values_.assign(new_capacity, Value{});
        fresh.occupied_.assign(new_capacity, 0);
        for_each([&](const Key& key, const Value& value) { fresh.insert_or_assign(key, value); });
        *this = std::move(fresh);
    }

    Hash hasher_;
    std::vector<Key> keys_;
    std::vector<Value> values_;
    std::vector<std::uint8_t> occupied_; // 0 = empty, 1 = filled, 2 = tombstone
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
    std::size_t tombstones_ = 0;
};

} // namespace ashfall
