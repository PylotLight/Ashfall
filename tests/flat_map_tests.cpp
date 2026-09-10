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

} // namespace

int main() {
    test_reserve_never_shrinks();
    test_shrink_to_fit_after_clear();
    test_stateful_hash_survives_rehash();
    test_probe_accounting_on_collision_chain();
    test_miss_counts_terminating_slot();
    return 0;
}
