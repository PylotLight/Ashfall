// Task B (Group 1) characterization tests for ashfall::FlatMap hygiene (F1)
// and probe counters (F4).
//
// Standalone binary using the same `expect` convention as
// tests/simulation_tests.cpp: silent on success (exit 0); the first failure
// prints to stderr and exits non-zero. The integrator wires this file into
// CMake; it also compiles standalone:
//   c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I engine/include \
//       tests/flat_map_tests.cpp -o /tmp/flat_verify && /tmp/flat_verify

#include "ashfall/flat_cell_map.hpp"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(EXIT_FAILURE); }
}

struct IntHash {
    std::size_t operator()(int key) const noexcept {
        return static_cast<std::size_t>(std::hash<int>{}(key));
    }
};

// Degenerate hash: every key collides at slot 0, building a hand-checkable
// linear-probe chain.
struct ConstHash {
    std::size_t operator()(int) const noexcept { return 0; }
};

// Stateful hash: unique id per default construction (copies preserve it) that
// feeds the hash output, plus a per-instance call counter and a static log of
// the most recently used id. If rehash silently swapped in a
// default-constructed hasher, lookups would land on wrong slots (the id feeds
// the mix) and last_used_id would change.
struct StatefulHash {
    int id = 0;
    mutable std::uint64_t calls = 0;
    static inline int next_id = 1;
    static inline int last_used_id = 0;

    StatefulHash() : id(next_id++) {}
    StatefulHash(const StatefulHash& other) : id(other.id), calls(other.calls) {}
    StatefulHash& operator=(const StatefulHash& other) {
        id = other.id;
        calls = other.calls;
        return *this;
    }

    std::size_t operator()(int key) const noexcept {
        ++calls;
        last_used_id = id;
        std::uint64_t h = static_cast<std::uint64_t>(static_cast<std::uint32_t>(key)) ^
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(id)) * 0x9E3779B1u);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return static_cast<std::size_t>(h);
    }
};

template <typename Counters>
std::uint64_t walk_total(const Counters& c) {
    return c.hits + c.misses + c.inserts + c.updates;
}

template <typename Counters>
std::uint64_t histogram_total(const Counters& c) {
    std::uint64_t total = 0;
    for (const std::uint64_t bucket : c.histogram) total += bucket;
    return total;
}

void test_reserve_never_shrinks() {
    ashfall::FlatMap<int, int, IntHash> map;
    map.reserve_for(1000);
    // Formula trace: 64 -> ... -> 2048 is the first cap with 2048 >= 1000*2+16.
    const std::size_t big = map.capacity();
    expect(big == std::size_t{2048}, "reserve_for(1000) sizes capacity to 2048");
    for (int i = 0; i < 100; ++i) map.insert_or_assign(i, i);
    map.reserve_for(10);
    expect(map.capacity() == big, "reserve_for(small) after reserve_for(big) never shrinks");
    for (int i = 0; i < 100; ++i) {
        expect(map.find(i) != nullptr, "entries survive the small reserve");
    }
}

void test_shrink_to_fit_after_clear() {
    ashfall::FlatMap<int, int, IntHash> map;
    for (int i = 0; i < 500; ++i) map.insert_or_assign(i, i * 2);
    expect(map.capacity() > std::size_t{64}, "bulk insert grows capacity past the minimum");
    map.clear();
    expect(map.size() == 0u, "clear empties the map");
    map.shrink_to_fit();
    expect(map.capacity() == std::size_t{64}, "shrink_to_fit returns to 64 with 0 entries");
    expect(map.size() == 0u, "shrink_to_fit keeps the map empty");
    map.insert_or_assign(7, 70);
    const int* found = map.find(7);
    expect(found != nullptr && *found == 70, "map stays usable after shrink_to_fit");
}

void test_stateful_hash_survives_rehash() {
    const int ids_before = StatefulHash::next_id;
    ashfall::FlatMap<int, int, StatefulHash> map;
    // The map's hasher_ is default-constructed once here, taking this id.
    const int map_id = ids_before;
    for (int i = 0; i < 300; ++i) map.insert_or_assign(i, i * 10); // multiple rehashes
    for (int i = 0; i < 300; ++i) {
        const int* value = map.find(i);
        expect(value != nullptr && *value == i * 10,
            "stateful hash still locates every entry after rehash");
    }
    expect(StatefulHash::last_used_id == map_id, "same hasher id still used after rehash");
}

