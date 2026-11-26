/**************************************************************************/
/*  frame_generation.cpp                                                  */
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

#include "frame_generation.h"

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/fi_compute_game_vector_field_inpainting_pyramid_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/fi_compute_inpainting_pyramid_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/fi_debug_view_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/fi_disocclusion_mask_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/fi_game_motion_vector_field_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/fi_inpainting_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/fi_interpolation_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/fi_optical_flow_vector_field_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/fi_reconstruct_and_dilate_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/fi_reconstruct_previous_depth_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/fi_setup_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/of_compute_luminance_pyramid_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/of_compute_optical_flow_advanced_pass_v5.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/of_compute_scd_divergence_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/of_filter_optical_flow_pass_v5.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/of_generate_scd_histogram_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/of_prepare_luma_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/frame_generation/of_scale_optical_flow_advanced_pass_v5.glsl.gen.h"

// First: the SDK's type names are renamed away from FSR 2's (ffx_rename.h).
#include "thirdparty/amd-fidelityfx/godot/ffx_rename.h"
#include "thirdparty/amd-fidelityfx/src/ffx_godot_compat.h"

#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>

using namespace RendererRD;

// The SDK version these sources are from (FidelityFX SDK 1.1.4).
#define FG_SDK_VERSION FFX_SDK_MAKE_VERSION(1, 1, 4)

namespace FrameGenerationTables {

enum Effect {
	EFFECT_FI,
	EFFECT_OF,
};

// What a binding no resource fills is given: Godot's reflection asks for every binding a shader declares.
enum Placeholder {
	PLACEHOLDER_TEXTURE2D,
	PLACEHOLDER_UTEXTURE2D,
	PLACEHOLDER_ITEXTURE2D,
	PLACEHOLDER_UBO,
	PLACEHOLDER_MAX,
};

struct SampledBinding {
	uint32_t slot;
	const wchar_t *name;
	Placeholder kind;
};

struct NamedBinding {
	uint32_t slot;
	const wchar_t *name;
};

struct UnusedBinding {
	uint32_t slot;
	Placeholder kind;
};

struct Lists {
	Vector<SampledBinding> srv_textures;
	Vector<NamedBinding> uav_textures;
	Vector<NamedBinding> srv_buffers;
	Vector<NamedBinding> uav_buffers;
	Vector<NamedBinding> constant_buffers;
	Vector<UnusedBinding> unused;
};

struct Pass {
	Effect effect;
	uint32_t pass;
	Lists lists;
};

} //namespace FrameGenerationTables

using namespace FrameGenerationTables;

// One pass's shader and pipeline, and the bindings its job is filled in by.
struct FrameGenerationPass {
	ShaderRD *shader = nullptr;
	RID version;
	RID shader_rid;
	RID pipeline;
	Lists lists;
};

struct FrameGeneration::Pipelines {
	FiReconstructAndDilatePassShaderRD fi_reconstruct_and_dilate;
	FiSetupPassShaderRD fi_setup;
	FiReconstructPreviousDepthPassShaderRD fi_reconstruct_previous_depth;
	FiGameMotionVectorFieldPassShaderRD fi_game_motion_vector_field;
	FiOpticalFlowVectorFieldPassShaderRD fi_optical_flow_vector_field;
	FiDisocclusionMaskPassShaderRD fi_disocclusion_mask;
	FiInterpolationPassShaderRD fi_interpolation;
	FiComputeInpaintingPyramidPassShaderRD fi_compute_inpainting_pyramid;
	FiInpaintingPassShaderRD fi_inpainting;
	FiComputeGameVectorFieldInpaintingPyramidPassShaderRD fi_compute_game_vector_field_inpainting_pyramid;
	FiDebugViewPassShaderRD fi_debug_view;
	OfPrepareLumaPassShaderRD of_prepare_luma;
	OfComputeLuminancePyramidPassShaderRD of_compute_luminance_pyramid;
	OfGenerateScdHistogramPassShaderRD of_generate_scd_histogram;
	OfComputeScdDivergencePassShaderRD of_compute_scd_divergence;
	OfComputeOpticalFlowAdvancedPassV5ShaderRD of_compute_optical_flow_advanced;
	OfFilterOpticalFlowPassV5ShaderRD of_filter_optical_flow;
	OfScaleOpticalFlowAdvancedPassV5ShaderRD of_scale_optical_flow_advanced;

	FrameGenerationPass fi[FFX_FRAMEINTERPOLATION_PASS_COUNT];
	FrameGenerationPass of[FFX_OPTICALFLOW_PASS_COUNT];

	RID linear_clamp_sampler;
	RID placeholders[PLACEHOLDER_MAX];
	// A storage image for each mip a pass binds past a texture's last (`fg_mip_view`), one a mip: a dispatch may take
	// several.
	static constexpr uint32_t MIPS_BOUND = 16;
	RID past_last_mip[MIPS_BOUND];
	FfxDeviceCapabilities capabilities = {};

	FrameGenerationPass *get(FfxEffect p_effect, FfxPass p_pass) {
		switch (p_effect) {
			case FFX_EFFECT_FRAMEINTERPOLATION:
				ERR_FAIL_UNSIGNED_INDEX_V(uint32_t(p_pass), uint32_t(FFX_FRAMEINTERPOLATION_PASS_COUNT), nullptr);
				return &fi[p_pass];
			case FFX_EFFECT_OPTICALFLOW:
				ERR_FAIL_UNSIGNED_INDEX_V(uint32_t(p_pass), uint32_t(FFX_OPTICALFLOW_PASS_COUNT), nullptr);
				return &of[p_pass];
			default:
				return nullptr;
		}
	}
};

/* The backend: the SDK's FfxInterface over RenderingDevice. */

// How many uniform buffers a context cycles through: optical flow stages its constants again for each level of its
// pyramid, so a frame takes about 40.
static constexpr uint32_t FG_UBO_RING_SIZE = 96;
static constexpr uint32_t FG_UBO_SIZE = 1024;
// CPU room for the constants staged before a run of jobs is executed (uint32_t each).
static constexpr uint32_t FG_STAGING_SIZE = 32 * 1024;

struct FrameGenerationResource {
	RID rid;
	bool owned = false; // Made by the backend; else a texture of Godot's registered for one call.
	bool is_buffer = false;
	uint32_t effect_context = 0;
	FfxResourceDescription description = {};
	LocalVector<RID> mip_slices;
	wchar_t name[FFX_RESOURCE_NAME_SIZE] = {};
};

struct FrameGenerationBackend {
	FrameGeneration::Pipelines *pipelines = nullptr;
	LocalVector<FrameGenerationResource> resources;
	LocalVector<uint32_t> free_slots;
	LocalVector<FfxGpuJobDescription> jobs;
	uint32_t next_effect_context = 1;

	RID ubo_ring[FG_UBO_RING_SIZE];
	uint32_t ubo_index = 0;
	uint32_t staging[FG_STAGING_SIZE];
	uint32_t staging_used = 0;

	int32_t add(const FrameGenerationResource &p_resource) {
		if (!free_slots.is_empty()) {
			uint32_t slot = free_slots[free_slots.size() - 1];
			free_slots.remove_at(free_slots.size() - 1);
			resources[slot] = p_resource;
			return int32_t(slot);
		}
		resources.push_back(p_resource);
		return int32_t(resources.size() - 1);
	}

