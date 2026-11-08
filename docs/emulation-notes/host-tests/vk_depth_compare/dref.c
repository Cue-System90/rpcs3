// Vulkan test: does the driver clamp Dref to [0,1] for depth compare, per format?
// Image cleared to depth 1.0, compare op LESS_OR_EQUAL (result = Dref <= texel).
// usage: dref <spv> [clear_depth]
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)

static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q; static uint32_t qf;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
	fprintf(stderr, "no memory type\n"); exit(1);
}

static void run(VkFormat fmt, const char* name, const uint32_t* spv, size_t spv_size, float clear)
{
	VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, fmt, &fp);
	if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { printf("%-22s not supported\n", name); return; }
	int has_stencil = fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;

	VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, fmt, {4, 4, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
		VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED };
	VkImage img; CK(vkCreateImage(dev, &ici, 0, &img));
	VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory imem; CK(vkAllocateMemory(dev, &mai, 0, &imem)); CK(vkBindImageMemory(dev, img, imem, 0));

	VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, img, VK_IMAGE_VIEW_TYPE_2D, fmt, {0}, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1} };
	VkImageView view; CK(vkCreateImageView(dev, &vci, 0, &view));

	VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		0.f, VK_FALSE, 1.f, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL, 0.f, 0.f, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, VK_FALSE };
	VkSampler s_white, s_black; CK(vkCreateSampler(dev, &sci, 0, &s_white));
	sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; CK(vkCreateSampler(dev, &sci, 0, &s_black));

	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 96 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0 };
	VkBuffer buf; CK(vkCreateBuffer(dev, &bci, 0, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory bmem; CK(vkAllocateMemory(dev, &mai, 0, &bmem)); CK(vkBindBufferMemory(dev, buf, bmem, 0));

	VkDescriptorSetLayoutBinding b[3] = {
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0},
		{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0},
		{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0} };
	VkDescriptorSetLayoutCreateInfo dlci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 3, b };
	VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &dlci, 0, &dsl));
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 0, 0 };
	VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &plci, 0, &pl));
	VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, spv_size, spv };
	VkShaderModule sm; CK(vkCreateShaderModule(dev, &smci, 0, &sm));
	VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0,
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", 0 }, pl, 0, 0 };
	VkPipeline pipe; CK(vkCreateComputePipelines(dev, 0, 1, &cpci, 0, &pipe));

	VkDescriptorPoolSize ps[2] = { {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1} };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 2, ps };
	VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpci, 0, &dp));
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 1, &dsl };
	VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dsai, &ds));
	VkDescriptorImageInfo ii[2] = { {s_white, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {s_black, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL} };
	VkDescriptorBufferInfo bi = { buf, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet w[3] = {
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[0], 0, 0 },
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[1], 0, 0 },
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &bi, 0 } };
	vkUpdateDescriptorSets(dev, 3, w, 0, 0);

	VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, qf };
	VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpi, 0, &cp));
	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
	VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cbai, &cb));
	VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 0 };
	CK(vkBeginCommandBuffer(cb, &cbbi));
	VkImageAspectFlags asp = VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
	VkImageMemoryBarrier imb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {asp, 0, 1, 0, 1} };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &imb);
	VkClearDepthStencilValue cv = { clear, 0 };
	VkImageSubresourceRange rng = { asp, 0, 1, 0, 1 };
	vkCmdClearDepthStencilImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rng);
	imb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; imb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, 0, 0, 0, 1, &imb);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, 0);
	vkCmdDispatch(cb, 1, 1, 1);
	CK(vkEndCommandBuffer(cb));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb, 0, 0 };
	CK(vkQueueSubmit(q, 1, &si, 0)); CK(vkQueueWaitIdle(q));

	float* out; CK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void**)&out));
	const char* rows[3] = { "inside", "border white", "border black" };
	for (int r = 0; r < 3; r++)
	{
		printf("%-22s %-13s", name, rows[r]);
		for (int i = 0; i < 32; i += 2) printf(" %4.2f:%d", -0.5 + 0.125 * i, (int)(out[r * 32 + i] + 0.5f));
		printf("\n");
	}
	vkUnmapMemory(dev, bmem);
	vkDeviceWaitIdle(dev);
	vkDestroyCommandPool(dev, cp, 0); vkDestroyDescriptorPool(dev, dp, 0); vkDestroyPipeline(dev, pipe, 0); vkDestroyShaderModule(dev, sm, 0);
	vkDestroyPipelineLayout(dev, pl, 0); vkDestroyDescriptorSetLayout(dev, dsl, 0); vkDestroyBuffer(dev, buf, 0); vkFreeMemory(dev, bmem, 0);
	vkDestroySampler(dev, s_white, 0); vkDestroySampler(dev, s_black, 0); vkDestroyImageView(dev, view, 0); vkDestroyImage(dev, img, 0); vkFreeMemory(dev, imem, 0);
}

int main(int argc, char** argv)
{
	FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint32_t* spv = malloc(n); fread(spv, 1, n, f); fclose(f);
	float clear = argc > 2 ? strtof(argv[2], 0) : 1.0f;

	VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "dref", 1, 0, 0, VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai, 0, 0, 0, 0 };
	VkInstance inst; CK(vkCreateInstance(&ici, 0, &inst));
	uint32_t c = 8; VkPhysicalDevice pds[8]; CK(vkEnumeratePhysicalDevices(inst, &c, pds));
	pd = 0;
	for (uint32_t i = 0; i < c; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { pd = pds[i]; printf("GPU: %s, driver 0x%x\n", p.deviceName, p.driverVersion); break; } }
	if (!pd) { fprintf(stderr, "no discrete GPU\n"); return 1; }
	uint32_t nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
	for (qf = 0; qf < nq && !(qp[qf].queueFlags & VK_QUEUE_COMPUTE_BIT); qf++) {}
	float prio = 1.f;
	VkDeviceQueueCreateInfo dqci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, qf, 1, &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &dqci, 0, 0, 0, 0, 0 };
	CK(vkCreateDevice(pd, &dci, 0, &dev)); vkGetDeviceQueue(dev, qf, 0, &q);
	printf("clear depth = %.2f, compare LESS_OR_EQUAL, columns ref:result\n", clear);

	run(VK_FORMAT_D16_UNORM, "D16_UNORM", spv, n, clear);
	run(VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT", spv, n, clear);
	run(VK_FORMAT_D32_SFLOAT, "D32_SFLOAT", spv, n, clear);
	run(VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT", spv, n, clear);

	vkDestroyDevice(dev, 0); vkDestroyInstance(inst, 0);
	return 0;
}
