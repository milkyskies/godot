/**************************************************************************/
/*  frame_generation.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "servers/rendering/rendering_device.h"

// FSR 3 frame generation (hacco #1441): the FidelityFX SDK's frame interpolation and optical flow, run on
// RenderingDevice as FSR 2 is. A frame is generated between the last two real ones from the scene's depth and motion
// vectors and the optical flow of the picture without the HUD; the HUD is put back on from the real frame.
//
// The SDK's own headers are not included here: they declare the same names as Godot's FSR 2 copy with other layouts,
// so they stay inside frame_generation.cpp.

namespace RendererRD {

class FrameGeneration {
public:
	struct Context;

	struct Camera {
		float z_near = 0.05f;
		float z_far = 4000.0f;
		float fov_y = 1.0f; // Radians.
		Vector2 jitter; // Pixels of the render size.
		float delta_ms = 16.6f;
	};

	FrameGeneration();
	~FrameGeneration();

	// A context for a view rendered at `p_render_size` (its depth and motion vectors) and shown at `p_display_size`
	// in `p_color_format` (the render target's); null when the SDK refuses it.
	Context *context_create(Size2i p_render_size, Size2i p_display_size, RD::DataFormat p_color_format);
	void context_free(Context *p_context);

	// After the 3D scene is drawn: its depth and motion vectors become the next generated frame's, and `p_color` (the
	// render target before any 2D is drawn) its picture without the HUD.
	void prepare(Context *p_context, RID p_depth, RID p_velocity, RID p_color, const Camera &p_camera);
	// After the 2D is drawn on `p_color`: the frame between the last real one and this one, with this one's HUD,
	// into the context's output. Returns it, or a null RID when there is no frame before this one to start from.
	RID generate(Context *p_context, RID p_color);
	// Starts over: the next frame is not joined to the one before (a camera cut, a teleport, a resize).
	void reset(Context *p_context);

	struct Pipelines;

private:
	Pipelines *pipelines = nullptr;
};

} // namespace RendererRD