void test_probe_accounting_on_collision_chain() {
    ashfall::FlatMap<int, int, ConstHash> map;
    // 50 colliding keys cross the 32-entry growth rehash (cap 64 -> 128), so
    // inserts == 50 also proves rehash's internal re-inserts stay unrecorded.
    constexpr int kKeys = 50;
    for (int i = 1; i <= kKeys; ++i) map.insert_or_assign(i, i * 10);
    expect(map.probe_counters().inserts == 50u, "all logical inserts recorded despite rehash");

    map.insert_or_assign(1, 999); // update: key 1 sits at slot 0, a 1-probe walk
    {
        const auto c = map.probe_counters();
        expect(c.updates == 1u, "re-insert records one update");
        expect(c.update_probes == 1u, "update counts its single examined slot");
    }

    const int* updated = map.find(1); // hit: 1 probe
    expect(updated != nullptr && *updated == 999, "update stores the new value");
    expect(map.find(9999) == nullptr, "missing key not found"); // miss walk
    map.erase(2); // hit walk
    expect(map.find(2) == nullptr, "erased key is gone"); // miss walk
    int& third = map[3]; // hit walk (key 3 sits at slot 2: 3 probes)
    expect(third == 30, "operator[] on a hit returns the value");
    int& fresh = map[1000]; // miss walk + insert walk (default-constructs)
    expect(fresh == 0, "operator[] on a miss default-constructs");

    const auto c = map.probe_counters();
    expect(c.inserts == 51u, "operator[] miss adds one insert");
    expect(c.updates == 1u, "no extra updates recorded");
    expect(c.hits == 3u, "find hit + erase hit + operator[] hit");
    expect(c.misses == 3u, "find miss + erased-key miss + operator[] miss");
    expect(c.max_probes >= 2u, "collision chain probes reach depth");
    expect(histogram_total(c) == walk_total(c), "histogram buckets sum to total walks");

    // Functional checks after the counter snapshot (their walks are irrelevant now).
    expect(!map.contains(2), "erased key stays erased");
    expect(map.contains(1000), "operator[]-inserted key present");

    map.reset_probe_counters();
    const auto cleared = map.probe_counters();
    expect(walk_total(cleared) == 0u, "reset clears walk counts");
    expect(cleared.hit_probes == 0u && cleared.miss_probes == 0u &&
            cleared.insert_probes == 0u && cleared.update_probes == 0u,
        "reset clears probe sums");
    expect(cleared.max_probes == 0u, "reset clears max_probes");
    expect(histogram_total(cleared) == 0u, "reset clears the histogram");
}

void test_miss_counts_terminating_slot() {
    ashfall::FlatMap<int, int, IntHash> map;
    map.reserve_for(1); // allocate capacity, keep the table entry-free
    map.reset_probe_counters();
    expect(map.find(12345) == nullptr, "missing key not found in empty table");
    const auto c = map.probe_counters();
    expect(c.misses == 1u, "empty-table find records one miss");
    expect(c.miss_probes >= 1u, "miss counts the terminating empty slot");
    expect(c.hits == 0u && c.inserts == 0u && c.updates == 0u, "no other walks recorded");
}

void test_find_or_insert_hit_keeps_value() {
    ashfall::FlatMap<int, int, IntHash> map;
    map.insert_or_assign(42, 100);
    map.reset_probe_counters();
    auto result = map.find_or_insert(42, 999);
    expect(!result.inserted, "find_or_insert on a present key reports inserted=false");
    expect(result.ptr != nullptr && *result.ptr == 100,
        "find_or_insert hit returns the EXISTING value (no overwrite)");
    expect(map.size() == 1u, "find_or_insert hit does not change size");
    const auto c = map.probe_counters();
    expect(c.hits == 1u && c.inserts == 0u && c.updates == 0u && c.misses == 0u,
        "hit records exactly one hit walk (no insert/update)");
    expect(histogram_total(c) == walk_total(c), "histogram buckets sum to total walks");
    // Functional checks after the counter snapshot (their walks are irrelevant now).
    const int* stored = map.find(42);
    expect(stored != nullptr && *stored == 100, "stored value unchanged after hit");
    expect(result.ptr == stored, "returned ptr aliases the table's value slot");
}

