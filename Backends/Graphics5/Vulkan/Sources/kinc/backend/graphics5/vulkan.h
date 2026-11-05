#pragma once

#include <stdbool.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

struct vk_funs {
	PFN_vkGetPhysicalDeviceSurfaceSupportKHR fpGetPhysicalDeviceSurfaceSupportKHR;
	PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR fpGetPhysicalDeviceSurfaceCapabilitiesKHR;
	PFN_vkGetPhysicalDeviceSurfaceFormatsKHR fpGetPhysicalDeviceSurfaceFormatsKHR;
	PFN_vkGetPhysicalDeviceSurfacePresentModesKHR fpGetPhysicalDeviceSurfacePresentModesKHR;
	PFN_vkCreateSwapchainKHR fpCreateSwapchainKHR;
	PFN_vkDestroySwapchainKHR fpDestroySwapchainKHR;
	PFN_vkGetSwapchainImagesKHR fpGetSwapchainImagesKHR;
	PFN_vkDestroySurfaceKHR fpDestroySurfaceKHR;

	PFN_vkCreateDebugUtilsMessengerEXT fpCreateDebugUtilsMessengerEXT;
	PFN_vkDestroyDebugUtilsMessengerEXT fpDestroyDebugUtilsMessengerEXT;

	PFN_vkQueuePresentKHR fpQueuePresentKHR;
	PFN_vkAcquireNextImageKHR fpAcquireNextImageKHR;
};

struct vk_depth {
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
};

struct vk_window {
	int width;
	int height;

	bool resized;
	bool surface_destroyed;

	int depth_bits;
	int stencil_bits;

	bool vsynced;

	uint32_t current_image;

	VkSurfaceKHR surface;
	VkSurfaceFormatKHR format;

	VkSwapchainKHR swapchain;
	uint32_t image_count;
	VkImage *images;
	VkImageView *views;
	VkFramebuffer *framebuffers;

	// One semaphore per swapchain image, signaled for and waited on by the present of that image.
	// Acquiring an image again means its previous present has consumed the semaphore.
	VkSemaphore *present_semaphores;
	uint32_t present_semaphore_count;

	VkRenderPass framebuffer_render_pass;      // first pass on an acquired image: its contents are discarded
	VkRenderPass framebuffer_render_pass_load; // every later pass on that image: its contents are loaded
	VkRenderPass rendertarget_render_pass;
	VkRenderPass rendertarget_render_pass_with_depth;

	struct vk_depth depth;
};

#define MAXIMUM_WINDOWS 16

struct vk_context {
	VkInstance instance;
	VkPhysicalDevice gpu;
	VkDevice device;
	VkPhysicalDeviceMemoryProperties memory_properties;

	VkCommandBuffer setup_cmd;
	VkCommandPool cmd_pool;
	VkQueue queue;

	struct vk_window windows[MAXIMUM_WINDOWS];

	// buffer hack
	VkBuffer *vertex_uniform_buffer;
	VkBuffer *fragment_uniform_buffer;
	VkBuffer *compute_uniform_buffer;

	int current_window;

#ifdef VALIDATE
	bool validation_found;
	VkDebugUtilsMessengerEXT debug_messenger;
#endif
};

extern struct vk_funs vk;
extern struct vk_context vk_ctx;

extern void flush_init_cmd(void);

// Objects a command list may still use are destroyed only after the GPU is done with them.
void kinc_vulkan_internal_retire(VkFramebuffer framebuffer, VkImageView view, VkImage image, VkDeviceMemory memory);
void kinc_vulkan_internal_retired_flush(void);
void kinc_vulkan_internal_forget_image_view(VkImageView view);
extern void reuse_descriptor_sets(void);
extern void reuse_compute_descriptor_sets(void);

#include <assert.h>
