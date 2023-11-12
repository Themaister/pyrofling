#include "device.hpp"
#include "context.hpp"

using namespace Vulkan;

int main()
{
	if (!Vulkan::Context::init_loader(nullptr))
		return EXIT_FAILURE;

	Context ctx;
	if (!ctx.init_instance_and_device(nullptr, 0, nullptr, 0,
	                                  CONTEXT_CREATION_ENABLE_VIDEO_ENCODE_BIT |
	                                  CONTEXT_CREATION_ENABLE_VIDEO_H264_BIT | CONTEXT_CREATION_ENABLE_VIDEO_H265_BIT))
		return EXIT_FAILURE;

	Device dev;
	dev.set_context(ctx);

	if (!dev.get_device_features().supports_video_encode_h264)
		return EXIT_FAILURE;

	uint32_t count;

	VkPhysicalDeviceVideoFormatInfoKHR format_info = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR };
	format_info.imageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;

	VkVideoProfileInfoKHR profile_info = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR };
	profile_info.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	profile_info.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile_info.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile_info.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT;

	VkVideoEncodeH264ProfileInfoEXT h264_profile = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_EXT };
	h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
	profile_info.pNext = &h264_profile;

	VkVideoProfileListInfoKHR profile_list = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR };
	profile_list.pProfiles = &profile_info;
	profile_list.profileCount = 1;
	format_info.pNext = &profile_list;

	auto res = vkGetPhysicalDeviceVideoFormatPropertiesKHR(dev.get_physical_device(), &format_info, &count, nullptr);
	std::vector<VkVideoFormatPropertiesKHR> props(count);
	for (auto &p : props)
		p.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
	vkGetPhysicalDeviceVideoFormatPropertiesKHR(dev.get_physical_device(), &format_info, &count, props.data());

	VkFormat fmt = props.front().format;

	VkFormatProperties3 props3 = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 };
	dev.get_format_properties(fmt, &props3);

	if ((props3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_VIDEO_ENCODE_INPUT_BIT_KHR) == 0)
		return EXIT_FAILURE;
	if ((props3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_VIDEO_ENCODE_DPB_BIT_KHR) == 0)
		return EXIT_FAILURE;

	VkImageFormatProperties2 props2 = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
	dev.get_image_format_properties(fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
									VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR, 0,
									&profile_list, &props2);

	if (props2.imageFormatProperties.maxArrayLayers < 16)
		return EXIT_FAILURE;
	if (props2.imageFormatProperties.maxExtent.width < 1920)
		return EXIT_FAILURE;
	if (props2.imageFormatProperties.maxExtent.height < 1080)
		return EXIT_FAILURE;

	VkVideoCapabilitiesKHR video_caps = { VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR };
	VkVideoEncodeCapabilitiesKHR encode_caps = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR };
	VkVideoEncodeH264CapabilitiesEXT h264_encode_caps = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_EXT };
	video_caps.pNext = &encode_caps;
	encode_caps.pNext = &h264_encode_caps;
	vkGetPhysicalDeviceVideoCapabilitiesKHR(dev.get_physical_device(), &profile_info, &video_caps);

	if (1920 < video_caps.minCodedExtent.width || 1080 < video_caps.minCodedExtent.height ||
	    1920 > video_caps.maxCodedExtent.width || 1080 > video_caps.maxCodedExtent.height)
	{
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}