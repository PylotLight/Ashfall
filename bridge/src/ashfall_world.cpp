#include "ashfall_world.hpp"

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
    ClassDB::bind_method(D_METHOD("setup_reference_world"), &AshfallWorld::setup_reference_world);

    ClassDB::bind_method(D_METHOD("set_auto_step", "auto_step"), &AshfallWorld::set_auto_step);
    ClassDB::bind_method(D_METHOD("is_auto_step"), &AshfallWorld::is_auto_step);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_step"), "set_auto_step", "is_auto_step");

    ClassDB::bind_method(D_METHOD("update_world_texture"), &AshfallWorld::update_world_texture);
    ClassDB::bind_method(D_METHOD("get_world_texture"), &AshfallWorld::get_world_texture);
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

void AshfallWorld::_physics_process(double /*delta*/) {
    if (auto_step_) {
        step();
    }
}

void AshfallWorld::init_world(int p_width, int p_height) {
    world_width_ = p_width > 0 ? p_width : 256;
    world_height_ = p_height > 0 ? p_height : 256;

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

void AshfallWorld::set_auto_step(bool p_auto) {
    auto_step_ = p_auto;
}

bool AshfallWorld::is_auto_step() const {
    return auto_step_;
}

void AshfallWorld::setup_reference_world() {
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

    world_.copy_rect({0, 0}, {world_width_ - 1, world_height_ - 1}, cell_buffer_);

    uint8_t *w = pixel_bytes_.ptrw();
    const std::size_t total_cells = static_cast<std::size_t>(world_width_ * world_height_);

    for (std::size_t i = 0; i < total_cells; ++i) {
        const ashfall::MaterialId mat = cell_buffer_[i];
        uint8_t r = 18;
        uint8_t g = 20;
        uint8_t b = 24;
        uint8_t a = 255;

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

        const std::size_t offset = i * 4;
        w[offset + 0] = r;
        w[offset + 1] = g;
        w[offset + 2] = b;
        w[offset + 3] = a;
    }

    world_image_->set_data(world_width_, world_height_, false, Image::FORMAT_RGBA8, pixel_bytes_);
    world_texture_->update(world_image_);

    return world_texture_;
}

Ref<ImageTexture> AshfallWorld::get_world_texture() const {
    return world_texture_;
}

} // namespace godot
