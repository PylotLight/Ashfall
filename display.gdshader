shader_type canvas_item;

void fragment() {
    float v = texture(TEXTURE, UV).r;
    int mat = int(round(v * 255.0));

    vec4 col;
    if (mat == 0) {
        col = vec4(0.05, 0.05, 0.08, 1.0);   // empty / background
    } else if (mat == 1) {
        col = vec4(0.40, 0.40, 0.42, 1.0);   // stone
    } else if (mat == 2) {
        col = vec4(0.85, 0.70, 0.35, 1.0);   // sand
    } else if (mat == 3) {
        col = vec4(0.20, 0.45, 0.85, 0.85);  // water
    } else {
        col = vec4(1.0, 0.0, 1.0, 1.0);      // debug magenta: unexpected id, means a bug
    }
    COLOR = col;
}