	void remove(int32_t p_index) {
		FrameGenerationResource &res = resources[p_index];
		RenderingDevice *rd = RD::get_singleton();
		// A slice goes with its texture: one made through a registration of a texture freed before is gone already.
		for (RID &slice : res.mip_slices) {
			if (slice.is_valid() && rd->texture_is_valid(slice)) {
				rd->free_rid(slice);
			}
		}
		if (res.owned && res.rid.is_valid() && (res.is_buffer || rd->texture_is_valid(res.rid))) {
			rd->free_rid(res.rid);
		}
		res = FrameGenerationResource();
		free_slots.push_back(uint32_t(p_index));
	}

	FrameGenerationResource *get(FfxResourceInternal p_resource) {
		if (p_resource.internalIndex < 0 || uint32_t(p_resource.internalIndex) >= resources.size() || resources[p_resource.internalIndex].rid.is_null()) {
			return nullptr;
		}
		return &resources[p_resource.internalIndex];
	}
};

static FrameGenerationBackend &fg_backend(FfxInterface *p_interface) {
	return *reinterpret_cast<FrameGenerationBackend *>(p_interface->scratchBuffer);
}

static RD::DataFormat fg_format_to_rd(FfxSurfaceFormat p_format) {
	switch (p_format) {
		case FFX_SURFACE_FORMAT_R32G32B32A32_TYPELESS:
		case FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT:
			return RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		case FFX_SURFACE_FORMAT_R32G32B32A32_UINT:
			return RD::DATA_FORMAT_R32G32B32A32_UINT;
		case FFX_SURFACE_FORMAT_R16G16B16A16_TYPELESS:
		case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT:
			return RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		case FFX_SURFACE_FORMAT_R32G32B32_FLOAT:
			return RD::DATA_FORMAT_R32G32B32_SFLOAT;
		case FFX_SURFACE_FORMAT_R32G32_TYPELESS:
		case FFX_SURFACE_FORMAT_R32G32_FLOAT:
			return RD::DATA_FORMAT_R32G32_SFLOAT;
		case FFX_SURFACE_FORMAT_R8_UINT:
			return RD::DATA_FORMAT_R8_UINT;
		case FFX_SURFACE_FORMAT_R32_TYPELESS:
		case FFX_SURFACE_FORMAT_R32_UINT:
			return RD::DATA_FORMAT_R32_UINT;
		case FFX_SURFACE_FORMAT_R8G8B8A8_TYPELESS:
		case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM:
			return RD::DATA_FORMAT_R8G8B8A8_UNORM;
		case FFX_SURFACE_FORMAT_R8G8B8A8_SNORM:
			return RD::DATA_FORMAT_R8G8B8A8_SNORM;
		case FFX_SURFACE_FORMAT_R8G8B8A8_SRGB:
			return RD::DATA_FORMAT_R8G8B8A8_SRGB;
		case FFX_SURFACE_FORMAT_B8G8R8A8_TYPELESS:
		case FFX_SURFACE_FORMAT_B8G8R8A8_UNORM:
			return RD::DATA_FORMAT_B8G8R8A8_UNORM;
		case FFX_SURFACE_FORMAT_B8G8R8A8_SRGB:
			return RD::DATA_FORMAT_B8G8R8A8_SRGB;
		case FFX_SURFACE_FORMAT_R11G11B10_FLOAT:
			return RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32;
		case FFX_SURFACE_FORMAT_R10G10B10A2_TYPELESS:
		case FFX_SURFACE_FORMAT_R10G10B10A2_UNORM:
			return RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32;
		case FFX_SURFACE_FORMAT_R16G16_TYPELESS:
		case FFX_SURFACE_FORMAT_R16G16_FLOAT:
			return RD::DATA_FORMAT_R16G16_SFLOAT;
		case FFX_SURFACE_FORMAT_R16G16_UINT:
			return RD::DATA_FORMAT_R16G16_UINT;
		case FFX_SURFACE_FORMAT_R16G16_SINT:
			return RD::DATA_FORMAT_R16G16_SINT;
		case FFX_SURFACE_FORMAT_R16_TYPELESS:
		case FFX_SURFACE_FORMAT_R16_FLOAT:
			return RD::DATA_FORMAT_R16_SFLOAT;
		case FFX_SURFACE_FORMAT_R16_UINT:
			return RD::DATA_FORMAT_R16_UINT;
		case FFX_SURFACE_FORMAT_R16_UNORM:
			return RD::DATA_FORMAT_R16_UNORM;
		case FFX_SURFACE_FORMAT_R16_SNORM:
			return RD::DATA_FORMAT_R16_SNORM;
		case FFX_SURFACE_FORMAT_R8_TYPELESS:
		case FFX_SURFACE_FORMAT_R8_UNORM:
			return RD::DATA_FORMAT_R8_UNORM;
		case FFX_SURFACE_FORMAT_R8G8_TYPELESS:
		case FFX_SURFACE_FORMAT_R8G8_UNORM:
			return RD::DATA_FORMAT_R8G8_UNORM;
		case FFX_SURFACE_FORMAT_R8G8_UINT:
			return RD::DATA_FORMAT_R8G8_UINT;
		case FFX_SURFACE_FORMAT_R32_FLOAT:
			return RD::DATA_FORMAT_R32_SFLOAT;
		case FFX_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP:
			return RD::DATA_FORMAT_E5B9G9R9_UFLOAT_PACK32;
		case FFX_SURFACE_FORMAT_UNKNOWN:
		default:
			return RD::DATA_FORMAT_MAX;
	}
}

static FfxSurfaceFormat fg_format_from_rd(RD::DataFormat p_format) {
	switch (p_format) {
		case RD::DATA_FORMAT_R32G32B32A32_SFLOAT:
			return FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT;
		case RD::DATA_FORMAT_R16G16B16A16_SFLOAT:
			return FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
		case RD::DATA_FORMAT_R32G32_SFLOAT:
			return FFX_SURFACE_FORMAT_R32G32_FLOAT;
		case RD::DATA_FORMAT_R32_UINT:
			return FFX_SURFACE_FORMAT_R32_UINT;
		case RD::DATA_FORMAT_R8G8B8A8_UNORM:
			return FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
		case RD::DATA_FORMAT_R8G8B8A8_SRGB:
			return FFX_SURFACE_FORMAT_R8G8B8A8_SRGB;
		case RD::DATA_FORMAT_B8G8R8A8_UNORM:
			return FFX_SURFACE_FORMAT_B8G8R8A8_UNORM;
		case RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32:
			return FFX_SURFACE_FORMAT_R10G10B10A2_UNORM;
		case RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32:
			return FFX_SURFACE_FORMAT_R11G11B10_FLOAT;
		case RD::DATA_FORMAT_R16G16_SFLOAT:
			return FFX_SURFACE_FORMAT_R16G16_FLOAT;
		case RD::DATA_FORMAT_R16G16_UINT:
			return FFX_SURFACE_FORMAT_R16G16_UINT;
		case RD::DATA_FORMAT_R16_SFLOAT:
			return FFX_SURFACE_FORMAT_R16_FLOAT;
		case RD::DATA_FORMAT_R16_UINT:
			return FFX_SURFACE_FORMAT_R16_UINT;
		case RD::DATA_FORMAT_R16_UNORM:
			return FFX_SURFACE_FORMAT_R16_UNORM;
		case RD::DATA_FORMAT_R8_UNORM:
			return FFX_SURFACE_FORMAT_R8_UNORM;
		case RD::DATA_FORMAT_R8G8_UNORM:
			return FFX_SURFACE_FORMAT_R8G8_UNORM;
		case RD::DATA_FORMAT_R32_SFLOAT:
		case RD::DATA_FORMAT_D32_SFLOAT:
		case RD::DATA_FORMAT_D32_SFLOAT_S8_UINT:
			return FFX_SURFACE_FORMAT_R32_FLOAT;
		case RD::DATA_FORMAT_D24_UNORM_S8_UINT:
			return FFX_SURFACE_FORMAT_R32_FLOAT;
		default:
			return FFX_SURFACE_FORMAT_UNKNOWN;
	}
}

