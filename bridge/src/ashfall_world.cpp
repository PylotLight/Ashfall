#include "ashfall_world.hpp"

#include "ashfall/material_data.hpp"
#include "ashfall/reaction_data.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

namespace {
inline double present_ms_between(
    const std::chrono::steady_clock::time_point& start,
    const std::chrono::steady_clock::time_point& end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
} // namespace

void AshfallWorld::_bind_methods() {
    ClassDB::bind_method(D_METHOD("init_world", "width", "height"), &AshfallWorld::init_world);
    ClassDB::bind_method(D_METHOD("step"), &AshfallWorld::step);
    ClassDB::bind_method(D_METHOD("set_cell", "x", "y", "material_id"), &AshfallWorld::set_cell);
    ClassDB::bind_method(D_METHOD("get_cell", "x", "y"), &AshfallWorld::get_cell);
    ClassDB::bind_method(D_METHOD("fill_rect", "x1", "y1", "x2", "y2", "material_id"), &AshfallWorld::fill_rect);
    ClassDB::bind_method(D_METHOD("get_tick"), &AshfallWorld::get_tick);
    ClassDB::bind_method(D_METHOD("get_occupied_cell_count"), &AshfallWorld::get_occupied_cell_count);
    ClassDB::bind_method(D_METHOD("get_dirty_chunks"), &AshfallWorld::get_dirty_chunks);
    ClassDB::bind_method(D_METHOD("clear_dirty_chunks"), &AshfallWorld::clear_dirty_chunks);
    ClassDB::bind_method(D_METHOD("setup_reference_world"), &AshfallWorld::setup_reference_world);
    ClassDB::bind_method(D_METHOD("setup_play_level"), &AshfallWorld::setup_play_level);
    ClassDB::bind_method(D_METHOD("get_player_spawn"), &AshfallWorld::get_player_spawn);
    ClassDB::bind_method(D_METHOD("is_solid_cell", "x", "y"), &AshfallWorld::is_solid_cell);
    ClassDB::bind_method(D_METHOD("can_move_to_box", "x1", "y1", "x2", "y2"), &AshfallWorld::can_move_to_box);
    ClassDB::bind_method(D_METHOD("save_to_bytes"), &AshfallWorld::save_to_bytes);
    ClassDB::bind_method(D_METHOD("load_from_bytes", "bytes"), &AshfallWorld::load_from_bytes);
    ClassDB::bind_method(D_METHOD("load_materials", "json_text"), &AshfallWorld::load_materials);
    ClassDB::bind_method(D_METHOD("load_reactions", "json_text"), &AshfallWorld::load_reactions);
    ClassDB::bind_method(D_METHOD("get_material_count"), &AshfallWorld::get_material_count);
    ClassDB::bind_method(D_METHOD("get_material_ids"), &AshfallWorld::get_material_ids);
    ClassDB::bind_method(D_METHOD("get_material_name", "material_id"), &AshfallWorld::get_material_name);
    ClassDB::bind_method(D_METHOD("get_material_color", "material_id"), &AshfallWorld::get_material_color);
    ClassDB::bind_method(D_METHOD("get_material_movement", "material_id"), &AshfallWorld::get_material_movement);

    ClassDB::bind_method(D_METHOD("set_auto_step", "auto_step"), &AshfallWorld::set_auto_step);
    ClassDB::bind_method(D_METHOD("is_auto_step"), &AshfallWorld::is_auto_step);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_step"), "set_auto_step", "is_auto_step");
    ClassDB::bind_method(D_METHOD("set_sim_rate_hz", "hz"), &AshfallWorld::set_sim_rate_hz);
    ClassDB::bind_method(D_METHOD("get_sim_rate_hz"), &AshfallWorld::get_sim_rate_hz);
    ClassDB::bind_method(D_METHOD("set_max_steps_per_physics_frame", "max_steps"), &AshfallWorld::set_max_steps_per_physics_frame);
    ClassDB::bind_method(D_METHOD("get_max_steps_per_physics_frame"), &AshfallWorld::get_max_steps_per_physics_frame);

    ClassDB::bind_method(D_METHOD("update_world_texture"), &AshfallWorld::update_world_texture);
    ClassDB::bind_method(D_METHOD("flush_present"), &AshfallWorld::flush_present);
    ClassDB::bind_method(D_METHOD("debug_force_full_repaint"), &AshfallWorld::debug_force_full_repaint);
    ClassDB::bind_method(D_METHOD("debug_drop_presentation"), &AshfallWorld::debug_drop_presentation);
    ClassDB::bind_method(D_METHOD("get_present_sim_ms"), &AshfallWorld::get_present_sim_ms);
    ClassDB::bind_method(D_METHOD("get_present_dirty_collect_ms"), &AshfallWorld::get_present_dirty_collect_ms);
    ClassDB::bind_method(D_METHOD("get_present_material_read_ms"), &AshfallWorld::get_present_material_read_ms);
    ClassDB::bind_method(D_METHOD("get_present_paint_ms"), &AshfallWorld::get_present_paint_ms);
    ClassDB::bind_method(D_METHOD("get_present_set_data_ms"), &AshfallWorld::get_present_set_data_ms);
    ClassDB::bind_method(D_METHOD("get_present_submit_ms"), &AshfallWorld::get_present_submit_ms);
    ClassDB::bind_method(D_METHOD("get_present_texture_submits"), &AshfallWorld::get_present_texture_submits);
    ClassDB::bind_method(D_METHOD("get_present_flushes"), &AshfallWorld::get_present_flushes);
    ClassDB::bind_method(D_METHOD("get_present_dirty_pixels"), &AshfallWorld::get_present_dirty_pixels);
    ClassDB::bind_method(D_METHOD("get_present_logical_bytes"), &AshfallWorld::get_present_logical_bytes);
    ClassDB::bind_method(D_METHOD("get_world_texture"), &AshfallWorld::get_world_texture);
    ClassDB::bind_method(D_METHOD("get_world_image"), &AshfallWorld::get_world_image);
    ClassDB::bind_method(D_METHOD("get_water_texture"), &AshfallWorld::get_water_texture);
    ClassDB::bind_method(D_METHOD("get_water_image"), &AshfallWorld::get_water_image);
    ClassDB::bind_method(D_METHOD("get_last_step_ms"), &AshfallWorld::get_last_step_ms);
    ClassDB::bind_method(D_METHOD("get_world_width"), &AshfallWorld::get_world_width);
    ClassDB::bind_method(D_METHOD("get_world_height"), &AshfallWorld::get_world_height);

    BIND_ENUM_CONSTANT(MATERIAL_AIR);
    BIND_ENUM_CONSTANT(MATERIAL_STONE);
    BIND_ENUM_CONSTANT(MATERIAL_SAND);
    BIND_ENUM_CONSTANT(MATERIAL_WATER);
    BIND_ENUM_CONSTANT(MATERIAL_STEAM);
    BIND_ENUM_CONSTANT(MATERIAL_FIRE);
    BIND_ENUM_CONSTANT(MATERIAL_WOOD);
    BIND_ENUM_CONSTANT(MATERIAL_LAVA);
    BIND_ENUM_CONSTANT(MATERIAL_ROCK);
}

AshfallWorld::AshfallWorld() {
    init_world(512, 512);
    set_physics_process(true);
}

void AshfallWorld::_physics_process(double delta) {
    if (!auto_step_) {
        step_accumulator_ = 0.0;
        return;
    }
    if (sim_rate_hz_ <= 0.0) {
        step();
        return;
    }
    step_accumulator_ += delta;
    const double interval = 1.0 / sim_rate_hz_;
    // Drop backlog beyond one frame's worth of steps: never spiral.
    const double max_accum = interval * static_cast<double>(max_steps_per_physics_frame_ > 0 ? max_steps_per_physics_frame_ : 1);
    if (step_accumulator_ > max_accum) step_accumulator_ = max_accum;
    int steps = 0;
    while (step_accumulator_ >= interval && steps < max_steps_per_physics_frame_) {
        step();
        step_accumulator_ -= interval;
        ++steps;
    }
}

void AshfallWorld::init_world(int p_width, int p_height) {
    // Group 3, Task B5: checked dims + resource limits BEFORE any world
    // reset or allocation. Signed arithmetic in int64 so large inputs cannot
    // overflow; rejection leaves the current world untouched.
    if (p_width < 1 || p_height < 1 || p_width > kMaxWorldDim || p_height > kMaxWorldDim) {
        UtilityFunctions::push_error("init_world: dimensions out of range (1..4096 each)");
        return;
    }
    const std::uint64_t cells64 =
        static_cast<std::uint64_t>(p_width) * static_cast<std::uint64_t>(p_height);
    if (cells64 > kMaxWorldCells) {
        UtilityFunctions::push_error("init_world: world too large (cells exceed 1<<22)");
        return;
    }
    world_width_ = p_width;
    world_height_ = p_height;

    // Authoritative state reset: presentation window resize must not preserve
    // stale cells from a previous world. reset() dirties removed chunks, but
    // since we repaint the full background below, clear immediately to keep
    // presentation and dirty set in sync.
    world_.reset();
    refresh_material_cache();

    const std::size_t total_cells = static_cast<std::size_t>(world_width_) * static_cast<std::size_t>(world_height_);
    cell_buffer_.assign(total_cells, ashfall::kAir);

    fill_background_pixels();

    world_image_ = Image::create_from_data(world_width_, world_height_, false, Image::FORMAT_RGBA8, pixel_bytes_);
    world_texture_ = ImageTexture::create_from_image(world_image_);
    water_image_ = Image::create_from_data(world_width_, world_height_, false, Image::FORMAT_RGBA8, water_pixel_bytes_);
    water_texture_ = ImageTexture::create_from_image(water_image_);
    force_full_repaint_ = false;
    world_.clear_dirty_chunks();
    step_accumulator_ = 0.0;
}

void AshfallWorld::step() {
    // Group 3, Task B4 (cadence): step() advances the sim and accumulates
    // dirtiness ONLY. Presentation happens in flush_present(), called once
    // per rendered frame by the host (main_controller._process) — never once
    // per sim step, edit, or mouse-motion event. Tests needing a synchronous
    // step-and-present use flush_present() (or update_world_texture())
    // directly after stepping.
    // HUD number span: world_.step() ONLY — the same span bench_step.cpp
    // times (sim, no present), so the HUD ms/step is directly comparable to
    // sim-bench numbers. Present cost lives in last_present_ (see
    // flush_present()).
    const auto sim_t0 = std::chrono::steady_clock::now();
    world_.step();
    const auto sim_t1 = std::chrono::steady_clock::now();
    last_present_.sim_ms = present_ms_between(sim_t0, sim_t1);
    last_step_ms_ = last_present_.sim_ms;
}

bool AshfallWorld::validate_material_id(int p_material_id) const {
    // Group 3, Task B5: range + known-id reject BEFORE the u8 narrow, so 256
    // can never alias to air (and 257 to stone) ahead of engine validation.
    if (p_material_id < 0 || p_material_id > 255) {
        UtilityFunctions::push_error("AshfallWorld: material id out of range 0..255");
        return false;
    }
    ashfall::MaterialSpec spec;
    if (!world_.material_info(static_cast<ashfall::MaterialId>(p_material_id), spec)) {
        UtilityFunctions::push_error("AshfallWorld: unknown material id");
        return false;
    }
    return true;
}

void AshfallWorld::set_cell(int p_x, int p_y, int p_material_id) {
    if (!validate_material_id(p_material_id)) return;
    world_.set_cell({p_x, p_y}, static_cast<ashfall::MaterialId>(p_material_id));
}

int AshfallWorld::get_cell(int p_x, int p_y) const {
    return static_cast<int>(world_.cell({p_x, p_y}));
}

void AshfallWorld::fill_rect(int p_x1, int p_y1, int p_x2, int p_y2, int p_material_id) {
    if (!validate_material_id(p_material_id)) return;
    world_.fill_rect({p_x1, p_y1}, {p_x2, p_y2}, static_cast<ashfall::MaterialId>(p_material_id));
}

int64_t AshfallWorld::get_tick() const {
    return static_cast<int64_t>(world_.tick());
}

int64_t AshfallWorld::get_occupied_cell_count() const {
    return static_cast<int64_t>(world_.occupied_cell_count());
}

PackedVector2Array AshfallWorld::get_dirty_chunks() const {
    PackedVector2Array result;
    for (const ashfall::CellPosition chunk_position : world_.dirty_chunks()) {
        result.push_back(Vector2(chunk_position.x, chunk_position.y));
    }
    return result;
}

void AshfallWorld::clear_dirty_chunks() {
    world_.clear_dirty_chunks();
}

void AshfallWorld::set_auto_step(bool p_auto) {
    auto_step_ = p_auto;
    if (!auto_step_) step_accumulator_ = 0.0;
}

bool AshfallWorld::is_auto_step() const {
    return auto_step_;
}

void AshfallWorld::set_sim_rate_hz(double p_hz) {
    // Group 3, Task B5: non-finite rates are rejected (old rate kept);
    // finite rates clamp to the supported range.
    if (!std::isfinite(p_hz)) {
        UtilityFunctions::push_error("AshfallWorld: sim rate must be finite");
        return;
    }
    sim_rate_hz_ = std::clamp(p_hz, kMinSimRateHz, kMaxSimRateHz);
    step_accumulator_ = 0.0;
}

void AshfallWorld::set_max_steps_per_physics_frame(int p_max) {
    max_steps_per_physics_frame_ = p_max < 1 ? 1 : (p_max > 4 ? 4 : p_max);
}

void AshfallWorld::setup_reference_world() {
    world_.reset();
    // 512 by 512 world layout
    // Solid floor
    world_.fill_rect({0, 480}, {511, 511}, ashfall::kStone);

    // Lateral boundary walls
    world_.fill_rect({0, 80}, {16, 479}, ashfall::kStone);
    world_.fill_rect({494, 80}, {511, 479}, ashfall::kStone);

    // Funnel guiding down towards center
    world_.fill_rect({40, 260}, {210, 272}, ashfall::kStone);
    world_.fill_rect({300, 260}, {470, 272}, ashfall::kStone);
    world_.fill_rect({120, 340}, {240, 350}, ashfall::kStone);
    world_.fill_rect({270, 340}, {390, 350}, ashfall::kStone);

    // Initial powder and liquid deposits
    world_.fill_rect({80, 100}, {180, 200}, ashfall::kSand);
    world_.fill_rect({330, 100}, {430, 200}, ashfall::kWater);

    update_world_texture();
}

void AshfallWorld::setup_play_level() {
    world_.reset();
    world_.build_play_level();
    // Settle the authored rects so the first paint shows a lived-in world
    // (dunes at repose, pools flat) instead of raw rectangles. Fixed count
    // keeps setup deterministic; ~1s at 512 on M2 Max.
    for (int i = 0; i < 200; ++i) world_.step();
    update_world_texture();
}

bool AshfallWorld::load_materials(const String &p_json_text) {
    ashfall::MaterialTable table;
    std::string error;
    if (!ashfall::parse_material_data(p_json_text.utf8().get_data(), table, error)) {
        UtilityFunctions::push_error("load_materials: ", error.c_str());
        return false;
    }
    if (!world_.load_material_table(table)) {
        UtilityFunctions::push_error("load_materials: table failed validation");
        return false;
    }
    refresh_material_cache();
    update_world_texture();
    return true;
}

bool AshfallWorld::load_reactions(const String &p_json_text) {
    ashfall::ReactionTable table;
    std::string error;
    if (!ashfall::parse_reaction_data(p_json_text.utf8().get_data(), table, error)) {
        UtilityFunctions::push_error("load_reactions: ", error.c_str());
        return false;
    }
    if (!world_.load_reactions(table)) {
        UtilityFunctions::push_error("load_reactions: rules failed validation");
        return false;
    }
    return true;
}

void AshfallWorld::refresh_material_cache() {
    for (int i = 0; i < 256; ++i) {
        base_colors_[i] = {18, 20, 24};
        deep_colors_[i] = {18, 20, 24};
        movements_[i] = static_cast<std::uint8_t>(ashfall::Movement::Static);
        known_ids_[i] = false;
    }
    for (const ashfall::MaterialId id : world_.material_ids()) {
        ashfall::MaterialSpec spec;
        if (!world_.material_info(id, spec)) continue;
        base_colors_[id] = spec.color;
        // Deep shade reproduces the legacy water ramp exactly for water
        // (lround(86*.30)=26, lround(178*(88/178))=88, lround(214*.55)=118)
        // and gives every future liquid a matching gradient for free.
        deep_colors_[id] = {
            static_cast<std::uint8_t>(std::lround(static_cast<float>(spec.color[0]) * 0.30f)),
            static_cast<std::uint8_t>(std::lround(static_cast<float>(spec.color[1]) * (88.0f / 178.0f))),
            static_cast<std::uint8_t>(std::lround(static_cast<float>(spec.color[2]) * 0.55f)),
        };
        movements_[id] = static_cast<std::uint8_t>(spec.movement);
        known_ids_[id] = true;
    }
}

int AshfallWorld::get_material_count() const {
    return static_cast<int>(world_.material_ids().size());
}

PackedInt32Array AshfallWorld::get_material_ids() const {
    PackedInt32Array ids;
    for (const ashfall::MaterialId id : world_.material_ids()) {
        ids.push_back(static_cast<int>(id));
    }
    return ids;
}

String AshfallWorld::get_material_name(int p_material_id) const {
    if (p_material_id < 0 || p_material_id > 255) return String();
    ashfall::MaterialSpec spec;
    if (!world_.material_info(static_cast<ashfall::MaterialId>(p_material_id), spec)) return String();
    return String(spec.name.c_str());
}

Color AshfallWorld::get_material_color(int p_material_id) const {
    if (p_material_id < 0 || p_material_id > 255) return Color(0, 0, 0, 1);
    const auto& c = base_colors_[static_cast<std::size_t>(p_material_id)];
    return Color(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f);
}

int AshfallWorld::get_material_movement(int p_material_id) const {
    if (p_material_id < 0 || p_material_id > 255) return -1;
    ashfall::MaterialSpec spec;
    if (!world_.material_info(static_cast<ashfall::MaterialId>(p_material_id), spec)) return -1;
    return static_cast<int>(spec.movement);
}

Vector2i AshfallWorld::get_player_spawn() const {
    const ashfall::CellPosition spawn = ashfall::SimulationWorld::play_level_spawn();
    return Vector2i(spawn.x, spawn.y);
}

bool AshfallWorld::is_solid_cell(int p_x, int p_y) const {
    return world_.is_solid({p_x, p_y});
}

bool AshfallWorld::can_move_to_box(int p_x1, int p_y1, int p_x2, int p_y2) const {
    return world_.can_move_to({p_x1, p_y1}, {p_x2, p_y2});
}

PackedByteArray AshfallWorld::save_to_bytes() const {
    const std::vector<std::uint8_t> saved = world_.save();
    PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(saved.size()));
    if (!saved.empty()) {
        std::memcpy(bytes.ptrw(), saved.data(), saved.size());
    }
    return bytes;
}

