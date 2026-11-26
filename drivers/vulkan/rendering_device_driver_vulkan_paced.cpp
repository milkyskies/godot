/**************************************************************************/
/*  rendering_device_driver_vulkan_paced.cpp                              */
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

// Paced presents for frame generation (hacco #1441). A frame with a generated picture shows two: the generated one
// as soon as the frame's work is done, the real one half a real frame later. Presenting both from the render thread
// would either show them back to back (no pacing) or hold that thread through the wait, so a thread of its own owns
// the swap chain while frame generation runs: it acquires, blits the frame's textures in and presents, on the same
// queue as the frames under that queue's lock.

#include "rendering_device_driver_vulkan.h"

#include "core/os/os.h"
#include "drivers/vulkan/vulkan_hooks.h"

// How often a pacer reports what it showed (shown and generated frames a second, presents that came late).
static constexpr uint64_t FRAME_PACER_REPORT_USEC = 5000000;

bool RenderingDeviceDriverVulkan::swap_chain_supports_paced_present(SwapChainID p_swap_chain) {
	SwapChain *swap_chain = (SwapChain *)(p_swap_chain.id);
	return swap_chain->vk_swapchain != VK_NULL_HANDLE && swap_chain->can_blit_to;
}

bool RenderingDeviceDriverVulkan::swap_chain_paced_is_running(SwapChainID p_swap_chain) {
	return ((SwapChain *)(p_swap_chain.id))->pacer != nullptr;
}

bool RenderingDeviceDriverVulkan::swap_chain_paced_needs_resize(SwapChainID p_swap_chain) {
	SwapChain *swap_chain = (SwapChain *)(p_swap_chain.id);
	if (swap_chain->vk_swapchain != VK_NULL_HANDLE && _paced_images_short(swap_chain)) {
		return true;
	}
	if (swap_chain->pacer == nullptr) {
		return false;
	}
	MutexLock lock(swap_chain->pacer->mutex);
	return swap_chain->pacer->needs_resize || context_driver->surface_get_needs_resize(swap_chain->surface);
}

bool RenderingDeviceDriverVulkan::_paced_images_short(SwapChain *p_swap_chain) {
	// Paced presents hold two images at once: with fewer than the surface's minimum plus two, a second acquire could
	// wait forever, so the swap chain is made again with more.
	if (p_swap_chain->images.size() < p_swap_chain->min_image_count + 2 && !p_swap_chain->paced_images) {
		p_swap_chain->paced_images = true;
		return true;
	}
	return false;
}

void RenderingDeviceDriverVulkan::swap_chain_paced_wait(SwapChainID p_swap_chain, uint32_t p_max_pending) {
	FramePacer *pacer = ((SwapChain *)(p_swap_chain.id))->pacer;
	if (pacer == nullptr) {
		return;
	}
	MutexLock lock(pacer->mutex);
	while (pacer->pending > p_max_pending) {
		pacer->cond.wait(lock);
	}
}

void RenderingDeviceDriverVulkan::swap_chain_paced_stop(SwapChainID p_swap_chain) {
	_frame_pacer_stop((SwapChain *)(p_swap_chain.id));
}

