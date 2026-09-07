shader_type canvas_item;

// Liquid-surface smoothing for the water-only layer.
//
// The layer is sampled with LINEAR filtering, so texel centers carry full
// coverage (alpha ~0.8) while water/air boundaries form gradients. Remapping
// that gradient through smoothstep rounds every corner: isolated droplets
// become soft beads, surfaces lose their single-pixel staircase, and solids
// underneath stay nearest-crisp because they live on the base layer.
//
// Design notes:
// - Edge band (mid-coverage) gets a foam lift so shorelines read bright.
// - Interior gets a faint animated shimmer; amplitude stays tiny to avoid
//   boiling at 60 Hz.
// - A +/-1 LSB hash dither kills banding in the depth ramp without
//   frame-to-frame shimmer (static in screen space).

uniform float edge_low : hint_range(0.0, 1.0) = 0.25;
uniform float edge_high : hint_range(0.0, 1.0) = 0.65;
uniform float final_alpha : hint_range(0.0, 1.0) = 0.88;
uniform vec3 foam_tint : source_color = vec3(0.09, 0.11, 0.12);
uniform float shimmer_strength = 0.02;

void fragment() {
	// Morphological closing on coverage: dilate the alpha mask with the
	// 4-neighborhood so 1-cell notches and staircase grooves fill in, then
	// the smoothstep below re-thresholds. Net effect is rounded liquid
	// blobs with ~half-texel meniscus instead of hard squares.
	vec2 px = 1.0 / vec2(textureSize(TEXTURE, 0));
	float a = texture(TEXTURE, UV).a;
	a = max(a, texture(TEXTURE, UV + vec2(px.x, 0.0)).a);
	a = max(a, texture(TEXTURE, UV - vec2(px.x, 0.0)).a);
	a = max(a, texture(TEXTURE, UV + vec2(0.0, px.y)).a);
	a = max(a, texture(TEXTURE, UV - vec2(0.0, px.y)).a);
	vec4 c = texture(TEXTURE, UV);
	float cov = smoothstep(edge_low, edge_high, a);
	float edge = cov * (1.0 - cov) * 4.0;
	float shim = sin(UV.x * 900.0 + TIME * 1.6) * sin(UV.y * 760.0 - TIME * 1.3);
	vec3 rgb = c.rgb + edge * foam_tint + (1.0 - edge) * shim * shimmer_strength;
	float hash = fract(sin(dot(FRAGCOORD.xy, vec2(12.9898, 78.233))) * 43758.5453);
	rgb += (hash - 0.5) * (2.0 / 255.0);
	COLOR = vec4(rgb, cov * final_alpha);
}
