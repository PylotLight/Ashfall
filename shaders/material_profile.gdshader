shader_type canvas_item;

// P3A Slice B2 — shader-side profile evaluation from loaded material visuals.
//
// Contract (plan Slice B; bridge review: never recover material properties
// from grain-modified RGB):
// - The TextureRect's TEXTURE is the bridge identity layer (R8, one stable
//   material id per cell). CATEGORICAL: the sampling node chain must use
//   NEAREST filtering with mipmaps disabled, and the id below is decoded
//   with round(v*255) — never filtered, never color-managed (R8 has no sRGB
//   variant, so bytes survive sampling unmodified).
// - Appearance comes ONLY from the profile LUTs built by
//   scripts/material_profile_glue.gd out of data/material_visuals.json via
//   scripts/material_visuals.gd (keyed by stable material NAME, row = table
//   id). ZERO material-ID branches: the shader never compares an id against
//   a constant; every decision below branches on profile-supplied numbers
//   (pattern mode, palette count, opacity mode) or on coordinates/seed.
// - Liquid-coverage filtering stays on the SEPARATE linear water overlay
//   (shaders/water_fx.gdshader, center-sample-only — untouched). This shader
//   renders translucent-profile cells as background on the base layer; the
//   overlay carries their coverage.
// - CPU painter output stays the static/bootstrapping reference: opaque
//   profiles render palette color here, air/liquids render bg_color — the
//   same partition the bridge painter uses.
// - present_tick / present_phase / anim_mix are LIVE since Slice D1: the glue
//   feeds present_tick from the world tick each flush (present_phase is the
//   optional 0..1 interpolation fraction between sim ticks, held at 0.0) and
//   anim_mix selects static reference (0, bit-exact for E2 captures) vs live
//   animation (1). Section 7 branches on the profile anim index only.
//   emission_mix likewise (1 from C1 — intrinsic-only, no heat input; B2 held
//   0). No blanket clock: pause freezes deterministically by construction.
// - Crop note: under an AtlasTexture camera view the caller sets view_origin
//   (top-left cell) + view_size (region cells) so cell coords stay in sim
//   space; full-texture use is view_origin=(0,0), view_size=world_size.

// Stable per-cell variation seed (bridge R8, hash(x,y) — static for all time).
uniform sampler2D seed_tex : filter_nearest, repeat_disable;
// Profile LUTs from the glue: 4 wide x 256 tall (row = material id).
uniform sampler2D profile_palette : filter_nearest, repeat_disable; // RGBA8: 4 palette colors (unused slots repeat last)
uniform sampler2D profile_params : filter_nearest, repeat_disable; // RGBAF: tex0=(pattern_mode,pattern_scale,detail,edge) tex1=(opacity,opacity_mode,emission_strength,anim) tex2=(emission rgb,-) tex3=(palette_count,-,-,-)

// Sim-space mapping.
uniform vec2 world_size = vec2(512.0, 512.0);
uniform vec2 view_origin = vec2(0.0, 0.0);
uniform vec2 view_size = vec2(512.0, 512.0);
uniform vec3 bg_color : source_color = vec3(0.0706, 0.0784, 0.0941); // (18,20,24): CPU painter background

// Presentation clock (Slice D animates; B2 holds anim_mix = 0 ⇒ static).
uniform int present_tick = 0;
uniform float present_phase = 0.0;
uniform float anim_mix = 0.0;
// Emission enable (Slice C1 holds 1: intrinsic-only, no heat input).
uniform float emission_mix = 1.0;

