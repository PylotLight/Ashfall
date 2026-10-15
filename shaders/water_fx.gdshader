shader_type canvas_item;

// Liquid-surface presentation for the water-only layer (Group 3, Task B3).
//
// Group-2 dilation REMOVED. The old shader took a 4-neighborhood max() over
// coverage and re-thresholded it with smoothstep: at an air texel directly
// beside liquid the neighbor alpha (~0.8) pushed smoothstep(0.25, 0.65, a)
// to 1, expanding liquid by roughly a whole texel while the color sample
// stayed center-only (transparent black) — a dark halo that could also paint
// over adjacent solids (bridge review C1-C3).
//
// Current contract: straight alpha, center sample only. Coverage and color
// both come from the texel under the fragment, so solid-adjacent texels
// render exactly what the CPU layer holds (proven by the F6 pixel fixtures
// in tests/godot_presentation_verify.gd). Residual half-texel linear-filter
// bleed at water/air boundaries is ACCEPTED (ordinary texture filtering, not
// shader expansion).
//
// Design notes:
// - Layer texels carry shaded RGB + coverage alpha for liquids, transparent
//   black elsewhere; interior gets a faint animated shimmer, amplitude tiny
//   to avoid boiling at 60 Hz.
// - A +/-1 LSB hash dither kills banding in the depth ramp without
//   frame-to-frame shimmer (static in screen space).
// - edge_low / edge_high / foam_tint are RETAINED (unused) for scene
//   compat: main.tscn sets them as shader parameters. Reintroducing any
//   coverage remap must also propagate a matching liquid color and a solid
//   exclusion test (review C2-C3), not just center RGB over new coverage.

// Retained for main.tscn compat (unused since dilation removal).
uniform float edge_low : hint_range(0.0, 1.0) = 0.25;
// Retained for main.tscn compat (unused since dilation removal).
uniform float edge_high : hint_range(0.0, 1.0) = 0.65;
uniform float final_alpha : hint_range(0.0, 1.0) = 0.88;
// Retained for main.tscn compat (unused since dilation removal).
uniform vec3 foam_tint : source_color = vec3(0.09, 0.11, 0.12);
uniform float shimmer_strength = 0.02;

void fragment() {
	vec4 c = texture(TEXTURE, UV);
	float shim = sin(UV.x * 900.0 + TIME * 1.6) * sin(UV.y * 760.0 - TIME * 1.3);
	vec3 rgb = c.rgb + shim * shimmer_strength;
	float hash = fract(sin(dot(FRAGCOORD.xy, vec2(12.9898, 78.233))) * 43758.5453);
	rgb += (hash - 0.5) * (2.0 / 255.0);
	COLOR = vec4(rgb, c.a * final_alpha);
}