bool AshfallWorld::load_from_bytes(const PackedByteArray &p_bytes) {
    if (p_bytes.is_empty()) return false;
    const std::span<const std::uint8_t> span(
        reinterpret_cast<const std::uint8_t *>(p_bytes.ptr()), static_cast<std::size_t>(p_bytes.size()));
    const bool ok = world_.load(span);
    if (ok) update_world_texture();
    return ok;
}

Ref<ImageTexture> AshfallWorld::update_world_texture() {
    // Compatibility wrapper: synchronous present for tests/tools.
    return flush_present();
}

void AshfallWorld::debug_force_full_repaint() {
    force_full_repaint_ = true;
}

void AshfallWorld::debug_drop_presentation() {
    // Test hook only: release presentation objects, sim state untouched.
    world_image_.unref();
    world_texture_.unref();
    water_image_.unref();
    water_texture_.unref();
}

std::vector<ashfall::CellPosition> AshfallWorld::visible_repaint_set(
    const std::vector<ashfall::CellPosition>& dirty) const {
    // B1 halo: each dirty chunk plus its vertical neighbors (±1 cy), clipped
    // to the presentation window. The window is the whole world here, so the
    // clip drops chunks that cannot paint (offscreen dirt must neither
    // repaint nor upload — B4 visible-set filtering).
    std::set<ashfall::CellPosition> repaint;
    for (const ashfall::CellPosition chunk : dirty) {
        for (int dy = -1; dy <= 1; ++dy) {
            const ashfall::CellPosition candidate{chunk.x, chunk.y + dy};
            const int base_x = candidate.x * ashfall::kChunkSize;
            const int base_y = candidate.y * ashfall::kChunkSize;
            if (base_x + ashfall::kChunkSize - 1 < 0 || base_x > world_width_ - 1) continue;
            if (base_y + ashfall::kChunkSize - 1 < 0 || base_y > world_height_ - 1) continue;
            repaint.insert(candidate);
        }
    }
    return std::vector<ashfall::CellPosition>(repaint.begin(), repaint.end());
}

