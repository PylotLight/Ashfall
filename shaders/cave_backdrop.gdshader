shader_type canvas_item;

// A distant, low-contrast cave wall/atmosphere behind transparent air cells.
// Coordinates follow the cropped world view, so the pattern is stable while
// the camera pans and zooms. No TIME-based animation or material-ID sampling.
uniform vec2 view_origin = vec2(0.0, 0.0);
uniform vec2 view_size = vec2(192.0, 120.0);
uniform float surface_level = 80.0;
uniform vec3 base_tint : source_color = vec3(0.095, 0.105, 0.145);

float hash12(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float value_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = hash12(i);
	float b = hash12(i + vec2(1.0, 0.0));
	float c = hash12(i + vec2(0.0, 1.0));
	float d = hash12(i + vec2(1.0, 1.0));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void fragment() {
	vec2 world_cell = view_origin + UV * view_size;
	// Structured distant wall (R9): horizontal strata bands carry the large
	// masses, a ridged crack field cuts them, fine grain stays restrained.
	// World-locked so panning never swims the geology.
	float strata = value_noise(vec2(world_cell.x / 90.0, world_cell.y / 26.0));
	float strata_band = smoothstep(0.35, 0.65, strata);
	float crack_n = value_noise((world_cell + vec2(21.7, 8.3)) / 17.0);
	float crack = 1.0 - abs(crack_n * 2.0 - 1.0);
	crack = smoothstep(0.92, 1.0, crack);
	float grain = value_noise((world_cell + vec2(4.1, 37.9)) / 9.0);
	float wall_tint = mix(0.80, 1.12, strata_band);
	wall_tint *= mix(1.0, 0.62, crack);
	wall_tint *= mix(0.96, 1.04, grain);
	// Dark mortar seams every ~14 cells break the fog into courses.
	float course = smoothstep(0.0, 1.2, abs(fract(world_cell.y / 14.0) - 0.5) * 14.0);
	wall_tint *= mix(0.72, 1.0, course);
	vec3 cave_color = base_tint * wall_tint;
	// Above the authored soil line, the same world-locked field becomes a dim
	// blue-violet evening sky. A soft horizon avoids a hard backdrop seam.
	float sky_y = clamp(world_cell.y / max(surface_level, 1.0), 0.0, 1.0);
	vec3 sky_color = mix(vec3(0.075, 0.105, 0.17), vec3(0.27, 0.17, 0.19), sky_y);
	float sky_mask = 1.0 - smoothstep(surface_level - 4.0, surface_level + 3.0, world_cell.y);
	vec3 color = mix(cave_color, sky_color, sky_mask);
	COLOR = vec4(color, 1.0);
}