static FfxVersionNumber fg_get_sdk_version(FfxInterface *) {
	return FG_SDK_VERSION;
}

static FfxErrorCode fg_get_effect_gpu_memory_usage(FfxInterface *, FfxUInt32, FfxEffectMemoryUsage *r_usage) {
	*r_usage = {};
	return FFX_OK;
}

static FfxErrorCode fg_create_backend_context(FfxInterface *p_interface, FfxEffect, FfxEffectBindlessConfig *, FfxUInt32 *r_effect_context) {
	FrameGenerationBackend &backend = fg_backend(p_interface);
	*r_effect_context = backend.next_effect_context++;
	return FFX_OK;
}

static FfxErrorCode fg_get_device_capabilities(FfxInterface *p_interface, FfxDeviceCapabilities *r_capabilities) {
	*r_capabilities = fg_backend(p_interface).pipelines->capabilities;
	return FFX_OK;
}

static FfxErrorCode fg_destroy_backend_context(FfxInterface *p_interface, FfxUInt32 p_effect_context) {
	FrameGenerationBackend &backend = fg_backend(p_interface);
	for (uint32_t i = 0; i < backend.resources.size(); i++) {
		if (backend.resources[i].rid.is_valid() && backend.resources[i].effect_context == p_effect_context) {
			backend.remove(int32_t(i));
		}
	}
	return FFX_OK;
}