Error RenderingDeviceDriverVulkan::swap_chain_present_paced(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, TextureID p_real, TextureID p_generated, SemaphoreID p_ready) {
	SwapChain *swap_chain = (SwapChain *)(p_swap_chain.id);
	CommandQueue *command_queue = (CommandQueue *)(p_cmd_queue.id);
	ERR_FAIL_COND_V(!swap_chain_supports_paced_present(p_swap_chain), ERR_UNAVAILABLE);
	ERR_FAIL_COND_V(p_real.id == 0 || p_ready.id == 0, ERR_INVALID_PARAMETER);

	if (swap_chain->pacer == nullptr) {
		FramePacer *pacer = memnew(FramePacer);
		pacer->driver = this;
		pacer->swap_chain = swap_chain;
		pacer->queue = &queue_families[command_queue->queue_family][command_queue->queue_index];

		VkCommandPoolCreateInfo pool_info = {};
		pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool_info.queueFamilyIndex = command_queue->queue_family;
		VkResult err = vkCreateCommandPool(vk_device, &pool_info, VKC::get_allocation_callbacks(VK_OBJECT_TYPE_COMMAND_POOL), &pacer->command_pool);
		ERR_FAIL_COND_V_MSG(err != VK_SUCCESS, ERR_CANT_CREATE, vformat("Frame pacing: no command pool (VkResult %d).", err));

		VkCommandBufferAllocateInfo buffer_info = {};
		buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		buffer_info.commandPool = pacer->command_pool;
		buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		buffer_info.commandBufferCount = 2;
		err = vkAllocateCommandBuffers(vk_device, &buffer_info, pacer->command_buffers);
		ERR_FAIL_COND_V_MSG(err != VK_SUCCESS, ERR_CANT_CREATE, vformat("Frame pacing: no command buffers (VkResult %d).", err));

		for (uint32_t i = 0; i < 2; i++) {
			VkFenceCreateInfo fence_info = {};
			fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			vkCreateFence(vk_device, &fence_info, VKC::get_allocation_callbacks(VK_OBJECT_TYPE_FENCE), &pacer->fences[i]);
			VkSemaphoreCreateInfo semaphore_info = {};
			semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			vkCreateSemaphore(vk_device, &semaphore_info, VKC::get_allocation_callbacks(VK_OBJECT_TYPE_SEMAPHORE), &pacer->acquired[i]);
		}

		pacer->stats_since_usec = OS::get_singleton()->get_ticks_usec();
		swap_chain->pacer = pacer;
		pacer->thread.start(_frame_pacer_thread, pacer);
		print_verbose("Frame pacing: started for frame generation.");
	}

	FramePacer *pacer = swap_chain->pacer;
	const TextureInfo *real = (const TextureInfo *)(p_real.id);
	const TextureInfo *generated = p_generated.id ? (const TextureInfo *)(p_generated.id) : nullptr;

	FramePacer::Job job;
	job.real = real->vk_image;
	job.generated = generated ? generated->vk_image : VK_NULL_HANDLE;
	job.size = { real->vk_create_info.extent.width, real->vk_create_info.extent.height };
	job.ready = VkSemaphore(p_ready.id);

	MutexLock lock(pacer->mutex);
	pacer->jobs.push_back(job);
	pacer->pending++;
	pacer->cond.notify_all();
	return OK;
}

void RenderingDeviceDriverVulkan::_frame_pacer_stop(SwapChain *p_swap_chain) {
	FramePacer *pacer = p_swap_chain->pacer;
	if (pacer == nullptr) {
		return;
	}
	{
		// What is queued is shown first: each frame's ready semaphore is waited on once, as its signal needs.
		MutexLock lock(pacer->mutex);
		pacer->exit = true;
		pacer->cond.notify_all();
	}
	pacer->thread.wait_to_finish();

	// Every submit of the thread was waited on before it went on, so nothing of its is in flight.
	for (uint32_t i = 0; i < 2; i++) {
		vkDestroyFence(vk_device, pacer->fences[i], VKC::get_allocation_callbacks(VK_OBJECT_TYPE_FENCE));
		vkDestroySemaphore(vk_device, pacer->acquired[i], VKC::get_allocation_callbacks(VK_OBJECT_TYPE_SEMAPHORE));
	}
	vkDestroyCommandPool(vk_device, pacer->command_pool, VKC::get_allocation_callbacks(VK_OBJECT_TYPE_COMMAND_POOL));
	p_swap_chain->pacer = nullptr;
	memdelete(pacer);
	print_verbose("Frame pacing: stopped.");
}

