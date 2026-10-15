// Identity oracle record tool (Group 4, Task O0).
//
// For each scenario (A–F) rebuilds via tests/oracle_scenarios.hpp, steps N
// ticks (A–D: 200; E: 20; F: 10), and writes tests/oracle/<name>.hash lines
// `tick_index hexhash` where hexhash is FNV-1a-64 over that tick's
// world.save() bytes, plus a `#` header comment recording the git SHA the
// recording was taken from. Save bytes are canonical (v3 deterministic:
// ordered map walk, cell-ordered temps) so hashes are stable across
// runs/machines for identical code.
//
// Build manually (integrator wires the CMake target; NOT part of ctest):
//   c++ -std=c++20 -O2 -I engine/include -I tests bench/oracle_record.cpp \
//       build/libashfall_sim.a -o /tmp/oracle_record
// Run from the repo root:
//   /tmp/oracle_record            (writes tests/oracle/*.hash)
// Determinism self-check:
//   /tmp/oracle_record && cp tests/oracle/A.hash /tmp/a1.hash && \
//   /tmp/oracle_record && diff /tmp/a1.hash tests/oracle/A.hash
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ashfall/simulation.hpp"
#include "oracle_scenarios.hpp"

namespace {

std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string git_sha() {
    // Recorded-from SHA for the header comment; "unknown" if git is absent.
    std::string sha = "unknown";
    FILE* pipe = ::popen("git rev-parse HEAD 2>/dev/null", "r");
    if (pipe != nullptr) {
        char buf[128] = {0};
        if (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
            sha = buf;
            while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r')) {
                sha.pop_back();
            }
        }
        ::pclose(pipe);
    }
    return sha;
}

struct Scenario {
    const char* name;
    bool (*build)(ashfall::SimulationWorld&);
    int ticks;
};

bool record_one(const Scenario& scenario, const std::string& dir, const std::string& sha) {
    ashfall::SimulationWorld world;
    if (!scenario.build(world)) {
        std::printf("oracle_record: build %s FAILED\n", scenario.name);
        return false;
    }
    const std::string path = dir + "/" + scenario.name + ".hash";
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        std::printf("oracle_record: cannot open %s\n", path.c_str());
        return false;
    }
    out << "# oracle " << scenario.name << " ticks=" << scenario.ticks << " sha=" << sha << "\n";
    for (int tick = 0; tick < scenario.ticks; ++tick) {
        world.step();
        const std::vector<std::uint8_t> bytes = world.save();
        const std::uint64_t hash = fnv1a64(bytes);
        char line[64];
        std::snprintf(line, sizeof(line), "%d %016llx", tick,
            static_cast<unsigned long long>(hash));
        out << line << "\n";
    }
    out.close();
    std::printf("oracle_record: %s -> %s (%d ticks)\n", scenario.name, path.c_str(),
        scenario.ticks);
    return true;
}

} // namespace

int main() {
    const std::array<Scenario, 6> scenarios = {{
        {"A", oracle::build_oracle_A, oracle::kMeasureTicksA},
        {"B", oracle::build_oracle_B, oracle::kMeasureTicksB},
        {"C", oracle::build_oracle_C, oracle::kMeasureTicksC},
        {"D", oracle::build_oracle_D, oracle::kMeasureTicksD},
        {"E", oracle::build_oracle_E, oracle::kMeasureTicksE},
        {"F", oracle::build_oracle_F, oracle::kMeasureTicksF},
    }};
    // Writable from the repo root; fall back to build-dir-relative.
    std::string dir = "tests/oracle";
    if (!std::filesystem::exists("tests/oracle_scenarios.hpp")) {
        dir = "../tests/oracle";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string sha = git_sha();
    bool ok = true;
    for (const Scenario& scenario : scenarios) {
        ok = record_one(scenario, dir, sha) && ok;
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