static FfxErrorCode fg_create_resource(FfxInterface *p_interface, const FfxCreateResourceDescription *p_desc, FfxUInt32 p_effect_context, FfxResourceInternal *r_resource) {
	RenderingDevice *rd = RD::get_singleton();
	FrameGenerationBackend &backend = fg_backend(p_interface);
	FfxResourceDescription desc = p_desc->resourceDescription;

	FrameGenerationResource res;
	res.owned = true;
	res.effect_context = p_effect_context;
	if (p_desc->name) {
		wcsncpy(res.name, p_desc->name, FFX_RESOURCE_NAME_SIZE - 1);
	}

	Vector<uint8_t> init;
	if (p_desc->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER && p_desc->initData.size > 0) {
		init.resize(p_desc->initData.size);
		memcpy(init.ptrw(), p_desc->initData.buffer, p_desc->initData.size);
	} else if (p_desc->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE && p_desc->initData.size > 0) {
		init.resize(p_desc->initData.size);
		memset(init.ptrw(), p_desc->initData.value, p_desc->initData.size);
	}

	if (desc.type == FFX_RESOURCE_TYPE_BUFFER) {
		res.is_buffer = true;
		uint32_t size = MAX(desc.size, 4u);
		if (init.is_empty()) {
			init.resize(size);
			memset(init.ptrw(), 0, size);
		}
		res.rid = rd->storage_buffer_create(size, Span<uint8_t>(init.ptr(), MIN(uint32_t(init.size()), size)), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		ERR_FAIL_COND_V(res.rid.is_null(), FFX_ERROR_BACKEND_API_ERROR);
	} else {
		if (desc.mipCount == 0) {
			desc.mipCount = uint32_t(1 + std::floor(std::log2(MAX(MAX(desc.width, desc.height), desc.depth))));
		}

		RD::TextureFormat tf;
		switch (desc.type) {
			case FFX_RESOURCE_TYPE_TEXTURE1D:
				tf.texture_type = RD::TEXTURE_TYPE_1D;
				break;
			case FFX_RESOURCE_TYPE_TEXTURE3D:
				tf.texture_type = RD::TEXTURE_TYPE_3D;
				break;
			default:
				tf.texture_type = RD::TEXTURE_TYPE_2D;
				break;
		}
		tf.format = fg_format_to_rd(desc.format);
		ERR_FAIL_COND_V_MSG(tf.format == RD::DATA_FORMAT_MAX, FFX_ERROR_INVALID_ARGUMENT, vformat("Frame generation asked for a texture format Godot does not map (%d).", int(desc.format)));
		tf.width = MAX(desc.width, 1u);
		tf.height = MAX(desc.height, 1u);
		tf.depth = MAX(desc.depth, 1u);
		tf.mipmaps = desc.mipCount;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		if (desc.usage & FFX_RESOURCE_USAGE_UAV) {
			tf.usage_bits |= RD::TEXTURE_USAGE_STORAGE_BIT;
		}
		if (desc.usage & (FFX_RESOURCE_USAGE_RENDERTARGET | FFX_RESOURCE_USAGE_DCC_RENDERTARGET)) {
			tf.usage_bits |= RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
		}

		Vector<Vector<uint8_t>> data;
		if (!init.is_empty()) {
			// The SDK gives the first mip's bytes; RD wants every mip's.
			uint32_t pixel = RD::get_singleton()->get_image_format_pixel_size(tf.format);
			uint32_t w = tf.width, h = tf.height;
			for (uint32_t m = 0; m < tf.mipmaps; m++) {
				Vector<uint8_t> mip;
				mip.resize(w * h * tf.depth * pixel);
				if (m == 0) {
					memset(mip.ptrw(), 0, mip.size());
					memcpy(mip.ptrw(), init.ptr(), MIN(init.size(), mip.size()));
				} else {
					memset(mip.ptrw(), p_desc->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE ? p_desc->initData.value : 0, mip.size());
				}
				data.push_back(mip);
				w = MAX(1u, w / 2);
				h = MAX(1u, h / 2);
			}
		}

		res.rid = rd->texture_create(tf, RD::TextureView(), data);
		ERR_FAIL_COND_V(res.rid.is_null(), FFX_ERROR_BACKEND_API_ERROR);
	}
	res.description = desc;
	if (p_desc->name) {
		rd->set_resource_name(res.rid, String(p_desc->name));
	}

	r_resource->internalIndex = backend.add(res);
	return FFX_OK;
}

static FfxErrorCode fg_register_resource(FfxInterface *p_interface, const FfxResource *p_resource, FfxUInt32 p_effect_context, FfxResourceInternal *r_resource) {
	if (p_resource->resource == nullptr) {
		r_resource->internalIndex = -1;
		return FFX_OK;
	}

	FrameGenerationBackend &backend = fg_backend(p_interface);
	FrameGenerationResource res;
	res.rid = RID::from_uint64(uint64_t(uintptr_t(p_resource->resource)));
	res.owned = false;
	res.is_buffer = p_resource->description.type == FFX_RESOURCE_TYPE_BUFFER;
	res.effect_context = p_effect_context;
	res.description = p_resource->description;
	r_resource->internalIndex = backend.add(res);
	return FFX_OK;
}

static FfxResource fg_get_resource(FfxInterface *p_interface, FfxResourceInternal p_resource) {
	FfxResource out = {};
	FrameGenerationResource *res = fg_backend(p_interface).get(p_resource);
	if (res) {
		out.resource = reinterpret_cast<void *>(uintptr_t(res->rid.get_id()));
		out.description = res->description;
		out.state = FFX_RESOURCE_STATE_UNORDERED_ACCESS;
		wcsncpy(out.name, res->name, FFX_RESOURCE_NAME_SIZE - 1);
	}
	return out;
}

static FfxErrorCode fg_unregister_resources(FfxInterface *p_interface, FfxCommandList, FfxUInt32 p_effect_context) {
	FrameGenerationBackend &backend = fg_backend(p_interface);
	for (uint32_t i = 0; i < backend.resources.size(); i++) {
		FrameGenerationResource &res = backend.resources[i];
		if (res.rid.is_valid() && !res.owned && res.effect_context == p_effect_context) {
			backend.remove(int32_t(i));
		}
	}
	return FFX_OK;
}

static FfxErrorCode fg_register_static_resource(FfxInterface *, const FfxStaticResourceDescription *, FfxUInt32) {
	return FFX_ERROR_BACKEND_API_ERROR; // Bindless only; neither effect asks.
}

static FfxResourceDescription fg_get_resource_description(FfxInterface *p_interface, FfxResourceInternal p_resource) {
	FrameGenerationResource *res = fg_backend(p_interface).get(p_resource);
	return res ? res->description : FfxResourceDescription{};
}

static FfxErrorCode fg_destroy_resource(FfxInterface *p_interface, FfxResourceInternal p_resource, FfxUInt32 p_effect_context) {
	FrameGenerationBackend &backend = fg_backend(p_interface);
	// An effect keeps the indices of what it registered after they were let go, and a slot is used again: only what
	// the effect itself made is its to destroy.
	FrameGenerationResource *res = backend.get(p_resource);
	if (res && res->owned && res->effect_context == p_effect_context) {
		backend.remove(p_resource.internalIndex);
	}
	return FFX_OK;
}

static FfxErrorCode fg_map_resource(FfxInterface *, FfxResourceInternal, void **) {
	return FFX_ERROR_BACKEND_API_ERROR;
}

static FfxErrorCode fg_unmap_resource(FfxInterface *, FfxResourceInternal) {
	return FFX_ERROR_BACKEND_API_ERROR;
}

static FfxErrorCode fg_stage_constant_buffer_data(FfxInterface *p_interface, void *p_data, FfxUInt32 p_size, FfxConstantBuffer *r_constant_buffer) {
	FrameGenerationBackend &backend = fg_backend(p_interface);
	uint32_t words = (p_size + 3) / 4;
	ERR_FAIL_COND_V(words > FG_UBO_SIZE / 4, FFX_ERROR_OUT_OF_RANGE);
	if (backend.staging_used + words > FG_STAGING_SIZE) {
		backend.staging_used = 0; // Older staged constants were uploaded when their jobs ran.
	}
	uint32_t *dst = backend.staging + backend.staging_used;
	memcpy(dst, p_data, p_size);
	backend.staging_used += words;
	r_constant_buffer->data = dst;
	r_constant_buffer->num32BitEntries = words;
	return FFX_OK;
}

static FfxErrorCode fg_create_pipeline(FfxInterface *p_interface, FfxEffect p_effect, FfxPass p_pass, uint32_t, const FfxPipelineDescription *p_description, FfxUInt32, FfxPipelineState *r_pipeline) {
	FrameGenerationBackend &backend = fg_backend(p_interface);
	FrameGenerationPass *pass = backend.pipelines->get(p_effect, p_pass);
	ERR_FAIL_NULL_V(pass, FFX_ERROR_INVALID_ARGUMENT);

	if (pass->pipeline.is_null()) {
		pass->shader_rid = pass->shader->version_get_shader(pass->version, 0);
		ERR_FAIL_COND_V(pass->shader_rid.is_null(), FFX_ERROR_BACKEND_API_ERROR);
		pass->pipeline = RD::get_singleton()->compute_pipeline_create(pass->shader_rid);
		ERR_FAIL_COND_V(pass->pipeline.is_null(), FFX_ERROR_BACKEND_API_ERROR);
	}

	*r_pipeline = {};
	r_pipeline->passId = p_pass;
	r_pipeline->pipeline = reinterpret_cast<FfxPipeline>(pass);
	r_pipeline->rootSignature = reinterpret_cast<FfxRootSignature>(pass);
	wcsncpy(r_pipeline->name, p_description->name, FFX_RESOURCE_NAME_SIZE - 1);

	const Lists &l = pass->lists;
	ERR_FAIL_COND_V(l.srv_textures.size() > FFX_MAX_NUM_SRVS || l.uav_textures.size() > FFX_MAX_NUM_UAVS, FFX_ERROR_OUT_OF_RANGE);
	ERR_FAIL_COND_V(l.srv_buffers.size() > FFX_MAX_NUM_SRVS || l.uav_buffers.size() > FFX_MAX_NUM_UAVS, FFX_ERROR_OUT_OF_RANGE);
	ERR_FAIL_COND_V(l.constant_buffers.size() > FFX_MAX_NUM_CONST_BUFFERS, FFX_ERROR_OUT_OF_RANGE);

	auto fill = [](FfxResourceBinding &r_binding, uint32_t p_slot, const wchar_t *p_name) {
		r_binding = {};
		r_binding.slotIndex = p_slot;
		wcsncpy(r_binding.name, p_name, FFX_RESOURCE_NAME_SIZE - 1);
	};
	r_pipeline->srvTextureCount = l.srv_textures.size();
	for (int i = 0; i < l.srv_textures.size(); i++) {
		fill(r_pipeline->srvTextureBindings[i], l.srv_textures[i].slot, l.srv_textures[i].name);
	}
	r_pipeline->uavTextureCount = l.uav_textures.size();
	for (int i = 0; i < l.uav_textures.size(); i++) {
		fill(r_pipeline->uavTextureBindings[i], l.uav_textures[i].slot, l.uav_textures[i].name);
	}
	r_pipeline->srvBufferCount = l.srv_buffers.size();
	for (int i = 0; i < l.srv_buffers.size(); i++) {
		fill(r_pipeline->srvBufferBindings[i], l.srv_buffers[i].slot, l.srv_buffers[i].name);
	}
	r_pipeline->uavBufferCount = l.uav_buffers.size();
	for (int i = 0; i < l.uav_buffers.size(); i++) {
		fill(r_pipeline->uavBufferBindings[i], l.uav_buffers[i].slot, l.uav_buffers[i].name);
	}
	r_pipeline->constCount = l.constant_buffers.size();
	for (int i = 0; i < l.constant_buffers.size(); i++) {
		fill(r_pipeline->constantBufferBindings[i], l.constant_buffers[i].slot, l.constant_buffers[i].name);
	}
	return FFX_OK;
}

static FfxErrorCode fg_destroy_pipeline(FfxInterface *, FfxPipelineState *, FfxUInt32) {
	// Pipelines are the effect's, shared by every context, and freed with it.
	return FFX_OK;
}

static FfxErrorCode fg_schedule_gpu_job(FfxInterface *p_interface, const FfxGpuJobDescription *p_job) {
	fg_backend(p_interface).jobs.push_back(*p_job);
	return FFX_OK;
}

static RID fg_mip_view(FrameGenerationResource &p_res, uint32_t p_mip, const RID *p_past_last) {
	// A pass binds more mips than a smaller texture has (the inpainting pyramid's 13). The SDK's own backends bind
	// the last again, but RenderingDevice refuses one texture bound twice in a dispatch: those get an image of their
	// own, which the pass never writes (it stops at the mips it is told the texture has).
	if (p_mip >= MAX(p_res.description.mipCount, 1u)) {
		ERR_FAIL_UNSIGNED_INDEX_V(p_mip, FrameGeneration::Pipelines::MIPS_BOUND, RID());
		return p_past_last[p_mip];
	}
	// A texture with one mip is bound whole; one with more by slices only, its first too: RenderingDevice takes the
	// whole texture beside slices of it in one dispatch for the texture depending on itself.
	if (p_res.description.mipCount <= 1) {
		return p_res.rid;
	}
	if (p_res.mip_slices.size() < p_res.description.mipCount) {
		p_res.mip_slices.resize(p_res.description.mipCount);
	}
	ERR_FAIL_UNSIGNED_INDEX_V(p_mip, p_res.mip_slices.size(), RID());
	if (p_res.mip_slices[p_mip].is_null()) {
		p_res.mip_slices[p_mip] = RD::get_singleton()->texture_create_shared_from_slice(RD::TextureView(), p_res.rid, 0, p_mip);
	}
	return p_res.mip_slices[p_mip];
}

static FfxErrorCode fg_execute_compute(FrameGenerationBackend &p_backend, const FfxComputeJobDescription &p_job) {
	RenderingDevice *rd = RD::get_singleton();
	FrameGenerationPass *pass = reinterpret_cast<FrameGenerationPass *>(p_job.pipeline.pipeline);
	ERR_FAIL_NULL_V(pass, FFX_ERROR_INVALID_ARGUMENT);
	const Lists &l = pass->lists;
	FrameGeneration::Pipelines &pipelines = *p_backend.pipelines;

	thread_local LocalVector<RD::Uniform> uniforms;
	uniforms.clear();

	for (uint32_t i = 0; i < p_job.pipeline.srvTextureCount; i++) {
		FrameGenerationResource *res = p_backend.get(p_job.srvTextures[i].resource);
		RID rid = res ? res->rid : pipelines.placeholders[l.srv_textures[i].kind];
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, p_job.pipeline.srvTextureBindings[i].slotIndex, rid));
	}
	for (uint32_t i = 0; i < p_job.pipeline.uavTextureCount; i++) {
		FrameGenerationResource *res = p_backend.get(p_job.uavTextures[i].resource);
		ERR_FAIL_NULL_V_MSG(res, FFX_ERROR_INVALID_ARGUMENT, vformat("Frame generation: nothing to write at binding %d.", p_job.pipeline.uavTextureBindings[i].slotIndex));
		RID view = fg_mip_view(*res, p_job.uavTextures[i].mip, pipelines.past_last_mip);
		ERR_FAIL_COND_V(view.is_null(), FFX_ERROR_BACKEND_API_ERROR);
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, p_job.pipeline.uavTextureBindings[i].slotIndex, view));
	}
	for (uint32_t i = 0; i < p_job.pipeline.srvBufferCount; i++) {
		FrameGenerationResource *res = p_backend.get(p_job.srvBuffers[i].resource);
		ERR_FAIL_NULL_V(res, FFX_ERROR_INVALID_ARGUMENT);
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, p_job.pipeline.srvBufferBindings[i].slotIndex, res->rid));
	}
	for (uint32_t i = 0; i < p_job.pipeline.uavBufferCount; i++) {
		FrameGenerationResource *res = p_backend.get(p_job.uavBuffers[i].resource);
		ERR_FAIL_NULL_V(res, FFX_ERROR_INVALID_ARGUMENT);
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, p_job.pipeline.uavBufferBindings[i].slotIndex, res->rid));
	}
	for (uint32_t i = 0; i < p_job.pipeline.constCount; i++) {
		RID ubo = p_backend.ubo_ring[p_backend.ubo_index];
		p_backend.ubo_index = (p_backend.ubo_index + 1) % FG_UBO_RING_SIZE;
		rd->buffer_update(ubo, 0, p_job.cbs[i].num32BitEntries * sizeof(uint32_t), p_job.cbs[i].data);
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, p_job.pipeline.constantBufferBindings[i].slotIndex, ubo));
	}
	for (const UnusedBinding &unused : l.unused) {
		RD::UniformType type = unused.kind == PLACEHOLDER_UBO ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : RD::UNIFORM_TYPE_TEXTURE;
		uniforms.push_back(RD::Uniform(type, unused.slot, pipelines.placeholders[unused.kind]));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 1000, pipelines.linear_clamp_sampler));

	RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache_vec(pass->shader_rid, 0, uniforms);
	ERR_FAIL_COND_V(uniform_set.is_null(), FFX_ERROR_BACKEND_API_ERROR);

	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pass->pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	rd->compute_list_dispatch(list, MAX(p_job.dimensions[0], 1u), MAX(p_job.dimensions[1], 1u), MAX(p_job.dimensions[2], 1u));
	rd->compute_list_end();
	return FFX_OK;
}