void RenderingDeviceDriverVulkan::_frame_pacer_thread(void *p_pacer) {
	FramePacer *pacer = (FramePacer *)p_pacer;
	RenderingDeviceDriverVulkan *driver = pacer->driver;
	OS *os = OS::get_singleton();

	while (true) {
		FramePacer::Job job;
		{
			MutexLock lock(pacer->mutex);
			while (pacer->jobs.is_empty() && !pacer->exit) {
				pacer->cond.wait(lock);
			}
			if (pacer->jobs.is_empty()) {
				break; // Exit with nothing left to show.
			}
			job = pacer->jobs[0];
			pacer->jobs.remove_at(0);
		}

		driver->_frame_pacer_show(pacer, job);
		if (job.generated != VK_NULL_HANDLE) {
			pacer->generated++;
			pacer->shown += 2;
		} else {
			pacer->shown++;
		}

		uint64_t now = os->get_ticks_usec();
		if (now - pacer->stats_since_usec >= FRAME_PACER_REPORT_USEC) {
			double seconds = double(now - pacer->stats_since_usec) / 1000000.0;
			print_line(vformat("Frame generation: shown %.1f fps, real %.1f, generated %.1f, real frame every %.2f ms, %d presents late.",
					pacer->shown / seconds, (pacer->shown - pacer->generated) / seconds, pacer->generated / seconds, pacer->interval_usec / 1000.0, pacer->late));
			pacer->stats_since_usec = now;
			pacer->shown = 0;
			pacer->generated = 0;
			pacer->late = 0;
		}

		MutexLock lock(pacer->mutex);
		pacer->pending--;
		pacer->cond.notify_all();
	}
}

