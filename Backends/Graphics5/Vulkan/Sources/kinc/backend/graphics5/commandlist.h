#pragma once

#include "MiniVulkan.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	int _indexCount;
	VkCommandBuffer _buffer;
	VkFence fence;
	int index;            // slot in the retired-object bookkeeping, -1 if none
	bool recording;       // begun and not yet executed
	uint64_t last_submit; // number of the queue submission it was last executed in
} CommandList5Impl;
