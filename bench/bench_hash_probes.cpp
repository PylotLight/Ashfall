// F4 hash-verdict probe measurement (docs/advisory/PERF_TASK_BACKLOG.md).
//
// Standalone bench tool: drives FlatMap with the SAME key type + hasher as
// production (CellPosition / CellPositionHash) through the review's required
// workloads and reports exact probe-length distributions. Measurement only:
// the engine is untouched (this TU links ashfall_sim for the representative
// world capture, but never modifies engine behavior).
//
// Why per-op counter deltas: FlatMap's built-in histogram buckets are too
// coarse for the p95 <= ~3 verdict (bucket "3-4" conflates 3 and 4), so each
// op runs between reset_probe_counters() and a probe_counters() read, which
// yields its EXACT examined-slot count (terminating empty slot included on
// miss, rehash churn excluded by the FlatMap itself). The sim never reads
// these counters, so this is behavior-neutral.
//
// Workloads (review §F4):
//   dense 64^2 / 256^2 / 512^2 heated regions (row-major build order, as
//     fill_rect produces), plus translated (+1000,+2000) and negative-coord
//     ((-300,-200) spanning zero, (-1000,-1000) fully negative) variants;
//   H/V strips (8192-long);
//   {64i+ox, 64j+oy} chunk-aligned sparse grids for several (ox,oy) — the
//     PROVEN low-4-bit correlation case (same within-chunk offset =>
//     identical low 4 hash bits => 1-in-16 buckets);
//   cold table (64 entries, miss-heavy reads);
//   real hot-path key sequences from a live world (256^2 heated stone block,
//     one step() taken): conduction candidates H U N(H) in (x,y) order with
//     5x stencil reads + conducted inserts sized reserve_for(5H+16) (mirrors
//     conduct_temperature's sparse path verbatim), chunk-walk order inserts
//     (std::map<CellPosition> x-first chunk order + row-major within chunk,
//     mirrors for_each_chunk_cell), and movement order inserts (y-desc,x-asc,
//     mirrors order_dynamic_cells).
//
// Table types: FlatTemps (int16_t: temps_ / temp_scratch_ /
// heat_next_scratch_), FlatCells (MaterialId: next_scratch_), FlatMarks
// (uint8_t: stayed_scratch_). Probes are key-hash-only, so types must agree;
// the harness verifies that rather than assuming it.
//
// Build wiring: same pattern as bench_step / oracle_record (CMakeLists,
// manual tool, NOT in ctest).
//   cmake --build build --target bench_hash_probes
//   ./build/bench_hash_probes            (default Release build dir)
//
// Report columns: occupancy (size/capacity/load), insert probe p50/p95/p99/
// max/mean, hit-find p50/p95/p99/max/mean, miss-find mean/p95/max, total
// probes (== probes/tick for a per-tick rebuilt scratch table).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

#include "ashfall/cell_types.hpp"
#include "ashfall/flat_cell_map.hpp"
#include "ashfall/simulation.hpp"

