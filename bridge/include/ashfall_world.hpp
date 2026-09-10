#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
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
        MATERIAL_STEAM = ashfall::kSteam,
        MATERIAL_FIRE = ashfall::kFire,
        MATERIAL_WOOD = ashfall::kWood,
        MATERIAL_LAVA = ashfall::kLava,
        MATERIAL_ROCK = ashfall::kRock,
    };

private:
    ashfall::SimulationWorld world_;
    Ref<Image> world_image_;
    Ref<ImageTexture> world_texture_;
    int world_width_ = 512;
    int world_height_ = 512;
    std::vector<ashfall::MaterialId> cell_buffer_;
    // Partial-path paint tile (Group 4, Task B7): per-chunk scratch (chunk
    // width x 64 + vertical halo), reused across flushes (grown-only).
    std::vector<ashfall::MaterialId> tile_scratch_;
    PackedByteArray pixel_bytes_;
    // Water-only layer: shaded RGB + coverage alpha, everything else
    // transparent. Sampled with linear filtering + a smoothing shader so the
    // liquid surface reads as rounded blobs instead of hard squares, while
    // solids stay nearest-crisp on the base layer.
    PackedByteArray water_pixel_bytes_;
    Ref<Image> water_image_;
    Ref<ImageTexture> water_texture_;
    // Renderer cache: base color, precomputed deep-shade color, movement
    // class, and known-flag per id, refreshed from the world table on
    // init/load. Unknown ids default to background/static so stale cells
    // can never paint garbage. Array-indexed (uint8 id) — no map lookups
    // on the per-pixel hot path.
    std::array<std::array<std::uint8_t, 3>, 256> base_colors_;
    std::array<std::array<std::uint8_t, 3>, 256> deep_colors_;
    std::array<std::uint8_t, 256> movements_;
    std::array<bool, 256> known_ids_;
    void refresh_material_cache();
    // Wall-clock cost of the last step() (sim ONLY — present cost lives in
    // last_present_; see flush_present()), for the perf HUD.
    double last_step_ms_ = 0.0;
    bool auto_step_ = false;
    // Fixed-rate simulation pacing: accumulate physics delta and step at
    // sim_rate_hz_ (default 60Hz), at most max_steps_per_physics_frame_ steps
    // per callback. Excess backlog is dropped to avoid spiral of death.
    double step_accumulator_ = 0.0;
    double sim_rate_hz_ = 60.0;
    int max_steps_per_physics_frame_ = 2;
    // Bridge/host resource limits (Group 3, Task B5 — mirror core caps).
    // Checked BEFORE any world reset or allocation.
    static constexpr int kMaxWorldDim = 4096;
    static constexpr std::uint64_t kMaxWorldCells = 1u << 22;
    static constexpr double kMinSimRateHz = 1.0;
    static constexpr double kMaxSimRateHz = 240.0;

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
    // Load versioned reaction rules (see data/reactions.json). Rules resolve
    // against the current material table; returns false (rules untouched) on
    // parse/validation failure. Loading a material table clears reactions,
    // so the host loads reactions after materials every boot.
    bool load_reactions(const String &p_json_text);
    // Presentation view of the loaded table (table order). Names/colors
    // come from data/materials.json; the renderer caches them per load.
    int get_material_count() const;
    PackedInt32Array get_material_ids() const;
    String get_material_name(int p_material_id) const;
    Color get_material_color(int p_material_id) const;
    // (int)ashfall::Movement, -1 for unknown ids.
    int get_material_movement(int p_material_id) const;

    void set_auto_step(bool p_auto);
    bool is_auto_step() const;
    void set_sim_rate_hz(double p_hz);
    double get_sim_rate_hz() const { return sim_rate_hz_; }
    void set_max_steps_per_physics_frame(int p_max);
    int get_max_steps_per_physics_frame() const { return max_steps_per_physics_frame_; }

    Ref<ImageTexture> update_world_texture();
    // Present-phase breakdown for the last flush (Group 1, Task C4; moved
    // into the class in Group 3, Task B). flush_present() is the documented
    // SOLE consumer of the engine dirty set: sim (step) and edits only
    // accumulate dirtiness; exactly one flush per rendered frame presents it.
    // update_world_texture() is a compatibility wrapper around flush_present()
    // for tests and tools that need a synchronous step-and-present.
    Ref<ImageTexture> flush_present();
    // Test hooks (presentation verifies only — never used by the host):
    // debug_force_full_repaint() forces the next flush onto the full-window
    // path (F1 partial==full compare); debug_drop_presentation() releases all
    // presentation objects without touching sim state (F5 recovery proof).
    void debug_force_full_repaint();
    void debug_drop_presentation();
    double get_present_sim_ms() const { return last_present_.sim_ms; }
    double get_present_dirty_collect_ms() const { return last_present_.dirty_collect_ms; }
    double get_present_material_read_ms() const { return last_present_.material_read_ms; }
    double get_present_paint_ms() const { return last_present_.paint_ms; }
    double get_present_set_data_ms() const { return last_present_.set_data_ms; }
    double get_present_submit_ms() const { return last_present_.submit_ms; }
    std::uint64_t get_present_texture_submits() const { return last_present_.texture_submits; }
    std::uint64_t get_present_flushes() const { return last_present_.present_flushes; }
    std::uint64_t get_present_dirty_pixels() const { return last_present_.dirty_pixels; }
    std::uint64_t get_present_logical_bytes() const { return last_present_.logical_bytes; }
    Ref<ImageTexture> get_world_texture() const;
    Ref<Image> get_world_image() const;
    Ref<ImageTexture> get_water_texture() const;
    Ref<Image> get_water_image() const;
    double get_last_step_ms() const { return last_step_ms_; }
    int get_world_width() const { return world_width_; }
    int get_world_height() const { return world_height_; }

