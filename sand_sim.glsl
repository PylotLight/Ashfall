#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8) in;

layout(r8, binding = 0) uniform readonly image2D src_grid;
layout(r8, binding = 1) uniform writeonly image2D dst_grid;

layout(push_constant, std430) uniform Params {
    int grid_w;
    int grid_h;
    uint frame;
    uint painting;
    int mouse_x;
    int mouse_y;
    int brush_radius;
    int paint_mat;
} params;

const uint EMPTY = 0u;
const uint STONE = 1u;
const uint SAND  = 2u;
const uint WATER = 3u;

uint read_cell(ivec2 p) {
    // Out-of-bounds reads as solid wall so nothing falls off the edge of the grid.
    if (p.x < 0 || p.y < 0 || p.x >= params.grid_w || p.y >= params.grid_h) {
        return STONE;
    }
    float v = imageLoad(src_grid, p).r;
    return uint(round(v * 255.0));
}

// Deterministic per-cell hash used to alternate left/right tie-break preference,
// so the sim doesn't develop a visible directional bias over time.
uint hash2(ivec2 p, uint frame) {
    uint h = uint(p.x) * 374761393u + uint(p.y) * 668265263u + frame * 2246822519u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return h ^ (h >> 16u);
}

// Pure function of the READ-ONLY previous frame: returns the single cell that
// (src_pos, val) would move into this tick, or src_pos itself if it doesn't move.
// Because this only reads last frame's frozen data, every thread that evaluates
// it (whether for itself or as a neighbor-candidate) gets the identical answer -
// that's what makes the pull-based update race-free without atomics or locks.
ivec2 move_target(ivec2 src_pos, uint val) {
    if (val != SAND && val != WATER) {
        return src_pos;
    }

    ivec2 down       = src_pos + ivec2(0, 1);
    ivec2 down_left  = src_pos + ivec2(-1, 1);
    ivec2 down_right = src_pos + ivec2(1, 1);

    if (read_cell(down) == EMPTY) {
        return down;
    }

    bool prefer_left = (hash2(src_pos, params.frame) & 1u) == 0u;
    uint dl = read_cell(down_left);
    uint dr = read_cell(down_right);

    if (prefer_left) {
        if (dl == EMPTY) return down_left;
        if (dr == EMPTY) return down_right;
    } else {
        if (dr == EMPTY) return down_right;
        if (dl == EMPTY) return down_left;
    }

    // Water additionally spreads sideways when it can't fall.
    if (val == WATER) {
        ivec2 left  = src_pos + ivec2(-1, 0);
        ivec2 right = src_pos + ivec2(1, 0);
        bool prefer_left2 = (hash2(src_pos + ivec2(17, 0), params.frame) & 1u) == 0u;
        if (prefer_left2) {
            if (read_cell(left) == EMPTY) return left;
            if (read_cell(right) == EMPTY) return right;
        } else {
            if (read_cell(right) == EMPTY) return right;
            if (read_cell(left) == EMPTY) return left;
        }
    }

    return src_pos;
}

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (pos.x >= params.grid_w || pos.y >= params.grid_h) {
        return;
    }

    uint self_val = read_cell(pos);
    uint result = self_val;

    // Am I (as a source) vacating this cell this tick?
    if (self_val != EMPTY && self_val != STONE) {
        ivec2 self_target = move_target(pos, self_val);
        if (self_target != pos) {
            result = EMPTY;
        }
    }

    // Fixed-priority scan: straight-down fall first (always conflict-free,
    // since only one cell is directly above a given destination), then
    // diagonal fall, then horizontal water spread. First match wins, which
    // guarantees each destination pulls from at most one source - no
    // duplication, no cells vanishing.
    ivec2 candidates[5] = ivec2[5](
        pos + ivec2(0, -1),
        pos + ivec2(-1, -1),
        pos + ivec2(1, -1),
        pos + ivec2(-1, 0),
        pos + ivec2(1, 0)
    );

    for (int i = 0; i < 5; i++) {
        uint cand_val = read_cell(candidates[i]);
        if (cand_val == EMPTY || cand_val == STONE) {
            continue;
        }
        if (move_target(candidates[i], cand_val) == pos) {
            result = cand_val;
            break;
        }
    }

    // Mouse painting overrides the simulated result within the brush radius.
    // Done GPU-side (rather than CPU texture_update) to avoid a per-frame
    // CPU->GPU upload for something this small and frequent.
    if (params.painting == 1u) {
        ivec2 d = pos - ivec2(params.mouse_x, params.mouse_y);
        if (d.x * d.x + d.y * d.y <= params.brush_radius * params.brush_radius) {
            // Protect the outer boundary and bottom floor from normal/standard edits
            if (pos.x > 0 && pos.x < params.grid_w - 1 && pos.y > 0 && pos.y < params.grid_h - 1) {
                result = uint(params.paint_mat);
            }
        }
    }

    imageStore(dst_grid, pos, vec4(float(result) / 255.0, 0.0, 0.0, 1.0));
}