static FfxErrorCode fg_execute_gpu_jobs(FfxInterface *p_interface, FfxCommandList, FfxUInt32) {
	RenderingDevice *rd = RD::get_singleton();
	FrameGenerationBackend &backend = fg_backend(p_interface);
	FfxErrorCode error = FFX_OK;

	for (const FfxGpuJobDescription &job : backend.jobs) {
		switch (job.jobType) {
			case FFX_GPU_JOB_CLEAR_FLOAT: {
				FrameGenerationResource *res = backend.get(job.clearJobDescriptor.target);
				if (!res) {
					break;
				}
				if (res->is_buffer) {
					rd->buffer_clear(res->rid, 0, res->description.size);
				} else {
					const float *c = job.clearJobDescriptor.color;
					rd->texture_clear(res->rid, Color(c[0], c[1], c[2], c[3]), 0, res->description.mipCount, 0, 1);
				}
			} break;
			case FFX_GPU_JOB_COPY: {
				FrameGenerationResource *src = backend.get(job.copyJobDescriptor.src);
				FrameGenerationResource *dst = backend.get(job.copyJobDescriptor.dst);
				if (!src || !dst) {
					break;
				}
				if (src->is_buffer && dst->is_buffer) {
					uint32_t size = job.copyJobDescriptor.size ? job.copyJobDescriptor.size : MIN(src->description.size, dst->description.size);
					rd->buffer_copy(src->rid, dst->rid, job.copyJobDescriptor.srcOffset, job.copyJobDescriptor.dstOffset, size);
				} else if (!src->is_buffer && !dst->is_buffer) {
					uint32_t mips = MIN(src->description.mipCount, dst->description.mipCount);
					Vector3 size(MIN(src->description.width, dst->description.width), MIN(src->description.height, dst->description.height), 1);
					for (uint32_t m = 0; m < MAX(mips, 1u); m++) {
						rd->texture_copy(src->rid, dst->rid, Vector3(), Vector3(), size, m, m, 0, 0);
						size.x = MAX(1.0f, Math::floor(size.x / 2));
						size.y = MAX(1.0f, Math::floor(size.y / 2));
					}
				} else {
					error = FFX_ERROR_INVALID_ARGUMENT;
				}
			} break;
			case FFX_GPU_JOB_COMPUTE: {
				error = fg_execute_compute(backend, job.computeJobDescriptor);
			} break;
			case FFX_GPU_JOB_BARRIER:
			case FFX_GPU_JOB_DISCARD:
				// RenderingDevice's graph places every barrier itself.
				break;
			default:
				error = FFX_ERROR_INVALID_ARGUMENT;
				break;
		}
		if (error != FFX_OK) {
			break;
		}
	}

	backend.jobs.clear();
	return error;
}