bool AshfallWorld::presentation_resources_valid() const {
    // B2: the null/degraded check covers BOTH layers (base + water).
    if (world_image_.is_null() || world_texture_.is_null()) return false;
    if (water_image_.is_null() || water_texture_.is_null()) return false;
    if (world_image_->get_width() != world_width_ || world_image_->get_height() != world_height_) return false;
    if (water_image_->get_width() != world_width_ || water_image_->get_height() != world_height_) return false;
    const int64_t expect = static_cast<int64_t>(world_width_) * static_cast<int64_t>(world_height_) * 4;
    if (pixel_bytes_.size() != expect || water_pixel_bytes_.size() != expect) return false;
    return true;
}

void AshfallWorld::fill_background_pixels() {
    const std::size_t total_cells =
        static_cast<std::size_t>(world_width_) * static_cast<std::size_t>(world_height_);
    pixel_bytes_.resize(static_cast<int64_t>(total_cells * 4));
    uint8_t *w = pixel_bytes_.ptrw();
    for (std::size_t i = 0; i < total_cells; ++i) {
        w[i * 4 + 0] = 18;
        w[i * 4 + 1] = 20;
        w[i * 4 + 2] = 24;
        w[i * 4 + 3] = 255;
    }
    water_pixel_bytes_.resize(static_cast<int64_t>(total_cells * 4));
    uint8_t *ww = water_pixel_bytes_.ptrw();
    for (std::size_t i = 0; i < total_cells; ++i) {
        ww[i * 4 + 0] = 0;
        ww[i * 4 + 1] = 0;
        ww[i * 4 + 2] = 0;
        ww[i * 4 + 3] = 0;
    }
}