void RenderingDeviceDriverVulkan::_frame_pacer_show(FramePacer *p_pacer, const FramePacer::Job &p_job) {
	SwapChain *swap_chain = p_pacer->swap_chain;
	OS *os = OS::get_singleton();

	// Both pictures go into their images in one submit, as soon as the frame is done: a copy submitted later would
	// wait behind the next frame's work on the queue and show late. So both images are acquired first (the swap chain
	// has two more than the surface's minimum while paced, `swap_chain_paced_needs_resize`).
	struct Shown {
		VkImage from = VK_NULL_HANDLE;
		uint32_t index = 0;
		bool acquired = false;
	};
	// A surface whose most images leave no two spare shows the real frames only.
	const bool both = p_job.generated != VK_NULL_HANDLE && swap_chain->images.size() >= swap_chain->min_image_count + 2;
	Shown shown[2];
	shown[0].from = both ? p_job.generated : p_job.real;
	shown[1].from = both ? p_job.real : VK_NULL_HANDLE;
	const uint32_t count = shown[1].from != VK_NULL_HANDLE ? 2 : 1;

	bool out_of_date;
	{
		MutexLock lock(p_pacer->mutex);
		out_of_date = p_pacer->needs_resize;
	}
	out_of_date = out_of_date || context_driver->surface_get_needs_resize(swap_chain->surface);
	for (uint32_t i = 0; i < count && !out_of_date; i++) {
		VkResult err = device_functions.AcquireNextImageKHR(vk_device, swap_chain->vk_swapchain, UINT64_MAX, p_pacer->acquired[i], VK_NULL_HANDLE, &shown[i].index);
		if (err == VK_SUCCESS || err == VK_SUBOPTIMAL_KHR) {
			shown[i].acquired = true;
		} else {
			// Out of date leaves the semaphore unsignaled, so it stays usable.
			out_of_date = true;
		}
	}
	if (out_of_date) {
		MutexLock lock(p_pacer->mutex);
		p_pacer->needs_resize = true;
	}

	VkCommandBuffer cmd = p_pacer->command_buffers[0];
	VkFence fence = p_pacer->fences[0];
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin = {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);
	for (uint32_t i = 0; i < count; i++) {
		if (!shown[i].acquired) {
			continue;
		}
		VkImage target = swap_chain->images[shown[i].index];
		VkImageMemoryBarrier barriers[2] = {};
		for (VkImageMemoryBarrier &b : barriers) {
			b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		}
		// RenderingDevice left the texture as the destination of its copy into it.
		barriers[0].image = shown[i].from;
		barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barriers[1].image = target;
		barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barriers[1].srcAccessMask = 0;
		barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

		VkImageBlit blit = {};
		blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		blit.srcOffsets[1] = { int32_t(p_job.size.width), int32_t(p_job.size.height), 1 };
		blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		blit.dstOffsets[1] = { int32_t(swap_chain->extent.width), int32_t(swap_chain->extent.height), 1 };
		bool same_size = p_job.size.width == swap_chain->extent.width && p_job.size.height == swap_chain->extent.height;
		vkCmdBlitImage(cmd, shown[i].from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, same_size ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);

		// The texture goes back to how RenderingDevice knows it; the image to the presentation engine.
		barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barriers[1].dstAccessMask = 0;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
	}
	vkEndCommandBuffer(cmd);

	// Even when nothing can be shown the frame's ready semaphore is waited on, so it can be signaled again.
	VkSemaphore waits[3];
	VkPipelineStageFlags stages[3] = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
	uint32_t wait_count = 0;
	waits[wait_count++] = p_job.ready;
	VkSemaphore signals[2];
	uint32_t signal_count = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (shown[i].acquired) {
			waits[wait_count++] = p_pacer->acquired[i];
			signals[signal_count++] = swap_chain->present_semaphores[shown[i].index];
		}
	}

	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = wait_count;
	submit.pWaitSemaphores = waits;
	submit.pWaitDstStageMask = stages;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	submit.signalSemaphoreCount = signal_count;
	submit.pSignalSemaphores = signals;

	vkResetFences(vk_device, 1, &fence);
	p_pacer->queue->submit_mutex.lock();
	VkResult err = vkQueueSubmit(p_pacer->queue->queue, 1, &submit, fence);
	p_pacer->queue->submit_mutex.unlock();
	ERR_FAIL_COND_MSG(err != VK_SUCCESS, vformat("Frame pacing: submit failed (VkResult %d).", err));

	// Done means the frame's own work is done too: its pictures are ready to be seen.
	vkWaitForFences(vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
	const uint64_t done = os->get_ticks_usec();
	if (count == 2) {
		if (p_pacer->last_done_usec != 0) {
			// A long gap (a hitch, a loading screen) does not stretch the pacing of the frames after it.
			double since = CLAMP(double(done - p_pacer->last_done_usec), 2000.0, 100000.0);
			p_pacer->interval_usec = p_pacer->interval_usec * 0.9 + since * 0.1;
		}
		p_pacer->last_done_usec = done;
	} else {
		p_pacer->last_done_usec = 0;
	}

	// The generated picture now, the real one half a real frame later.
	for (uint32_t i = 0; i < count; i++) {
		if (!shown[i].acquired) {
			continue;
		}
		if (i == 1) {
			const uint64_t at = done + uint64_t(p_pacer->interval_usec * 0.5);
			uint64_t now = os->get_ticks_usec();
			if (now + 1500 < at) {
				os->delay_usec(at - now - 1000);
			}
			while (os->get_ticks_usec() < at) {
				// The last millisecond spun: a sleep oversleeps by about that much.
			}
			if (os->get_ticks_usec() > at + 2000) {
				p_pacer->late++;
			}
		}
		VkSemaphore present_semaphore = swap_chain->present_semaphores[shown[i].index];
		VkPresentInfoKHR present = {};
		present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		present.waitSemaphoreCount = 1;
		present.pWaitSemaphores = &present_semaphore;
		present.swapchainCount = 1;
		present.pSwapchains = &swap_chain->vk_swapchain;
		present.pImageIndices = &shown[i].index;
		p_pacer->queue->submit_mutex.lock();
		err = device_functions.QueuePresentKHR(p_pacer->queue->queue, &present);
		p_pacer->queue->submit_mutex.unlock();
		if (err == VK_ERROR_OUT_OF_DATE_KHR) {
			MutexLock lock(p_pacer->mutex);
			p_pacer->needs_resize = true;
		}
	}
}
