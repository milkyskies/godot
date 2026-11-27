#[compute]

#version 450

#VERSION_DEFINES

// Frame generation's picture without its overlay (hacco #1483): an overlay (a first-person view model) is 3D drawn
// nearer than a depth no world geometry reaches. It is kept out of the interpolated picture as the 2D is: here its
// pixels take the picture around them, so no sample of a generated frame finds it away from where it is, and the SDK
// puts it back from the real frame wherever that differs from this one.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D color_buffer; // The real frame before its 2D, at the display size.
layout(set = 0, binding = 1) uniform sampler2D depth_buffer; // The scene's, at the render size (reverse Z).
#ifdef FLOAT_COLOR
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D output_buffer;
#else
layout(rgba8, set = 0, binding = 2) uniform restrict writeonly image2D output_buffer;
#endif

layout(push_constant, std430) uniform Params {
	ivec2 display_size;
	ivec2 render_size;
	float overlay_depth; // This near or nearer is the overlay.
	uint pad[3];
}
params;

// How far past the overlay's edge a fill is read, in render pixels: past the pixels the edge was blended into.
const float EDGE_MARGIN = 2.0;

bool covered(vec2 render_pos) {
	ivec2 p = clamp(ivec2(floor(render_pos)), ivec2(0), params.render_size - 1);
	return texelFetch(depth_buffer, p, 0).x >= params.overlay_depth;
}

bool inside(vec2 render_pos) {
	return all(greaterThanEqual(render_pos, vec2(0.0))) && all(lessThan(render_pos, vec2(params.render_size)));
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.display_size))) {
		return;
	}
	vec4 color = texelFetch(color_buffer, pos, 0);
	vec2 to_render = vec2(params.render_size) / vec2(params.display_size);
	vec2 here = (vec2(pos) + 0.5) * to_render;

	// The overlay and one render pixel around it, the edge the anti-aliasing or upscaling blended: no wider, as what
	// is filled and then differs from the real frame is shown from the real frame, not from between the two.
	bool fill = false;
	for (int y = -1; y <= 1 && !fill; y++) {
		for (int x = -1; x <= 1 && !fill; x++) {
			fill = covered(here + vec2(x, y));
		}
	}

	if (fill) {
		// The nearest picture outside the overlay along eight ways, the nearer weighing more.
		const vec2 ways[8] = vec2[](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0),
				vec2(0.707, 0.707), vec2(-0.707, 0.707), vec2(0.707, -0.707), vec2(-0.707, -0.707));
		vec3 sum = vec3(0.0);
		float weight = 0.0;
		for (int i = 0; i < 8; i++) {
			float under = 0.0;
			float out_at = -1.0;
			for (float t = 1.0; inside(here + ways[i] * t); t = t * 1.5 + 1.0) {
				if (!covered(here + ways[i] * t)) {
					out_at = t;
					break;
				}
				under = t;
			}
			if (out_at < 0.0) {
				continue;
			}
			// The steps grow, so the edge is looked for between the last two.
			for (int step = 0; step < 4; step++) {
				float middle = 0.5 * (under + out_at);
				if (covered(here + ways[i] * middle)) {
					under = middle;
				} else {
					out_at = middle;
				}
			}
			vec2 from = clamp(here + ways[i] * (out_at + EDGE_MARGIN), vec2(0.5), vec2(params.render_size) - 0.5);
			if (covered(from)) {
				from = here + ways[i] * out_at;
			}
			float w = 1.0 / (out_at * out_at);
			sum += texelFetch(color_buffer, clamp(ivec2(from / to_render), ivec2(0), params.display_size - 1), 0).rgb * w;
			weight += w;
		}
		if (weight > 0.0) {
			color.rgb = sum / weight;
		}
	}

	imageStore(output_buffer, pos, color);
}
