#pragma once

#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "ashfall/simulation.hpp"

namespace godot {

class AshfallWorld : public Node2D {
    GDCLASS(AshfallWorld, Node2D)

public:
    enum Material {
        MATERIAL_AIR = ashfall::kAir,
        MATERIAL_STONE = ashfall::kStone,
        MATERIAL_SAND = ashfall::kSand,
        MATERIAL_WATER = ashfall::kWater,
    };

private:
    ashfall::SimulationWorld world_;
    Ref<Image> world_image_;
    Ref<ImageTexture> world_texture_;
    int world_width_ = 256;
    int world_height_ = 256;
    std::vector<ashfall::MaterialId> cell_buffer_;
    PackedByteArray pixel_bytes_;
    bool auto_step_ = false;

protected:
    static void _bind_methods();

public:
    AshfallWorld();
    ~AshfallWorld() override = default;

    void _physics_process(double delta) override;

    void init_world(int p_width, int p_height);
    void step();
    void set_cell(int p_x, int p_y, int p_material_id);
    int get_cell(int p_x, int p_y) const;
    void fill_rect(int p_x1, int p_y1, int p_x2, int p_y2, int p_material_id);
    int64_t get_tick() const;
    int64_t get_occupied_cell_count() const;
    void setup_reference_world();

    void set_auto_step(bool p_auto);
    bool is_auto_step() const;

    Ref<ImageTexture> update_world_texture();
    Ref<ImageTexture> get_world_texture() const;
    int get_world_width() const { return world_width_; }
    int get_world_height() const { return world_height_; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::AshfallWorld::Material);