namespace {

using ashfall::CellPosition;
using ashfall::CellPositionHash;
using TempMap = ashfall::FlatMap<CellPosition, std::int16_t, CellPositionHash>;
using CellMap = ashfall::FlatMap<CellPosition, ashfall::MaterialId, CellPositionHash>;
using MarkMap = ashfall::FlatMap<CellPosition, std::uint8_t, CellPositionHash>;

struct Quant {
    std::uint64_t p50 = 0, p95 = 0, p99 = 0, max = 0;
    double mean = 0.0;
    std::uint64_t total = 0;
};

Quant summarize(std::vector<std::uint64_t> lens) {
    Quant q;
    if (lens.empty()) return q;
    q.total = 0;
    for (std::uint64_t v : lens) q.total += v;
    q.mean = static_cast<double>(q.total) / static_cast<double>(lens.size());
    std::sort(lens.begin(), lens.end());
    const std::size_t n = lens.size();
    auto rank = [&](double f) -> std::uint64_t {
        std::size_t idx = static_cast<std::size_t>(f * static_cast<double>(n));
        if (idx == 0) idx = 1;
        if (idx > n) idx = n;
        return lens[idx - 1]; // nearest-rank (ceil for the sampled fractions)
    };
    // ceil(q*n)-1 indexing for exact nearest-rank quantiles.
    auto ceil_rank = [&](double f) -> std::uint64_t {
        std::size_t idx = static_cast<std::size_t>(f * static_cast<double>(n));
        if (static_cast<double>(idx) < f * static_cast<double>(n)) ++idx;
        if (idx < 1) idx = 1;
        if (idx > n) idx = n;
        return lens[idx - 1];
    };
    (void)rank;
    q.p50 = ceil_rank(0.50);
    q.p95 = ceil_rank(0.95);
    q.p99 = ceil_rank(0.99);
    q.max = lens.back();
    return q;
}

template <typename Map>
std::uint64_t insert_probe(Map& map, CellPosition key) {
    map.reset_probe_counters();
    map.insert_or_assign(key, typename std::decay<decltype(*map.find(key))>::type{});
    const auto c = map.probe_counters();
    return c.insert_probes + c.update_probes; // exactly one walk recorded
}

template <typename Map>
std::uint64_t find_probe(Map& map, CellPosition key, bool& hit) {
    map.reset_probe_counters();
    hit = (map.find(key) != nullptr);
    const auto c = map.probe_counters();
    return hit ? c.hit_probes : c.miss_probes;
}

// Insert every key (fresh table, pre-sized like production), then re-find
// every key (hits), then find N absent keys (misses). Returns insert/hit/
// miss length vectors; occupancy via out params.
template <typename Map>
void run_insert_find(Map& map, const std::vector<CellPosition>& keys,
                     const std::vector<CellPosition>& absent, std::vector<std::uint64_t>& ins,
                     std::vector<std::uint64_t>& hits, std::vector<std::uint64_t>& misses) {
    ins.clear();
    hits.clear();
    misses.clear();
    ins.reserve(keys.size());
    hits.reserve(keys.size());
    misses.reserve(absent.size());
    for (const CellPosition k : keys) ins.push_back(insert_probe(map, k));
    bool hit = false;
    for (const CellPosition k : keys) hits.push_back(find_probe(map, k, hit));
    for (const CellPosition k : absent) misses.push_back(find_probe(map, k, hit));
}

void print_row(const std::string& table, const std::string& workload, const std::vector<CellPosition>& keys,
               std::size_t size, std::size_t cap, const Quant& ins, const Quant& hitq, const Quant& missq) {
    const double load = cap == 0 ? 0.0 : static_cast<double>(size) / static_cast<double>(cap);
    std::printf("%-9s %-28s N=%-7zu cap=%-7zu load=%5.1f%% | ins p50=%-3llu p95=%-3llu p99=%-3llu max=%-4llu mean=%6.3f | hit p50=%-3llu p95=%-3llu p99=%-3llu max=%-4llu mean=%6.3f | miss mean=%6.3f p95=%-3llu max=%-4llu | total=%llu\n",
                table.c_str(), workload.c_str(), keys.size(), cap, 100.0 * load,
                static_cast<unsigned long long>(ins.p50), static_cast<unsigned long long>(ins.p95),
                static_cast<unsigned long long>(ins.p99), static_cast<unsigned long long>(ins.max), ins.mean,
                static_cast<unsigned long long>(hitq.p50), static_cast<unsigned long long>(hitq.p95),
                static_cast<unsigned long long>(hitq.p99), static_cast<unsigned long long>(hitq.max), hitq.mean,
                missq.mean, static_cast<unsigned long long>(missq.p95),
                static_cast<unsigned long long>(missq.max),
                static_cast<unsigned long long>(ins.total + hitq.total + missq.total));
}

// --- key generators (row-major build order, as fill_rect produces) ---

std::vector<CellPosition> dense(int w, int h, int ox, int oy) {
    std::vector<CellPosition> keys;
    keys.reserve(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) keys.push_back({ox + x, oy + y});
    return keys;
}

std::vector<CellPosition> hstrip(int n, int y, int x0) {
    std::vector<CellPosition> keys;
    keys.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) keys.push_back({x0 + i, y});
    return keys;
}

std::vector<CellPosition> vstrip(int n, int x, int y0) {
    std::vector<CellPosition> keys;
    keys.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) keys.push_back({x, y0 + i});
    return keys;
}

// Chunk-aligned sparse features: same within-chunk offset everywhere.
std::vector<CellPosition> sparse64(int ni, int nj, int ox, int oy) {
    std::vector<CellPosition> keys;
    keys.reserve(static_cast<std::size_t>(ni) * static_cast<std::size_t>(nj));
    for (int j = 0; j < nj; ++j)
        for (int i = 0; i < ni; ++i) keys.push_back({64 * i + ox, 64 * j + oy});
    return keys;
}

// Absent keys for the miss phase: same shape shifted far away (no overlap).
std::vector<CellPosition> shifted(const std::vector<CellPosition>& keys, int dx, int dy) {
    std::vector<CellPosition> out;
    out.reserve(keys.size());
    for (const CellPosition k : keys) out.push_back({k.x + dx, k.y + dy});
    return out;
}

