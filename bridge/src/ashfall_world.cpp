#include "ashfall_world.hpp"

#include <algorithm>
#include <vector>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

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

    ClassDB::bind_method(D_METHOD("set_auto_step", "auto_step"), &AshfallWorld::set_auto_step);
    ClassDB::bind_method(D_METHOD("is_auto_step"), &AshfallWorld::is_auto_step);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_step"), "set_auto_step", "is_auto_step");
    ClassDB::bind_method(D_METHOD("set_sim_rate_hz", "hz"), &AshfallWorld::set_sim_rate_hz);
    ClassDB::bind_method(D_METHOD("get_sim_rate_hz"), &AshfallWorld::get_sim_rate_hz);
    ClassDB::bind_method(D_METHOD("set_max_steps_per_physics_frame", "max_steps"), &AshfallWorld::set_max_steps_per_physics_frame);
    ClassDB::bind_method(D_METHOD("get_max_steps_per_physics_frame"), &AshfallWorld::get_max_steps_per_physics_frame);

    ClassDB::bind_method(D_METHOD("update_world_texture"), &AshfallWorld::update_world_texture);
    ClassDB::bind_method(D_METHOD("get_world_texture"), &AshfallWorld::get_world_texture);
    ClassDB::bind_method(D_METHOD("get_world_image"), &AshfallWorld::get_world_image);
    ClassDB::bind_method(D_METHOD("get_world_width"), &AshfallWorld::get_world_width);
    ClassDB::bind_method(D_METHOD("get_world_height"), &AshfallWorld::get_world_height);

    BIND_ENUM_CONSTANT(MATERIAL_AIR);
    BIND_ENUM_CONSTANT(MATERIAL_STONE);
    BIND_ENUM_CONSTANT(MATERIAL_SAND);
    BIND_ENUM_CONSTANT(MATERIAL_WATER);
}

AshfallWorld::AshfallWorld() {
    init_world(256, 256);
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
    world_width_ = p_width > 0 ? p_width : 256;
    world_height_ = p_height > 0 ? p_height : 256;

    // Authoritative state reset: presentation window resize must not preserve
    // stale cells from a previous world. reset() dirties removed chunks, but
    // since we repaint the full background below, clear immediately to keep
    // presentation and dirty set in sync.
    world_.reset();

    const std::size_t total_cells = static_cast<std::size_t>(world_width_ * world_height_);
    cell_buffer_.resize(total_cells, ashfall::kAir);

    pixel_bytes_.resize(static_cast<int64_t>(total_cells * 4));
    uint8_t *w = pixel_bytes_.ptrw();
    for (std::size_t i = 0; i < total_cells; ++i) {
        w[i * 4 + 0] = 18;
        w[i * 4 + 1] = 20;
        w[i * 4 + 2] = 24;
        w[i * 4 + 3] = 255;
    }

    world_image_ = Image::create_from_data(world_width_, world_height_, false, Image::FORMAT_RGBA8, pixel_bytes_);
    world_texture_ = ImageTexture::create_from_image(world_image_);
    world_.clear_dirty_chunks();
    step_accumulator_ = 0.0;
}

void AshfallWorld::step() {
    world_.step();
    update_world_texture();
}

void AshfallWorld::set_cell(int p_x, int p_y, int p_material_id) {
    world_.set_cell({p_x, p_y}, static_cast<ashfall::MaterialId>(p_material_id));
}

int AshfallWorld::get_cell(int p_x, int p_y) const {
    return static_cast<int>(world_.cell({p_x, p_y}));
}

void AshfallWorld::fill_rect(int p_x1, int p_y1, int p_x2, int p_y2, int p_material_id) {
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
    sim_rate_hz_ = p_hz;
    step_accumulator_ = 0.0;
}

void AshfallWorld::set_max_steps_per_physics_frame(int p_max) {
    max_steps_per_physics_frame_ = p_max < 1 ? 1 : (p_max > 4 ? 4 : p_max);
}

