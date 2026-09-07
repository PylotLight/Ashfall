#include "ashfall/simulation.hpp"

#include <algorithm>
#include <array>

namespace ashfall {
namespace {

constexpr std::array<std::uint8_t, 8> kSaveMagic = {'A', 'S', 'H', 'F', 'A', 'L', 'L', 1};

int floor_divide(int value, int divisor) {
    const int quotient = value / divisor;
    return value % divisor < 0 ? quotient - 1 : quotient;
}

int positive_modulo(int value, int divisor) {
    const int remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int byte = 0; byte < 4; ++byte) out.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) out.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
}

bool read_u32(std::span<const std::uint8_t> input, std::size_t& offset, std::uint32_t& value) {
    if (offset + 4 > input.size()) return false;
    value = 0;
    for (int byte = 0; byte < 4; ++byte) value |= static_cast<std::uint32_t>(input[offset++]) << (byte * 8);
    return true;
}

bool read_u64(std::span<const std::uint8_t> input, std::size_t& offset, std::uint64_t& value) {
    if (offset + 8 > input.size()) return false;
    value = 0;
    for (int byte = 0; byte < 8; ++byte) value |= static_cast<std::uint64_t>(input[offset++]) << (byte * 8);
    return true;
}

} // namespace

SimulationWorld::SimulationWorld() {
    materials_.emplace(kAir, MaterialDefinition{Movement::Static, false});
    materials_.emplace(kStone, MaterialDefinition{Movement::Static, true});
    materials_.emplace(kSand, MaterialDefinition{Movement::Powder, false});
    materials_.emplace(kWater, MaterialDefinition{Movement::Liquid, false});
}

MaterialId SimulationWorld::cell(CellPosition position) const {
    const CellPosition chunk_position{floor_divide(position.x, kChunkSize), floor_divide(position.y, kChunkSize)};
    const auto chunk = chunks_.find(chunk_position);
    if (chunk == chunks_.end()) return kAir;
    const int x = positive_modulo(position.x, kChunkSize);
    const int y = positive_modulo(position.y, kChunkSize);
    return chunk->second[static_cast<std::size_t>(y * kChunkSize + x)];
}

void SimulationWorld::set_cell(CellPosition position, MaterialId material_id) {
    if (!materials_.contains(material_id)) return;
    CellMap cells = collect_cells();
    if (material_id == kAir) cells.erase(position);
    else cells[position] = material_id;
    rebuild_chunks(cells);
}

void SimulationWorld::fill_rect(CellPosition first, CellPosition last, MaterialId material_id) {
    if (!materials_.contains(material_id)) return;
    const int min_x = std::min(first.x, last.x);
    const int max_x = std::max(first.x, last.x);
    const int min_y = std::min(first.y, last.y);
    const int max_y = std::max(first.y, last.y);
    CellMap cells = collect_cells();
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            if (material_id == kAir) cells.erase({x, y});
            else cells[{x, y}] = material_id;
        }
    }
    rebuild_chunks(cells);
}

void SimulationWorld::step() {
    const CellMap current = collect_cells();
    CellMap next;
    std::vector<std::pair<CellPosition, MaterialId>> dynamic_cells;
    for (const auto& [position, material_id] : current) {
        if (material(material_id).movement == Movement::Static) next[position] = material_id;
        else dynamic_cells.push_back({position, material_id});
    }
    std::sort(dynamic_cells.begin(), dynamic_cells.end(), [](const auto& left, const auto& right) {
        return left.first.y != right.first.y ? left.first.y > right.first.y : left.first.x < right.first.x;
    });
    for (const auto& [source, material_id] : dynamic_cells) {
        CellPosition target = movement_target(source, material_id, next);
        if (!is_empty(target, next)) target = source;
        next[target] = material_id;
    }
    rebuild_chunks(next);
    ++tick_;
}

std::size_t SimulationWorld::occupied_cell_count() const { return collect_cells().size(); }

std::vector<std::uint8_t> SimulationWorld::save() const {
    const CellMap cells = collect_cells();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kSaveMagic.size() + 12 + cells.size() * 9);
    bytes.insert(bytes.end(), kSaveMagic.begin(), kSaveMagic.end());
    append_u64(bytes, tick_);
    append_u32(bytes, static_cast<std::uint32_t>(cells.size()));
    for (const auto& [position, material_id] : cells) {
        append_u32(bytes, static_cast<std::uint32_t>(position.x));
        append_u32(bytes, static_cast<std::uint32_t>(position.y));
        bytes.push_back(material_id);
    }
    return bytes;
}