private:
    // Present-phase breakdown (see flush_present()). Cumulative counters are
    // per-world (one world in practice).
    struct PresentPhases {
        double sim_ms = 0.0; // world_.step() span ONLY (see step() comment)
        double dirty_collect_ms = 0.0; // dirty_chunks() copy
        double material_read_ms = 0.0; // full-path copy_rect(); 0 on partial path (reads inline, timed in paint)
        double paint_ms = 0.0; // ptrw() acquire + per-pixel paint loops
        double set_data_ms = 0.0; // Image::set_data() x2 (CPU upload staging)
        double submit_ms = 0.0; // ImageTexture::update() x2 (GPU submit)
        std::uint64_t texture_submits = 0; // cumulative update() calls (2 per painted flush: base + water)
        std::uint64_t present_flushes = 0; // cumulative flush_present() calls that painted (updates-per-frame source)
        std::uint64_t dirty_pixels = 0; // pixels repainted in the last flush
        std::uint64_t logical_bytes = 0; // logical cells consumed in the last flush (u8 ids: 1 byte/cell)
    };
    PresentPhases last_present_;
    bool force_full_repaint_ = false;
    // Renderer invalidation halo (Group 3, Task B1): liquid shading samples
    // up to 5 cells vertically, so a dirty chunk can change the color of
    // unchanged cells in its vertical neighbors. Expand dirty chunks by ±1
    // cy, clipped to the presentation window. The sim stays unaware.
    [[nodiscard]] std::vector<ashfall::CellPosition> visible_repaint_set(
        const std::vector<ashfall::CellPosition>& dirty) const;
    [[nodiscard]] bool presentation_resources_valid() const;
    // Recreate presentation objects + full repaint from the CURRENT world.
    // Zero sim mutation (Group 3, Task B2): never resets or loads the world.
    void recover_presentation_resources();
    void fill_background_pixels();
    [[nodiscard]] bool validate_material_id(int p_material_id) const;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::AshfallWorld::Material);