void AshfallWorld::setup_reference_world() {
    world_.reset();
    // 256 by 256 world layout
    // Solid floor
    world_.fill_rect({0, 240}, {255, 255}, ashfall::kStone);

    // Lateral boundary walls
    world_.fill_rect({0, 40}, {8, 239}, ashfall::kStone);
    world_.fill_rect({247, 40}, {255, 239}, ashfall::kStone);

    // Funnel guiding down towards center
    world_.fill_rect({20, 130}, {105, 136}, ashfall::kStone);
    world_.fill_rect({150, 130}, {235, 136}, ashfall::kStone);
    world_.fill_rect({60, 170}, {120, 175}, ashfall::kStone);
    world_.fill_rect({135, 170}, {195, 175}, ashfall::kStone);

    // Initial powder and liquid deposits
    world_.fill_rect({40, 50}, {90, 100}, ashfall::kSand);
    world_.fill_rect({165, 50}, {215, 100}, ashfall::kWater);

    update_world_texture();
}

Ref<ImageTexture> AshfallWorld::update_world_texture() {
    if (world_image_.is_null() || world_texture_.is_null()) {
        init_world(world_width_, world_height_);
    }

    const std::vector<ashfall::CellPosition> dirty = world_.dirty_chunks();
    const std::size_t total_cells = static_cast<std::size_t>(world_width_ * world_height_);

    auto paint_material = [](ashfall::MaterialId mat, uint8_t &r, uint8_t &g, uint8_t &b) {
        r = 18;
        g = 20;
        b = 24;
        switch (mat) {
            case ashfall::kStone:
                r = 128;
                g = 133;
                b = 138;
                break;
            case ashfall::kSand:
                r = 230;
                g = 175;
                b = 50;
                break;
            case ashfall::kWater:
                r = 52;
                g = 152;
                b = 219;
                break;
            case ashfall::kAir:
            default:
                break;
        }
    };

    uint8_t *w = pixel_bytes_.ptrw();

    // Partial CPU-side path: repaint only dirty 64x64 chunks intersecting the
    // presentation window. The single ImageTexture still requires a full GPU
    // upload via update(), so GPU-side remains monolithic by design (see P1.2).
    const std::size_t kFullRepaintThreshold = 8;
    bool use_partial = !dirty.empty() && dirty.size() <= kFullRepaintThreshold;
    if (dirty.empty()) {
        return world_texture_;
    }

    if (!use_partial) {
        world_.copy_rect({0, 0}, {world_width_ - 1, world_height_ - 1}, cell_buffer_);
        for (std::size_t i = 0; i < total_cells; ++i) {
            uint8_t r, g, b;
            paint_material(cell_buffer_[i], r, g, b);
            const std::size_t offset = i * 4;
            w[offset + 0] = r;
            w[offset + 1] = g;
            w[offset + 2] = b;
            w[offset + 3] = 255;
        }
    } else {
        for (const ashfall::CellPosition chunk : dirty) {
            const int base_x = chunk.x * ashfall::kChunkSize;
            const int base_y = chunk.y * ashfall::kChunkSize;
            const int start_x = std::max(base_x, 0);
            const int start_y = std::max(base_y, 0);
            const int end_x = std::min(base_x + ashfall::kChunkSize - 1, world_width_ - 1);
            const int end_y = std::min(base_y + ashfall::kChunkSize - 1, world_height_ - 1);
            for (int y = start_y; y <= end_y; ++y) {
                for (int x = start_x; x <= end_x; ++x) {
                    const ashfall::MaterialId mat = world_.cell({x, y});
                    uint8_t r, g, b;
                    paint_material(mat, r, g, b);
                    const std::size_t offset = static_cast<std::size_t>(y * world_width_ + x) * 4;
                    w[offset + 0] = r;
                    w[offset + 1] = g;
                    w[offset + 2] = b;
                    w[offset + 3] = 255;
                }
            }
        }
    }

    world_image_->set_data(world_width_, world_height_, false, Image::FORMAT_RGBA8, pixel_bytes_);
    world_texture_->update(world_image_);
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

} // namespace godot