template <typename Map>
void measure(const std::string& table, const std::string& workload, const std::vector<CellPosition>& keys,
             std::size_t reserve_n) {
    Map map;
    map.reserve_for(reserve_n); // production sizing: bit_ceil(2E+16)-style ~50% load
    std::vector<std::uint64_t> ins, hits, misses;
    run_insert_find(map, keys, shifted(keys, 100000, -70000), ins, hits, misses);
    print_row(table, workload, keys, map.size(), map.capacity(), summarize(std::move(ins)),
              summarize(std::move(hits)), summarize(std::move(misses)));
}

} // namespace

int main() {
    std::printf("# F4 probe verdict: exact probe lengths (examined slots incl. terminating empty)\n");
    std::printf("# build: %s %s\n", __DATE__, __TIME__);
#ifdef NDEBUG
    std::printf("# NDEBUG (Release-like, asserts off)\n");
#else
    std::printf("# DEBUG (asserts on)\n");
#endif

    std::printf("\n## synthetic (TempMap = production temperature-table type)\n");
    {
        const auto d64 = dense(64, 64, 0, 0);
        measure<TempMap>("temps", "dense64@0,0", d64, d64.size());
    }
    {
        const auto d256 = dense(256, 256, 0, 0);
        measure<TempMap>("temps", "dense256@0,0", d256, d256.size());
    }
    {
        const auto d512 = dense(512, 512, 0, 0);
        measure<TempMap>("temps", "dense512@0,0", d512, d512.size());
    }
    {
        const auto t = dense(256, 256, 1000, 2000);
        measure<TempMap>("temps", "dense256@1000,2000", t, t.size());
    }
    {
        const auto t = dense(256, 256, -300, -200);
        measure<TempMap>("temps", "dense256@-300,-200", t, t.size());
    }
    {
        const auto t = dense(64, 64, -1000, -1000);
        measure<TempMap>("temps", "dense64@-1000,-1000", t, t.size());
    }
    {
        const auto s = hstrip(8192, 5, 0);
        measure<TempMap>("temps", "hstrip8192", s, s.size());
    }
    {
        const auto s = vstrip(8192, 5, 0);
        measure<TempMap>("temps", "vstrip8192", s, s.size());
    }
    for (const auto [ox, oy] : {std::tuple<int, int>{0, 0}, {1, 1}, {13, 37}, {63, 63}, {7, 31}}) {
        char name[64];
        std::snprintf(name, sizeof(name), "sparse64(ox=%d,oy=%d)", ox, oy);
        const auto s = sparse64(48, 48, ox, oy);
        measure<TempMap>("temps", name, s, s.size());
    }
    {
        // Cold table: 64 stored entries, miss-heavy read mix.
        const auto c = dense(8, 8, 10, 10);
        TempMap map;
        map.reserve_for(c.size());
        for (const CellPosition k : c) insert_probe(map, k);
        std::vector<std::uint64_t> misses;
        misses.reserve(4096);
        bool hit = false;
        for (int i = 0; i < 4096; ++i) misses.push_back(find_probe(map, {100 + i, 900 - i}, hit));
        print_row("temps", "cold64+4k-miss", c, map.size(), map.capacity(), Quant{}, Quant{},
                  summarize(std::move(misses)));
    }

    // --- real hot-path key sequences from a live world ---
    std::printf("\n## live world: 256^2 heated stone block, 1 step() taken\n");
    ashfall::SimulationWorld world;
    world.fill_rect({0, 0}, {255, 255}, ashfall::kStone);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) world.set_temperature({x, y}, 1000);
    world.step();
    // Post-step nonzero-temp set H (bounding box scan; conduction spreads a fringe).
    std::vector<CellPosition> heat;
    for (int y = -2; y < 258; ++y)
        for (int x = -2; x < 258; ++x)
            if (world.temperature({x, y}) != 0) heat.push_back({x, y});
    std::printf("# post-step nonzero temps H=%zu\n", heat.size());

    // 1. Conduction sparse path: candidates = H U N(H), sorted (x,y) [verbatim
    //    from conduct_temperature], 5 stencil reads each + conducted inserts
    //    into a table sized reserve_for(5H+16).
    {
        std::vector<CellPosition> cand;
        cand.reserve(heat.size() * 5);
        for (const CellPosition p : heat) {
            cand.push_back(p);
            cand.push_back({p.x - 1, p.y});
            cand.push_back({p.x + 1, p.y});
            cand.push_back({p.x, p.y - 1});
            cand.push_back({p.x, p.y + 1});
        }
        std::sort(cand.begin(), cand.end(),
                  [](const CellPosition& a, const CellPosition& b) { return std::tie(a.x, a.y) < std::tie(b.x, b.y); });
        cand.erase(std::unique(cand.begin(), cand.end(),
                               [](const CellPosition& a, const CellPosition& b) { return a.x == b.x && a.y == b.y; }),
                   cand.end());
        std::printf("# conduction candidates=%zu\n", cand.size());
        TempMap temps, conducted;
        temps.reserve_for(heat.size());
        for (const CellPosition p : heat) insert_probe(temps, p); // tick-start table
        conducted.reserve_for(heat.size() * 5 + 16);              // production sizing
        std::vector<std::uint64_t> reads, ins;
        reads.reserve(cand.size() * 5);
        bool hit = false;
        std::uint64_t read_hits = 0;
        for (const CellPosition p : cand) {
            // 5-snapshot-stencil reads (temperature_at finds on temps_).
            const CellPosition q[5] = {p, {p.x - 1, p.y}, {p.x + 1, p.y}, {p.x, p.y - 1}, {p.x, p.y + 1}};
            for (const CellPosition r : q) {
                reads.push_back(find_probe(temps, r, hit));
                read_hits += hit ? 1 : 0;
            }
            ins.push_back(insert_probe(conducted, p)); // conducted insert (over-est: real pass skips zeros)
        }
        const Quant rq = summarize(std::move(reads));
        const Quant iq = summarize(std::move(ins));
        std::printf("# stencil reads: 5/candidate, hit fraction=%.3f\n",
                    static_cast<double>(read_hits) / static_cast<double>(cand.size() * 5));
        print_row("temps", "conduct-reads(x,y)", cand, temps.size(), temps.capacity(), Quant{}, rq, Quant{});
        print_row("temps", "conduct-insert(5H)", cand, conducted.size(), conducted.capacity(), iq, Quant{}, Quant{});
    }

    // 2. Chunk-walk order (for_each_chunk_cell: map x-first chunk order, row-major in chunk).
    {
        std::vector<CellPosition> walk = heat;
        std::sort(walk.begin(), walk.end(), [](const CellPosition& a, const CellPosition& b) {
            const int acx = a.x >= 0 ? a.x / 64 : (a.x - 63) / 64;
            const int acy = a.y >= 0 ? a.y / 64 : (a.y - 63) / 64;
            const int bcx = b.x >= 0 ? b.x / 64 : (b.x - 63) / 64;
            const int bcy = b.y >= 0 ? b.y / 64 : (b.y - 63) / 64;
            return std::tie(acx, acy, a.y, a.x) < std::tie(bcx, bcy, b.y, b.x);
        });
        measure<TempMap>("temps", "chunk-walk(H)", walk, walk.size());
        measure<CellMap>("cells", "chunk-walk(H)", walk, walk.size() + 16); // next_scratch_ sizing
        measure<MarkMap>("marks", "chunk-walk(H)", walk, walk.size());      // stayed_scratch_ sizing
    }

    // 3. Movement order (order_dynamic_cells: y-desc, x-asc).
    {
        std::vector<CellPosition> mov = heat;
        std::sort(mov.begin(), mov.end(),
                  [](const CellPosition& a, const CellPosition& b) { return std::tie(b.y, a.x) < std::tie(a.y, b.x); });
        measure<TempMap>("temps", "movement-order(H)", mov, mov.size());
        measure<CellMap>("cells", "movement-order(H)", mov, mov.size() + 16);
        measure<MarkMap>("marks", "movement-order(H)", mov, mov.size());
    }

    // 4. Scratch-type parity on synthetic shapes (hash is key-only; verify, don't assume).
    std::printf("\n## scratch-type parity (same keys => same probes?)\n");
    {
        const auto d256 = dense(256, 256, 0, 0);
        measure<CellMap>("cells", "dense256@0,0", d256, d256.size() + 16);
        measure<MarkMap>("marks", "dense256@0,0", d256, d256.size());
        const auto s = sparse64(48, 48, 13, 37);
        measure<CellMap>("cells", "sparse64(13,37)", s, s.size() + 16);
        measure<MarkMap>("marks", "sparse64(13,37)", s, s.size());
    }
    // --- the review's literal criterion: p95 at 50% load (production
    // reserve_for() sizes to ~25%; these runs pin capacity to 2N via the
    // reserve argument so load lands exactly on 50%). ---
    std::printf("\n## 50%%-load runs (review criterion, capacity pinned to 2N)\n");
    {
        const auto d64 = dense(64, 64, 0, 0); // N=4096 -> cap 8192
        measure<TempMap>("temps", "dense64@50%", d64, 4088);
    }
    {
        const auto d256 = dense(256, 256, 0, 0); // N=65536 -> cap 131072
        measure<TempMap>("temps", "dense256@50%", d256, 65528);
    }
    {
        const auto d512 = dense(512, 512, 0, 0); // N=262144 -> cap 524288
        measure<TempMap>("temps", "dense512@50%", d512, 262136);
    }
    {
        auto s = sparse64(48, 48, 13, 37);
        s.resize(2048); // N=2048 -> cap 4096
        measure<TempMap>("temps", "sparse64(13,37)@50%", s, 2040);
    }
    return 0;
}