void AshfallWorld::recover_presentation_resources() {
    // B2: recreate presentation objects + full repaint from the CURRENT
    // world. Zero sim mutation: world_, its tick, cells, temps, and table are
    // never touched here (the old path called init_world(), erasing the sim).
    // The host rebinds its texture refs after every flush (see
    // main_controller._process), so replaced texture objects stay bound.
    const std::size_t total_cells =
        static_cast<std::size_t>(world_width_) * static_cast<std::size_t>(world_height_);
    cell_buffer_.assign(total_cells, ashfall::kAir);
    fill_background_pixels();
    world_image_ = Image::create_from_data(world_width_, world_height_, false, Image::FORMAT_RGBA8, pixel_bytes_);
    world_texture_ = ImageTexture::create_from_image(world_image_);
    water_image_ = Image::create_from_data(world_width_, world_height_, false, Image::FORMAT_RGBA8, water_pixel_bytes_);
    water_texture_ = ImageTexture::create_from_image(water_image_);
    force_full_repaint_ = true;
}

Ref<ImageTexture> AshfallWorld::flush_present() {
    if (!presentation_resources_valid()) {
        recover_presentation_resources();
    }

    const auto dirty_t0 = std::chrono::steady_clock::now();
    const std::vector<ashfall::CellPosition> dirty = world_.dirty_chunks();
    last_present_.dirty_collect_ms = present_ms_between(dirty_t0, std::chrono::steady_clock::now());

    // B1 halo + B4 visible-set filtering: the repaint set (dirty ∪ vertical
    // neighbors, window-clipped) is built BEFORE the partial/full decision,
    // so offscreen dirt neither repaints nor uploads.
    const std::vector<ashfall::CellPosition> repaint = visible_repaint_set(dirty);

    // No-work fast path BEFORE any ptrw(): PackedByteArray::ptrw() triggers
    // copy-on-write duplication even when the bytes are only read, so
    // acquiring the write pointer on a clean frame would copy ~1MB per layer
    // for nothing (CoW trap). Skipping here mutates nothing the old path did
    // not: the old code returned the same texture untouched when clean.
    // Invisible-only dirt is consumed the same way (fixed window: nothing
    // newly exposed can need it later) but submits nothing.
    if (repaint.empty()) {
        force_full_repaint_ = false;
        last_present_.material_read_ms = 0.0;
        last_present_.paint_ms = 0.0;
        last_present_.set_data_ms = 0.0;
        last_present_.submit_ms = 0.0;
        last_present_.dirty_pixels = 0;
        last_present_.logical_bytes = 0;
        world_.clear_dirty_chunks();
        return world_texture_;
    }

    // Deterministic per-pixel grain, stable across frames (no shimmer).
    auto grain = [](int x, int y) -> int {
        std::uint32_t h = static_cast<std::uint32_t>(x) * 0x9E3779B1u;
        h ^= static_cast<std::uint32_t>(y) * 0x85EBCA77u + 0x9E3779B9u + (h << 6) + (h >> 2);
        h ^= h >> 13;
        return static_cast<int>(h % 13u) - 6; // -6..+6
    };
    auto clamp_channel = [](int v) -> uint8_t {
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };

    // Colors come from the loaded table (refresh_material_cache); liquid
    // depth shading keys off the movement class, never the id. Powder gets
    // stronger grain than static. Paints BOTH layers: base gets opaque
    // known materials over background (air/unknown leave background), water
    // layer gets shaded RGB + coverage alpha for liquids, transparent else.
    static constexpr std::uint8_t kLiquidClass = static_cast<std::uint8_t>(ashfall::Movement::Liquid);
    static constexpr std::uint8_t kPowderClass = static_cast<std::uint8_t>(ashfall::Movement::Powder);
    auto paint_cell = [&](int x, int y, ashfall::MaterialId mat, int depth_below, bool surface,
                          uint8_t *dst, uint8_t *water_dst) {
        const int gr = grain(x, y);
        const std::size_t idx = static_cast<std::size_t>(mat);
        const bool is_liquid = movements_[idx] == kLiquidClass;
        int r = 18, g = 20, b = 24, a = 255;
        int wr = 0, wg = 0, wb = 0, wa = 0;
        if (is_liquid) {
            const float t = depth_below >= 5 ? 1.0f : static_cast<float>(depth_below) / 5.0f;
            const auto& base = base_colors_[idx];
            const auto& deep = deep_colors_[idx];
            wr = static_cast<int>(static_cast<float>(base[0]) +
                    (static_cast<float>(deep[0]) - static_cast<float>(base[0])) * t) +
                gr;
            wg = static_cast<int>(static_cast<float>(base[1]) +
                    (static_cast<float>(deep[1]) - static_cast<float>(base[1])) * t) +
                gr;
            wb = static_cast<int>(static_cast<float>(base[2]) +
                    (static_cast<float>(deep[2]) - static_cast<float>(base[2])) * t) +
                gr;
            if (surface) { // surface skin highlight
                wr += 22;
                wg += 22;
                wb += 22;
            }
            wa = 205;
        } else if (mat != ashfall::kAir && known_ids_[idx]) {
            const auto& base = base_colors_[idx];
            const int j = movements_[idx] == kPowderClass ? gr * 2 : gr;
            r = static_cast<int>(base[0]) + j;
            g = static_cast<int>(base[1]) + j;
            b = static_cast<int>(base[2]) + j;
        }
        dst[0] = clamp_channel(r);
        dst[1] = clamp_channel(g);
        dst[2] = clamp_channel(b);
        dst[3] = clamp_channel(a);
        water_dst[0] = clamp_channel(wr);
        water_dst[1] = clamp_channel(wg);
        water_dst[2] = clamp_channel(wb);
        water_dst[3] = clamp_channel(wa);
    };

    const auto paint_t0 = std::chrono::steady_clock::now();
    uint8_t *w = pixel_bytes_.ptrw();
    uint8_t *water_w = water_pixel_bytes_.ptrw();
    std::uint64_t painted_pixels = 0;
    std::uint64_t logical_cells = 0;

    // Partial CPU-side path: repaint only the visible repaint set (dirty
    // chunks + vertical halo, window-clipped). The single ImageTexture still
    // requires a full GPU upload via update(), so GPU-side remains monolithic
    // by design (see P1.2). The partial/full choice is made on the VISIBLE
    // set: offscreen dirt already returned early above.
    const std::size_t kFullRepaintThreshold = 8;
    bool use_partial = !force_full_repaint_ && repaint.size() <= kFullRepaintThreshold;

    if (!use_partial) {
        const auto read_t0 = std::chrono::steady_clock::now();
        world_.copy_rect({0, 0}, {world_width_ - 1, world_height_ - 1}, cell_buffer_);
        last_present_.material_read_ms = present_ms_between(read_t0, std::chrono::steady_clock::now());
        logical_cells = static_cast<std::uint64_t>(world_width_) * static_cast<std::uint64_t>(world_height_);
        for (int y = 0; y < world_height_; ++y) {
            for (int x = 0; x < world_width_; ++x) {
                const std::size_t i = static_cast<std::size_t>(y * world_width_ + x);
                const ashfall::MaterialId mat = cell_buffer_[i];
                const bool is_liquid =
                    movements_[static_cast<std::size_t>(mat)] == kLiquidClass;
                int depth = 0;
                bool surface = true;
                if (y > 0) {
                    const ashfall::MaterialId above_mat =
                        cell_buffer_[i - static_cast<std::size_t>(world_width_)];
                    surface = movements_[static_cast<std::size_t>(above_mat)] != kLiquidClass;
                }
                if (is_liquid) {
                    while (depth < 5 && y + 1 + depth < world_height_ &&
                           movements_[cell_buffer_[i + static_cast<std::size_t>((1 + depth) * world_width_)]] ==
                               kLiquidClass) {
                        ++depth;
                    }
                    int height = 0;
                    while (height < depth && y - 1 - height >= 0 &&
                           movements_[cell_buffer_[i - static_cast<std::size_t>((1 + height) * world_width_)]] ==
                               kLiquidClass) {
                        ++height;
                    }
                    depth = height;
                }
                paint_cell(x, y, mat, depth, surface, w + i * 4, water_w + i * 4);
                ++painted_pixels;
            }
        }
    } else {
        // Partial path reads cells inline via world_.cell() during the paint
        // loops below (no separate copy_rect), so there is no isolated
        // material-read phase here: it is timed within paint.
        last_present_.material_read_ms = 0.0;
        for (const ashfall::CellPosition chunk : repaint) {
            const int base_x = chunk.x * ashfall::kChunkSize;
            const int base_y = chunk.y * ashfall::kChunkSize;
            const int start_x = std::max(base_x, 0);
            const int start_y = std::max(base_y, 0);
            const int end_x = std::min(base_x + ashfall::kChunkSize - 1, world_width_ - 1);
            const int end_y = std::min(base_y + ashfall::kChunkSize - 1, world_height_ - 1);
            for (int y = start_y; y <= end_y; ++y) {
                for (int x = start_x; x <= end_x; ++x) {
                    const ashfall::MaterialId mat = world_.cell({x, y});
                    const bool is_liquid =
                        movements_[static_cast<std::size_t>(mat)] == kLiquidClass;
                    int depth = 0;
                    bool surface = true;
                    if (y > 0) {
                        surface = movements_[world_.cell({x, y - 1})] != kLiquidClass;
                    }
                    if (is_liquid) {
                        while (depth < 5 && y + 1 + depth < world_height_ &&
                               movements_[world_.cell({x, y + 1 + depth})] == kLiquidClass) {
                            ++depth;
                        }
                        int height = 0;
                        while (height < depth && y - 1 - height >= 0 &&
                               movements_[world_.cell({x, y - 1 - height})] == kLiquidClass) {
                            ++height;
                        }
                        depth = height;
                    }
                    const std::size_t offset = static_cast<std::size_t>(y * world_width_ + x) * 4;
                    paint_cell(x, y, mat, depth, surface, w + offset, water_w + offset);
                    ++painted_pixels;
                    ++logical_cells;
                }
            }
        }
    }
    last_present_.paint_ms = present_ms_between(paint_t0, std::chrono::steady_clock::now());
    last_present_.dirty_pixels = painted_pixels;
    last_present_.logical_bytes = logical_cells; // u8 ids: 1 byte per logical cell

    const auto set_data_t0 = std::chrono::steady_clock::now();
    world_image_->set_data(world_width_, world_height_, false, Image::FORMAT_RGBA8, pixel_bytes_);
    water_image_->set_data(world_width_, world_height_, false, Image::FORMAT_RGBA8, water_pixel_bytes_);
    last_present_.set_data_ms = present_ms_between(set_data_t0, std::chrono::steady_clock::now());
    const auto submit_t0 = std::chrono::steady_clock::now();
    world_texture_->update(world_image_);
    water_texture_->update(water_image_);
    last_present_.submit_ms = present_ms_between(submit_t0, std::chrono::steady_clock::now());
    last_present_.texture_submits += 2; // base + water update() calls
    ++last_present_.present_flushes;
    force_full_repaint_ = false;
    // Presentation consumed the dirty set.
    world_.clear_dirty_chunks();

    return world_texture_;
}

Ref<ImageTexture> AshfallWorld::get_world_texture() const {
    return world_texture_;
}

Ref<Image> AshfallWorld::get_world_image() const {
    return world_image_;
}

Ref<ImageTexture> AshfallWorld::get_water_texture() const {
    return water_texture_;
}

Ref<Image> AshfallWorld::get_water_image() const {
    return water_image_;
}

} // namespace godot