void test_find_or_insert_miss_inserts() {
    ashfall::FlatMap<int, int, IntHash> map;
    map.reserve_for(1);
    map.reset_probe_counters();
    auto result = map.find_or_insert(7, 70);
    expect(result.inserted, "find_or_insert on an absent key reports inserted=true");
    expect(result.ptr != nullptr && *result.ptr == 70, "miss stores the passed value");
    expect(map.size() == 1u, "miss grows the map by one");
    const auto c = map.probe_counters();
    expect(c.inserts == 1u && c.hits == 0u && c.updates == 0u && c.misses == 0u,
        "miss records exactly one insert walk (no hit/miss/update)");
    expect(c.insert_probes >= 1u, "insert walk counts its examined slots");
    // Functional checks after the counter snapshot (their walks are irrelevant now).
    const int* stored = map.find(7);
    expect(stored != nullptr && *stored == 70, "missed key is findable afterwards");
    expect(result.ptr == stored, "returned ptr aliases the inserted slot");
    // A second claim on the same key observes without overwriting — the Task S
    // duplicate-check pattern (find_or_insert + !inserted → reject).
    auto dup = map.find_or_insert(7, 777);
    expect(!dup.inserted && *dup.ptr == 70, "duplicate claim observes the first value");
    expect(map.size() == 1u, "duplicate claim does not add an entry");
}

void test_find_or_insert_growth_rules() {
    ashfall::FlatMap<int, int, IntHash> map;
    for (int i = 0; i < 32; ++i) map.insert_or_assign(i, i);
    // Growth fires when (size+tombstones)*2 >= capacity BEFORE an insert, so
    // 32 entries still fit the minimum 64-slot table exactly at threshold.
    expect(map.capacity() == std::size_t{64}, "32 entries sit at the 64-slot threshold");
    const std::size_t cap = map.capacity();
    auto hit = map.find_or_insert(5, 5000);
    expect(!hit.inserted && *hit.ptr == 5, "hit at threshold observes the value");
    expect(map.capacity() == cap, "hit at threshold never grows the table");
    auto miss = map.find_or_insert(1000, 7);
    expect(miss.inserted && *miss.ptr == 7, "miss at threshold still inserts");
    expect(map.capacity() == cap * 2, "miss at threshold grows first (64 -> 128)");
    for (int i = 0; i < 32; ++i) {
        const int* value = map.find(i);
        expect(value != nullptr && *value == i, "pre-growth entries survive the miss growth");
    }
    expect(*map.find(1000) == 7, "threshold-miss entry readable after growth");
}

void test_find_or_insert_collision_chain() {
    ashfall::FlatMap<int, int, ConstHash> map;
    for (int i = 1; i <= 10; ++i) {
        auto r = map.find_or_insert(i, i * 10);
        expect(r.inserted && *r.ptr == i * 10, "colliding miss inserts at the chain tail");
    }
    expect(map.size() == 10u, "all colliding inserts land");
    map.reset_probe_counters();
    auto deep = map.find_or_insert(10, 9999);
    expect(!deep.inserted && *deep.ptr == 100, "deep-chain hit keeps its value");
    const auto c = map.probe_counters();
    expect(c.hits == 1u && c.inserts == 0u, "deep-chain hit records one hit walk");
    map.erase(3);
    auto reused = map.find_or_insert(3, 333);
    expect(reused.inserted && *reused.ptr == 333, "claim reuses a tombstone slot");
    for (int i = 1; i <= 10; ++i) {
        const int expected = (i == 3) ? 333 : i * 10;
        const int* value = map.find(i);
        expect(value != nullptr && *value == expected, "chain intact across tombstone reuse");
    }
}

void test_insert_or_assign_still_overwrites() {
    ashfall::FlatMap<int, int, IntHash> map;
    map.insert_or_assign(1, 10);
    auto observed = map.find_or_insert(1, 20);
    expect(!observed.inserted && *observed.ptr == 10,
        "find_or_insert does NOT overwrite (stays distinct from insert_or_assign)");
    map.reset_probe_counters();
    map.insert_or_assign(1, 20);
    expect(*map.find(1) == 20, "insert_or_assign still overwrites the value");
    const auto c = map.probe_counters();
    expect(c.updates == 1u && c.inserts == 0u, "overwrite records an update, not an insert");
}

} // namespace

int main() {
    test_reserve_never_shrinks();
    test_shrink_to_fit_after_clear();
    test_stateful_hash_survives_rehash();
    test_probe_accounting_on_collision_chain();
    test_miss_counts_terminating_slot();
    test_find_or_insert_hit_keeps_value();
    test_find_or_insert_miss_inserts();
    test_find_or_insert_growth_rules();
    test_find_or_insert_collision_chain();
    test_insert_or_assign_still_overwrites();
    return 0;
}
