shader_type canvas_item;

// A distant, low-contrast cave wall/atmosphere behind transparent air cells.
// Coordinates follow the cropped world view, so the pattern is stable while
// the camera pans and zooms. No TIME-based animation or material-ID sampling.
uniform vec2 view_origin = vec2(0.0, 0.0);
uniform vec2 view_size = vec2(192.0, 120.0);
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
	float cloud = value_noise(world_cell / 58.0);
	float wall = value_noise((world_cell + vec2(21.7, 8.3)) / 24.0);
	float grain = value_noise((world_cell + vec2(4.1, 37.9)) / 9.0);
	float cloud_tint = mix(0.72, 1.28, smoothstep(0.15, 0.85, cloud));
	float wall_tint = mix(0.88, 1.14, wall);
	float grain_tint = mix(0.96, 1.04, grain);
	vec3 color = base_tint * cloud_tint * wall_tint * grain_tint;
	// The broad, soft cool/plum shifts make the negative space read as distant
	// stone rather than a uniform black fill, while staying darker than rock.
	COLOR = vec4(color, 1.0);
}
