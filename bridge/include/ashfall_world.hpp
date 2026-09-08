#pragma once

#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

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
    int world_width_ = 512;
    int world_height_ = 512;
    std::vector<ashfall::MaterialId> cell_buffer_;
    PackedByteArray pixel_bytes_;
    // Water-only layer: shaded RGB + coverage alpha, everything else
    // transparent. Sampled with linear filtering + a smoothing shader so the
    // liquid surface reads as rounded blobs instead of hard squares, while
    // solids stay nearest-crisp on the base layer.
    PackedByteArray water_pixel_bytes_;
    Ref<Image> water_image_;
    Ref<ImageTexture> water_texture_;
    // Wall-clock cost of the last step() (sim + present), for the perf HUD.
    double last_step_ms_ = 0.0;
    bool auto_step_ = false;
    // Fixed-rate simulation pacing: accumulate physics delta and step at
    // sim_rate_hz_ (default 30Hz), at most max_steps_per_physics_frame_ steps
    // per callback. Excess backlog is dropped to avoid spiral of death.
    double step_accumulator_ = 0.0;
    double sim_rate_hz_ = 60.0;
    int max_steps_per_physics_frame_ = 2;

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
    PackedVector2Array get_dirty_chunks() const;
    void clear_dirty_chunks();
    void setup_reference_world();
    void setup_play_level();
    Vector2i get_player_spawn() const;

    bool is_solid_cell(int p_x, int p_y) const;
    bool can_move_to_box(int p_x1, int p_y1, int p_x2, int p_y2) const;
    PackedByteArray save_to_bytes() const;
    bool load_from_bytes(const PackedByteArray &p_bytes);
    // Load a versioned material table (see data/materials.json). Resets the
    // world; returns false (world untouched) on parse/validation failure.
    bool load_materials(const String &p_json_text);

    void set_auto_step(bool p_auto);
    bool is_auto_step() const;
    void set_sim_rate_hz(double p_hz);
    double get_sim_rate_hz() const { return sim_rate_hz_; }
    void set_max_steps_per_physics_frame(int p_max);
    int get_max_steps_per_physics_frame() const { return max_steps_per_physics_frame_; }

    Ref<ImageTexture> update_world_texture();
    Ref<ImageTexture> get_world_texture() const;
    Ref<Image> get_world_image() const;
    Ref<ImageTexture> get_water_texture() const;
    Ref<Image> get_water_image() const;
    double get_last_step_ms() const { return last_step_ms_; }
    int get_world_width() const { return world_width_; }
    int get_world_height() const { return world_height_; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::AshfallWorld::Material);