// Deterministic 0..1 hash, no RNG state.
float hash12(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

void fragment() {
	// 1. Categorical identity decode. Nearest/no-mipmaps upstream ⇒ the
	// center texel holds exactly id/255; round() recovers the byte.
	float idf = floor(texture(TEXTURE, UV).r * 255.0 + 0.5);
	float prow = (clamp(idf, 0.0, 255.0) + 0.5) / 256.0;

	// 2. Profile fetch. Row selects the profile; nothing below names a material.
	float seed01 = texture(seed_tex, UV).r;
	vec4 pal0 = texture(profile_palette, vec2(0.125, prow));
	vec4 pal1 = texture(profile_palette, vec2(0.375, prow));
	vec4 pal2 = texture(profile_palette, vec2(0.625, prow));
	vec4 pal3 = texture(profile_palette, vec2(0.875, prow));
	vec4 P0 = texture(profile_params, vec2(0.125, prow));
	vec4 P1 = texture(profile_params, vec2(0.375, prow));
	vec4 P2 = texture(profile_params, vec2(0.625, prow));
	vec4 P3 = texture(profile_params, vec2(0.875, prow));
	float mode = P0.x;
	float pscale = max(P0.y, 1.0);
	float detail = clamp(P0.z, 0.0, 1.0);
	float count = max(floor(P3.x + 0.5), 1.0);
	vec2 cell = view_origin + floor(UV * view_size);
	float sstep = floor(seed01 * 255.0 + 0.5);

	// 3. Pattern index from profile mode + coords + seed (never from identity).
	float pidx = 0.0;
	if (mode < 0.5) {
		pidx = 0.0; // quiet: first palette entry + restrained detail
	} else if (mode < 1.5) {
		// strata: horizontal bands, seed-offset phase per column
		float band = floor((cell.y + mod(sstep, pscale)) / pscale);
		pidx = mod(band, count);
	} else if (mode < 2.5) {
		// grain: long horizontal bands with slow low-amplitude wobble (wood
		// direction) — wobble blocks are wide in x so bands run along x.
		float wob = (hash12(vec2(floor(cell.x / 4.0), floor(cell.y / (pscale * 2.0)))) - 0.5) * pscale * 0.5;
		float grow = floor((cell.y + wob) / max(pscale * 0.5, 1.0));
		pidx = mod(grow, count);
	} else if (mode < 3.5) {
		// clusters: block-hash pick over the palette (medium scale)
		pidx = floor(hash12(floor(cell / pscale)) * count);
	} else if (mode < 4.5) {
		// plume: vertical gradient with column wobble (static in B2)
		float t = fract((cell.y + (hash12(vec2(cell.x, 0.0)) - 0.5) * pscale) / max(pscale * 2.0, 1.0));
		pidx = floor(t * count);
	} else {
		// crust: dark-biased block hash with bright cracks
		float h = hash12(floor(cell / pscale) + 7.3);
		pidx = h < 0.7 ? 0.0 : (h < 0.9 ? 1.0 : count - 1.0);
	}
	vec3 base = pal0.rgb;
	float pi = floor(pidx + 0.5);
	if (pi > 0.5) {
		base = pal1.rgb;
	}
	if (pi > 1.5) {
		base = pal2.rgb;
	}
	if (pi > 2.5) {
		base = pal3.rgb;
	}

	// 4. Restrained per-cell detail (profile detail_strength; quiet stays quiet).
	vec3 col = base * (1.0 + (seed01 - 0.5) * 2.0 * detail);
	// P0.w edge_response decoded for Slice C boundary treatment (reserved in B2).

	// 5. Coverage partition from profile opacity (never from identity):
	// air (opacity ~0) and translucent liquids render background on the base
	// layer — liquids live on the linear overlay; opaque/additive render here.
	float opacity = P1.x;
	float opmode = P1.y;
	vec3 outc = col;
	if (opacity < 0.01) {
		outc = bg_color;
	} else if (opmode > 0.5 && opmode < 1.5) {
		outc = bg_color;
	}

	// 6. Intrinsic emission (Slice C1 enabled via emission_mix = 1).
	// Contract: lit = albedo x (ambient + local) + emission; emission =
	// intrinsic_material_emission + optional_heat_curve(T). There is NO
	// temperature input to this shader (no new uniforms: engine/bridge
	// edits are out of scope for C1), so heat_curve(T) is identically 0 BY
	// CONSTRUCTION — hot water (strength 0) cannot glow; flames/lava are
	// art-directed intrinsics, never a generic temperature ramp.
	// Pattern-aware weight from final albedo luminance (profile-driven,
	// never identity): flame plume cores emit ~full, edges dim; lava
	// crust (~0.19 lum) emits nothing, cracks (~0.78 lum) emit ~full.
	// Gated on rendered coverage so air/translucent base cells stay bg;
	// albedo itself is never darkened (ambient floor preserved).
	float elum = clamp(dot(clamp(col, vec3(0.0), vec3(1.0)), vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
	float eweight = clamp((elum - 0.25) / 0.55, 0.0, 1.0);
	eweight *= eweight;
	vec3 ecolor = mix(P2.rgb, vec3(1.0, 0.96, 0.88), eweight * 0.45);
	float ecover = 1.0;
	if (opacity < 0.01) {
		ecover = 0.0;
	} else if (opmode > 0.5 && opmode < 1.5) {
		ecover = 0.0;
	}
	outc += ecolor * (P1.z * eweight * ecover) * emission_mix;

	// 7. Tick-driven animation (Slice D1 live; anim_mix = 0 ⇒ static reference).
	// Inputs: present_tick (world tick, glue-fed each flush), present_phase
	// (0..1 interpolation fraction between sim ticks; glue holds 0.0), cell
	// coords, seed01 (static visual seed), P1.w anim profile index.
	// Determinism: NO TIME uniform anywhere; every time-varying term is a
	// low-frequency sinusoid (or slow rising wave) of (tick + phase) with a
	// STATIC spatial phase offset — freezing tick+phase freezes output
	// bit-exactly. No hash(x,y,tick): hashes below take static coords/region
	// only, so flicker stays regionally correlated, never white noise.
	// temp/fuel expose no uniforms (engine/bridge out of scope) so their
	// contribution is 0 by construction. Branches below test the
	// profile-supplied anim index only — never material identity. Gated by
	// rendered coverage (air/translucent base cells stay bg) and the anim_mix
	// master (E2 captures force 0 for static, 1 for a live frame).
	float at = float(present_tick) + clamp(present_phase, 0.0, 1.0);
	float av = P1.w;
	float amod = 0.0;
	if (av > 0.5 && av < 1.5) {
		// flame: upward-drifting bands + STRONG low-frequency regional
		// flicker + WEAK high-frequency per-cell shimmer (region >> cell).
		float rphase = hash12(floor(cell / 4.0)) * 6.2831;
		float region = sin(at * 0.9 + rphase);
		float cellf = sin(at * 2.7 + seed01 * 6.2831);
		float rise = sin((cell.y + at * 3.0) * 0.9 + (hash12(vec2(cell.x, 3.7)) - 0.5) * 4.0);
		amod = region * 0.10 + cellf * 0.03 + rise * 0.04;
	} else if (av > 1.5 && av < 2.5) {
		// ember: slow regional breathing at ~1/4 flame cadence, low amplitude.
		float rphase = hash12(floor(cell / 4.0)) * 6.2831;
		amod = sin(at * 0.25 + rphase) * 0.05;
	} else if (av > 2.5 && av < 3.5) {
		// flow: low-amplitude travelling ripple + sparse slow highlights;
		// bulk stays quiet (peak |amod| ~0.065, typical ~0.015). The sparkle
		// gate is a narrowed sinusoid with a STATIC block phase — no
		// temporal hash, so highlights breathe rather than strobe.
		float ripple = sin((cell.x + cell.y) * 0.35 + at * 0.5);
		float gate = 0.5 + 0.5 * sin(at * 0.5 + hash12(floor(cell / 3.0)) * 6.2831);
		amod = ripple * 0.015 + gate * gate * gate * gate * 0.05;
	} else if (av > 3.5) {
		// wisp: slow upward drift, weak.
		float drift = sin((cell.y + at * 1.0) * 0.5 + hash12(vec2(cell.x, 9.1)) * 6.2831);
		amod = drift * 0.03;
	}
	// static (av ~ 0) falls through with amod = 0: stone/sand/wood stable.
	float acover = 1.0;
	if (opacity < 0.01) {
		acover = 0.0;
	} else if (opmode > 0.5 && opmode < 1.5) {
		acover = 0.0;
	}
	outc *= 1.0 + amod * acover * anim_mix;

	COLOR = vec4(outc, 1.0);
}