static FfxErrorCode fg_register_constant_buffer_allocator_unused(FfxInterface *, FfxConstantBufferAllocator) {
	return FFX_OK;
}

/* A view's frame generation. */

struct FrameGeneration::Context {
	FrameGenerationBackend backend;
	FfxInterface interface = {};
	FfxFrameInterpolationContext fi = {};
	FfxOpticalflowContext of = {};
	bool fi_made = false;
	bool of_made = false;
	uint32_t shared_context = 0;

	Size2i render_size;
	Size2i display_size;
	RD::DataFormat color_format = RD::DATA_FORMAT_MAX;
	// A float render target (HDR 2D) holds linear color, read as scRGB at 80 nits for 1 (unchanged); else sRGB.
	FfxBackbufferTransferFunction transfer = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
	float max_luminance = 1.0f;

	// The shared resources the prepare pass writes and interpolation reads.
	FfxResourceInternal dilated_depth = { -1 };
	FfxResourceInternal dilated_motion_vectors = { -1 };
	FfxResourceInternal reconstructed_prev_depth = { -1 };
	FfxResourceInternal optical_flow_vector = { -1 };
	FfxResourceInternal optical_flow_scd = { -1 };

	RID hudless; // The real frame before its 2D.
	RID output; // The generated frame.

	FrameGeneration::Camera camera;
	uint64_t frame_id = 0;
	bool reset = true;
	bool prepared = false;
	bool has_previous = false;
};

