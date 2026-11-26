// Godot (hacco #1441): Godot's FSR 2 copy (thirdparty/amd-fsr2) declares these with the same names and other
// layouts. Included first in every file that reads this SDK, so the two never meet as one type (C++'s one
// definition rule, which link-time optimization relies on). Generated from the shared names; keep it in sync.
#pragma once

#define FfxClearFloatJobDescription FfxFg_ClearFloatJobDescription
#define FfxComputeJobDescription FfxFg_ComputeJobDescription
#define FfxConstantBuffer FfxFg_ConstantBuffer
#define FfxCopyJobDescription FfxFg_CopyJobDescription
#define FfxCreateResourceDescription FfxFg_CreateResourceDescription
#define FfxDeviceCapabilities FfxFg_DeviceCapabilities
#define FfxDimensions2D FfxFg_Dimensions2D
#define FfxFilterType FfxFg_FilterType
#define FfxFloatCoords2D FfxFg_FloatCoords2D
#define FfxGpuJobDescription FfxFg_GpuJobDescription
#define FfxGpuJobType FfxFg_GpuJobType
#define FfxHeapType FfxFg_HeapType
#define FfxIntCoords2D FfxFg_IntCoords2D
#define FfxPipelineDescription FfxFg_PipelineDescription
#define FfxPipelineState FfxFg_PipelineState
#define FfxResource FfxFg_Resource
#define FfxResourceBinding FfxFg_ResourceBinding
#define FfxResourceDescription FfxFg_ResourceDescription
#define FfxResourceDimension FfxFg_ResourceDimension
#define FfxResourceFlags FfxFg_ResourceFlags
#define FfxResourceInternal FfxFg_ResourceInternal
#define FfxResourceStates FfxFg_ResourceStates
#define FfxResourceType FfxFg_ResourceType
#define FfxResourceUsage FfxFg_ResourceUsage
#define FfxResourceViewType FfxFg_ResourceViewType
#define FfxShaderModel FfxFg_ShaderModel
#define FfxSurfaceFormat FfxFg_SurfaceFormat