bool SimulationWorld::load(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kSaveMagic.size() + 12 || !std::equal(kSaveMagic.begin(), kSaveMagic.end(), bytes.begin())) return false;
    std::size_t offset = kSaveMagic.size();
    std::uint64_t saved_tick = 0;
    std::uint32_t count = 0;
    if (!read_u64(bytes, offset, saved_tick) || !read_u32(bytes, offset, count) || bytes.size() - offset != static_cast<std::size_t>(count) * 9) return false;
    CellMap loaded;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t raw_x = 0;
        std::uint32_t raw_y = 0;
        if (!read_u32(bytes, offset, raw_x) || !read_u32(bytes, offset, raw_y) || offset >= bytes.size()) return false;
        const MaterialId material_id = bytes[offset++];
        if (material_id == kAir || !materials_.contains(material_id)) return false;
        loaded[{static_cast<std::int32_t>(raw_x), static_cast<std::int32_t>(raw_y)}] = material_id;
    }
    rebuild_chunks(loaded);
    tick_ = saved_tick;
    return true;
}

const MaterialDefinition& SimulationWorld::material(MaterialId id) const { return materials_.at(id); }

CellPosition SimulationWorld::movement_target(CellPosition source, MaterialId material_id, const CellMap& state) const {
    const bool left_first = prefers_left(source, tick_);
    const std::array<CellPosition, 3> fall_targets = left_first
        ? std::array<CellPosition, 3>{CellPosition{source.x, source.y + 1}, CellPosition{source.x - 1, source.y + 1}, CellPosition{source.x + 1, source.y + 1}}
        : std::array<CellPosition, 3>{CellPosition{source.x, source.y + 1}, CellPosition{source.x + 1, source.y + 1}, CellPosition{source.x - 1, source.y + 1}};
    for (const CellPosition target : fall_targets) if (is_empty(target, state)) return target;
    if (material(material_id).movement == Movement::Liquid) {
        const std::array<CellPosition, 2> sides = left_first
            ? std::array<CellPosition, 2>{CellPosition{source.x - 1, source.y}, CellPosition{source.x + 1, source.y}}
            : std::array<CellPosition, 2>{CellPosition{source.x + 1, source.y}, CellPosition{source.x - 1, source.y}};
        for (const CellPosition target : sides) if (is_empty(target, state)) return target;
    }
    return source;
}

bool SimulationWorld::is_empty(CellPosition position, const CellMap& state) { return !state.contains(position); }

bool SimulationWorld::prefers_left(CellPosition position, std::uint64_t tick) {
    std::uint64_t hash = static_cast<std::uint32_t>(position.x) * 0x9E3779B1u;
    hash ^= static_cast<std::uint32_t>(position.y) * 0x85EBCA77u;
    hash ^= tick * 0xC2B2AE3Du;
    hash ^= hash >> 16;
    return (hash & 1u) == 0;
}

SimulationWorld::CellMap SimulationWorld::collect_cells() const {
    CellMap cells;
    for (const auto& [chunk_position, chunk] : chunks_) {
        for (int y = 0; y < kChunkSize; ++y) for (int x = 0; x < kChunkSize; ++x) {
            const MaterialId material_id = chunk[static_cast<std::size_t>(y * kChunkSize + x)];
            if (material_id != kAir) cells[{chunk_position.x * kChunkSize + x, chunk_position.y * kChunkSize + y}] = material_id;
        }
    }
    return cells;
}

void SimulationWorld::rebuild_chunks(const CellMap& cells) {
    chunks_.clear();
    for (const auto& [position, material_id] : cells) {
        const CellPosition chunk_position{floor_divide(position.x, kChunkSize), floor_divide(position.y, kChunkSize)};
        auto [chunk, inserted] = chunks_.try_emplace(chunk_position);
        if (inserted) chunk->second.assign(static_cast<std::size_t>(kChunkSize * kChunkSize), kAir);
        const int x = positive_modulo(position.x, kChunkSize);
        const int y = positive_modulo(position.y, kChunkSize);
        chunk->second[static_cast<std::size_t>(y * kChunkSize + x)] = material_id;
    }
}

} // namespace ashfall