static FfxResource fg_resource(RID p_rid, const wchar_t *p_name) {
	FfxResource res = {};
	if (p_rid.is_null()) {
		return res;
	}
	RD::TextureFormat tf = RD::get_singleton()->texture_get_format(p_rid);
	res.resource = reinterpret_cast<void *>(uintptr_t(p_rid.get_id()));
	res.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
	res.description.format = fg_format_from_rd(tf.format);
	res.description.width = tf.width;
	res.description.height = tf.height;
	res.description.depth = 1;
	res.description.mipCount = tf.mipmaps;
	res.description.flags = FFX_RESOURCE_FLAGS_NONE;
	res.description.usage = (tf.usage_bits & RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? FFX_RESOURCE_USAGE_DEPTHTARGET : FFX_RESOURCE_USAGE_READ_ONLY;
	res.state = FFX_RESOURCE_STATE_COMPUTE_READ;
	wcsncpy(res.name, p_name, FFX_RESOURCE_NAME_SIZE - 1);
	return res;
}

FrameGeneration::FrameGeneration() {
	pipelines = memnew(Pipelines);
	RenderingDevice *rd = RD::get_singleton();

	FfxDeviceCapabilities &caps = pipelines->capabilities;
	caps.maximumSupportedShaderModel = FFX_SHADER_MODEL_6_0;
	caps.waveLaneCountMin = 32;
	caps.waveLaneCountMax = 32;
	caps.fp16Supported = false; // The half-precision permutations are not built.
	caps.raytracingSupported = false;

	// Godot's depth is reversed (1 near, 0 far); its motion vectors leave out the jitter and are at the render size.
	String defines =
			"\n#define FFX_GPU 1\n"
			"\n#define FFX_GLSL 1\n"
			"\n#define FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS 1\n"
			"\n#define FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS 0\n"
			"\n#define FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH 1\n";
	Vector<String> modes;
	modes.push_back("");

	const Pass table[] = {
#include "frame_generation_bindings.inc"
	};
	ShaderRD *fi_shaders[FFX_FRAMEINTERPOLATION_PASS_COUNT] = {
		&pipelines->fi_reconstruct_and_dilate,
		&pipelines->fi_setup,
		&pipelines->fi_reconstruct_previous_depth,
		&pipelines->fi_game_motion_vector_field,
		&pipelines->fi_optical_flow_vector_field,
		&pipelines->fi_disocclusion_mask,
		&pipelines->fi_interpolation,
		&pipelines->fi_compute_inpainting_pyramid,
		&pipelines->fi_inpainting,
		&pipelines->fi_compute_game_vector_field_inpainting_pyramid,
		&pipelines->fi_debug_view,
	};
	ShaderRD *of_shaders[FFX_OPTICALFLOW_PASS_COUNT] = {
		&pipelines->of_prepare_luma,
		&pipelines->of_compute_luminance_pyramid,
		&pipelines->of_generate_scd_histogram,
		&pipelines->of_compute_scd_divergence,
		&pipelines->of_compute_optical_flow_advanced,
		&pipelines->of_filter_optical_flow,
		&pipelines->of_scale_optical_flow_advanced,
	};
	for (const Pass &entry : table) {
		FrameGenerationPass &pass = entry.effect == EFFECT_FI ? pipelines->fi[entry.pass] : pipelines->of[entry.pass];
		pass.shader = entry.effect == EFFECT_FI ? fi_shaders[entry.pass] : of_shaders[entry.pass];
		pass.lists = entry.lists;
		pass.shader->initialize(modes, defines);
		pass.version = pass.shader->version_create();
	}

	RD::SamplerState sampler;
	sampler.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	sampler.min_filter = RD::SAMPLER_FILTER_LINEAR;
	sampler.mip_filter = RD::SAMPLER_FILTER_LINEAR;
	sampler.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler.min_lod = -1000.0f;
	sampler.max_lod = 1000.0f;
	pipelines->linear_clamp_sampler = rd->sampler_create(sampler);

	auto placeholder = [&](RD::DataFormat p_format, int p_pixel) {
		RD::TextureFormat tf;
		tf.format = p_format;
		tf.width = 1;
		tf.height = 1;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT;
		Vector<uint8_t> zero;
		zero.resize(p_pixel);
		memset(zero.ptrw(), 0, p_pixel);
		Vector<Vector<uint8_t>> data;
		data.push_back(zero);
		return rd->texture_create(tf, RD::TextureView(), data);
	};
	pipelines->placeholders[PLACEHOLDER_TEXTURE2D] = placeholder(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, 8);
	pipelines->placeholders[PLACEHOLDER_UTEXTURE2D] = placeholder(RD::DATA_FORMAT_R32_UINT, 4);
	pipelines->placeholders[PLACEHOLDER_ITEXTURE2D] = placeholder(RD::DATA_FORMAT_R16G16_SINT, 4);
	pipelines->placeholders[PLACEHOLDER_UBO] = rd->uniform_buffer_create(FG_UBO_SIZE);
	for (RID &rid : pipelines->past_last_mip) {
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT; // The pyramid's.
		tf.width = 1;
		tf.height = 1;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		rid = rd->texture_create(tf, RD::TextureView());
	}
}

FrameGeneration::~FrameGeneration() {
	RenderingDevice *rd = RD::get_singleton();
	for (FrameGenerationPass &pass : pipelines->fi) {
		if (pass.pipeline.is_valid()) {
			rd->free_rid(pass.pipeline);
		}
		if (pass.shader) {
			pass.shader->version_free(pass.version);
		}
	}
	for (FrameGenerationPass &pass : pipelines->of) {
		if (pass.pipeline.is_valid()) {
			rd->free_rid(pass.pipeline);
		}
		if (pass.shader) {
			pass.shader->version_free(pass.version);
		}
	}
	for (RID &rid : pipelines->placeholders) {
		rd->free_rid(rid);
	}
	for (RID &rid : pipelines->past_last_mip) {
		rd->free_rid(rid);
	}
	rd->free_rid(pipelines->linear_clamp_sampler);
	memdelete(pipelines);
}

FrameGeneration::Context *FrameGeneration::context_create(Size2i p_render_size, Size2i p_display_size, RD::DataFormat p_color_format) {
	RenderingDevice *rd = RD::get_singleton();
	FfxSurfaceFormat color_format = fg_format_from_rd(p_color_format);
	ERR_FAIL_COND_V_MSG(color_format == FFX_SURFACE_FORMAT_UNKNOWN, nullptr, "Frame generation: the render target's format is not one it reads.");

	Context *ctx = memnew(Context);
	ctx->render_size = p_render_size;
	ctx->display_size = p_display_size;
	ctx->color_format = p_color_format;
	if (p_color_format == RD::DATA_FORMAT_R16G16B16A16_SFLOAT || p_color_format == RD::DATA_FORMAT_R32G32B32A32_SFLOAT) {
		ctx->transfer = FFX_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
		ctx->max_luminance = 80.0f;
	}
	ctx->backend.pipelines = pipelines;
	for (RID &ubo : ctx->backend.ubo_ring) {
		ubo = rd->uniform_buffer_create(FG_UBO_SIZE);
	}

	FfxInterface &fi = ctx->interface;
	fi.fpGetSDKVersion = fg_get_sdk_version;
	fi.fpGetEffectGpuMemoryUsage = fg_get_effect_gpu_memory_usage;
	fi.fpCreateBackendContext = fg_create_backend_context;
	fi.fpGetDeviceCapabilities = fg_get_device_capabilities;
	fi.fpDestroyBackendContext = fg_destroy_backend_context;
	fi.fpCreateResource = fg_create_resource;
	fi.fpRegisterResource = fg_register_resource;
	fi.fpGetResource = fg_get_resource;
	fi.fpUnregisterResources = fg_unregister_resources;
	fi.fpRegisterStaticResource = fg_register_static_resource;
	fi.fpGetResourceDescription = fg_get_resource_description;
	fi.fpDestroyResource = fg_destroy_resource;
	fi.fpMapResource = fg_map_resource;
	fi.fpUnmapResource = fg_unmap_resource;
	fi.fpStageConstantBufferDataFunc = fg_stage_constant_buffer_data;
	fi.fpCreatePipeline = fg_create_pipeline;
	fi.fpDestroyPipeline = fg_destroy_pipeline;
	fi.fpScheduleGpuJob = fg_schedule_gpu_job;
	fi.fpExecuteGpuJobs = fg_execute_gpu_jobs;
	fi.fpRegisterConstantBufferAllocator = nullptr;
	fi.scratchBuffer = &ctx->backend;
	fi.scratchBufferSize = sizeof(ctx->backend);
	fi.device = reinterpret_cast<FfxDevice>(pipelines);
	(void)fg_register_constant_buffer_allocator_unused;

	FfxOpticalflowContextDescription of_desc = {};
	of_desc.backendInterface = fi;
	of_desc.resolution = { uint32_t(p_display_size.x), uint32_t(p_display_size.y) };
	if (ffxOpticalflowContextCreate(&ctx->of, &of_desc) != FFX_OK) {
		ERR_PRINT("Frame generation: the optical flow context could not be made.");
		context_free(ctx);
		return nullptr;
	}
	ctx->of_made = true;

	FfxFrameInterpolationContextDescription fi_desc = {};
	fi_desc.backendInterface = fi;
	fi_desc.flags = FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;
	fi_desc.maxRenderSize = { uint32_t(p_render_size.x), uint32_t(p_render_size.y) };
	fi_desc.displaySize = { uint32_t(p_display_size.x), uint32_t(p_display_size.y) };
	fi_desc.backBufferFormat = color_format;
	fi_desc.previousInterpolationSourceFormat = color_format;
	if (ffxFrameInterpolationContextCreate(&ctx->fi, &fi_desc) != FFX_OK) {
		ERR_PRINT("Frame generation: the frame interpolation context could not be made.");
		context_free(ctx);
		return nullptr;
	}
	ctx->fi_made = true;

	// The resources the two effects share, as FSR 3 makes them.
	uint32_t shared = 0;
	fg_create_backend_context(&ctx->interface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &shared);
	ctx->shared_context = shared;
	FfxOpticalflowSharedResourceDescriptions of_shared = {};
	FfxFrameInterpolationSharedResourceDescriptions fi_shared = {};
	bool ok = ffxOpticalflowGetSharedResourceDescriptions(&ctx->of, &of_shared) == FFX_OK && ffxFrameInterpolationGetSharedResourceDescriptions(&ctx->fi, &fi_shared) == FFX_OK;
	ok = ok && fg_create_resource(&ctx->interface, &of_shared.opticalFlowVector, shared, &ctx->optical_flow_vector) == FFX_OK;
	ok = ok && fg_create_resource(&ctx->interface, &of_shared.opticalFlowSCD, shared, &ctx->optical_flow_scd) == FFX_OK;
	ok = ok && fg_create_resource(&ctx->interface, &fi_shared.dilatedDepth, shared, &ctx->dilated_depth) == FFX_OK;
	ok = ok && fg_create_resource(&ctx->interface, &fi_shared.dilatedMotionVectors, shared, &ctx->dilated_motion_vectors) == FFX_OK;
	ok = ok && fg_create_resource(&ctx->interface, &fi_shared.reconstructedPrevNearestDepth, shared, &ctx->reconstructed_prev_depth) == FFX_OK;
	if (!ok) {
		ERR_PRINT("Frame generation: its shared resources could not be made.");
		context_free(ctx);
		return nullptr;
	}

	RD::TextureFormat tf;
	tf.format = p_color_format;
	tf.width = p_display_size.x;
	tf.height = p_display_size.y;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	ctx->hudless = rd->texture_create(tf, RD::TextureView());
	rd->set_resource_name(ctx->hudless, "Frame generation: frame before its 2D");
	tf.usage_bits |= RD::TEXTURE_USAGE_STORAGE_BIT;
	ctx->output = rd->texture_create(tf, RD::TextureView());
	rd->set_resource_name(ctx->output, "Frame generation: generated frame");
	return ctx;
}

void FrameGeneration::context_free(Context *p_context) {
	if (!p_context) {
		return;
	}
	RenderingDevice *rd = RD::get_singleton();
	if (p_context->fi_made) {
		ffxFrameInterpolationContextDestroy(&p_context->fi);
	}
	if (p_context->of_made) {
		ffxOpticalflowContextDestroy(&p_context->of);
	}
	if (p_context->shared_context) {
		fg_destroy_backend_context(&p_context->interface, p_context->shared_context);
	}
	// Registrations first (their slices are of textures freed below), then what the backend made.
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t i = 0; i < p_context->backend.resources.size(); i++) {
			const FrameGenerationResource &res = p_context->backend.resources[i];
			if (res.rid.is_valid() && res.owned == (pass == 1)) {
				p_context->backend.remove(int32_t(i));
			}
		}
	}
	for (RID &ubo : p_context->backend.ubo_ring) {
		if (ubo.is_valid()) {
			rd->free_rid(ubo);
		}
	}
	if (p_context->hudless.is_valid()) {
		rd->free_rid(p_context->hudless);
	}
	if (p_context->output.is_valid()) {
		rd->free_rid(p_context->output);
	}
	memdelete(p_context);
}

