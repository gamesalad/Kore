#pragma once

#include "MiniVulkan.h"

#include "named_number.h"

struct kinc_g5_shader;

typedef struct PipelineState5Impl_s {
	const char **textures;
	int *textureValues;
	int textureCount;

	VkPipeline framebuffer_pipeline;
	VkPipeline rendertarget_pipeline;
	VkShaderModule vert_shader_module;
	VkShaderModule frag_shader_module;

	// The shader reflection tables (KINC_INTERNAL_NAMED_NUMBER_COUNT entries each) live in
	// one heap block allocated by kinc_g5_pipeline_init, so the pipeline struct stays small:
	// bindings embed it by value in garbage-collected objects (Kha on hxcpp), and inline
	// tables made it ~42 KB.
	kinc_internal_named_number *vertexLocations;
	kinc_internal_named_number *fragmentLocations;
	kinc_internal_named_number *textureBindings;
	kinc_internal_named_number *vertexOffsets;
	kinc_internal_named_number *fragmentOffsets;

	VkPipelineLayout pipeline_layout;
} PipelineState5Impl;

typedef struct ComputePipelineState5Impl_t {
	int a;
} ComputePipelineState5Impl;

typedef struct {
	int vertexOffset;
	int fragmentOffset;
	int computeOffset;
} ConstantLocation5Impl;

typedef struct {
	int nothing;
} AttributeLocation5Impl;