void FrameGeneration::reset(Context *p_context) {
	ERR_FAIL_NULL(p_context);
	p_context->reset = true;
	p_context->has_previous = false;
}

void FrameGeneration::prepare(Context *p_context, RID p_depth, RID p_velocity, RID p_color, const Camera &p_camera) {
	ERR_FAIL_NULL(p_context);
	RenderingDevice *rd = RD::get_singleton();
	p_context->prepared = false;
	if (p_depth.is_null() || p_velocity.is_null() || p_color.is_null()) {
		p_context->reset = true;
		return;
	}

	RD::get_singleton()->draw_command_begin_label("Frame generation: prepare");
	rd->texture_copy(p_color, p_context->hudless, Vector3(), Vector3(), Vector3(p_context->display_size.x, p_context->display_size.y, 1), 0, 0, 0, 0);

	p_context->frame_id++;
	p_context->camera = p_camera;

	FfxFrameInterpolationPrepareDescription desc = {};
	desc.commandList = reinterpret_cast<FfxCommandList>(rd);
	desc.renderSize = { uint32_t(p_context->render_size.x), uint32_t(p_context->render_size.y) };
	desc.jitterOffset = { p_camera.jitter.x, p_camera.jitter.y };
	// Godot's motion vectors are in UV units; the SDK divides this by the render size.
	desc.motionVectorScale = { float(p_context->render_size.x), float(p_context->render_size.y) };
	desc.frameTimeDelta = p_camera.delta_ms;
	desc.cameraNear = p_camera.z_near;
	desc.cameraFar = p_camera.z_far;
	desc.viewSpaceToMetersFactor = 1.0f;
	desc.cameraFovAngleVertical = p_camera.fov_y;
	desc.depth = fg_resource(p_depth, L"depth");
	desc.motionVectors = fg_resource(p_velocity, L"velocity");
	desc.frameID = p_context->frame_id;
	desc.dilatedDepth = fg_get_resource(&p_context->interface, p_context->dilated_depth);
	desc.dilatedMotionVectors = fg_get_resource(&p_context->interface, p_context->dilated_motion_vectors);
	desc.reconstructedPrevDepth = fg_get_resource(&p_context->interface, p_context->reconstructed_prev_depth);

	FfxErrorCode err = ffxFrameInterpolationPrepare(&p_context->fi, &desc);
	RD::get_singleton()->draw_command_end_label();
	ERR_FAIL_COND_MSG(err != FFX_OK, vformat("Frame generation: prepare failed (%d).", int(err)));
	p_context->prepared = true;
}

RID FrameGeneration::generate(Context *p_context, RID p_color) {
	ERR_FAIL_NULL_V(p_context, RID());
	if (!p_context->prepared || p_color.is_null()) {
		p_context->reset = true;
		p_context->has_previous = false;
		return RID();
	}
	RenderingDevice *rd = RD::get_singleton();
	rd->draw_command_begin_label("Frame generation: generate");

	const bool reset = p_context->reset;
	FfxResource color = fg_resource(p_color, L"present color");
	FfxResource hudless = fg_resource(p_context->hudless, L"hudless color");

	FfxOpticalflowDispatchDescription of = {};
	of.commandList = reinterpret_cast<FfxCommandList>(rd);
	of.color = hudless;
	of.opticalFlowVector = fg_get_resource(&p_context->interface, p_context->optical_flow_vector);
	of.opticalFlowSCD = fg_get_resource(&p_context->interface, p_context->optical_flow_scd);
	of.reset = reset;
	of.backbufferTransferFunction = p_context->transfer;
	of.minMaxLuminance = { 0.0f, p_context->max_luminance };
	FfxErrorCode err = ffxOpticalflowContextDispatch(&p_context->of, &of);

	FfxFrameInterpolationDispatchDescription fi = {};
	fi.commandList = reinterpret_cast<FfxCommandList>(rd);
	fi.displaySize = { uint32_t(p_context->display_size.x), uint32_t(p_context->display_size.y) };
	fi.renderSize = { uint32_t(p_context->render_size.x), uint32_t(p_context->render_size.y) };
	fi.currentBackBuffer = color;
	fi.currentBackBuffer_HUDLess = hudless;
	fi.output = fg_resource(p_context->output, L"generated frame");
	fi.interpolationRect = { 0, 0, p_context->display_size.x, p_context->display_size.y };
	fi.opticalFlowVector = of.opticalFlowVector;
	fi.opticalFlowSceneChangeDetection = of.opticalFlowSCD;
	fi.opticalFlowBufferSize = { uint32_t(p_context->display_size.x / 8), uint32_t(p_context->display_size.y / 8) };
	fi.opticalFlowScale = { 1.0f / p_context->display_size.x, 1.0f / p_context->display_size.y };
	fi.opticalFlowBlockSize = 8;
	fi.cameraNear = p_context->camera.z_near;
	fi.cameraFar = p_context->camera.z_far;
	fi.cameraFovAngleVertical = p_context->camera.fov_y;
	fi.viewSpaceToMetersFactor = 1.0f;
	fi.frameTimeDelta = p_context->camera.delta_ms;
	fi.reset = reset;
	fi.backBufferTransferFunction = p_context->transfer;
	fi.minMaxLuminance[0] = 0.0f;
	fi.minMaxLuminance[1] = p_context->max_luminance;
	fi.frameID = p_context->frame_id;
	fi.dilatedDepth = fg_get_resource(&p_context->interface, p_context->dilated_depth);
	fi.dilatedMotionVectors = fg_get_resource(&p_context->interface, p_context->dilated_motion_vectors);
	fi.reconstructedPrevDepth = fg_get_resource(&p_context->interface, p_context->reconstructed_prev_depth);
	if (err == FFX_OK) {
		err = ffxFrameInterpolationDispatch(&p_context->fi, &fi);
	}
	rd->draw_command_end_label();

	p_context->prepared = false;
	p_context->reset = false;
	const bool had_previous = p_context->has_previous;
	p_context->has_previous = err == FFX_OK;
	ERR_FAIL_COND_V_MSG(err != FFX_OK, RID(), vformat("Frame generation: generate failed (%d).", int(err)));
	// The first frame after a reset has nothing before it to meet halfway.
	return (had_previous && !reset) ? p_context->output : RID();
}
