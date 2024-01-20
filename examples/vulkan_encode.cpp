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

	// Query supported formats.
	VkPhysicalDeviceVideoFormatInfoKHR format_info = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR };
	format_info.imageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;

	VkVideoProfileInfoKHR profile_info = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR };
	profile_info.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	profile_info.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile_info.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile_info.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;

	VkVideoEncodeH264ProfileInfoKHR h264_profile = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR };
	h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
	profile_info.pNext = &h264_profile;

	VkVideoProfileListInfoKHR profile_list = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR };
	profile_list.pProfiles = &profile_info;
	profile_list.profileCount = 1;
	format_info.pNext = &profile_list;

	vkGetPhysicalDeviceVideoFormatPropertiesKHR(dev.get_physical_device(), &format_info, &count, nullptr);
	Util::SmallVector<VkVideoFormatPropertiesKHR> props(count);
	for (auto &p : props)
		p.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
	vkGetPhysicalDeviceVideoFormatPropertiesKHR(dev.get_physical_device(), &format_info, &count, props.data());

	VkFormat fmt = props.front().format;
	///

	// Sanity check
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
	///

	// Query encoder caps.
	VkVideoCapabilitiesKHR video_caps = { VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR };
	VkVideoEncodeCapabilitiesKHR encode_caps = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR };
	VkVideoEncodeH264CapabilitiesKHR h264_encode_caps = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR };
	video_caps.pNext = &encode_caps;
	encode_caps.pNext = &h264_encode_caps;
	vkGetPhysicalDeviceVideoCapabilitiesKHR(dev.get_physical_device(), &profile_info, &video_caps);

	if (1920 < video_caps.minCodedExtent.width || 1080 < video_caps.minCodedExtent.height ||
	    1920 > video_caps.maxCodedExtent.width || 1080 > video_caps.maxCodedExtent.height)
	{
		return EXIT_FAILURE;
	}
	///

	// Create DPB layers and input image
	ImageCreateInfo dpb_info;
	dpb_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	dpb_info.width = (1920 + video_caps.pictureAccessGranularity.width - 1) &
	                 ~(video_caps.pictureAccessGranularity.width - 1);
	dpb_info.height = (1080 + video_caps.pictureAccessGranularity.height - 1) &
	                  ~(video_caps.pictureAccessGranularity.height - 1);
	dpb_info.levels = 1;
	dpb_info.layers = 16;
	dpb_info.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;
	dpb_info.format = fmt;
	dpb_info.pnext = &profile_list;
	auto dpb_layers = dev.create_image(dpb_info);

	dpb_info.layers = 1;
	dpb_info.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	auto encode_input = dev.create_image(dpb_info);
	///

	auto &table = dev.get_device_table();

	VkVideoSessionCreateInfoKHR session_info = { VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR };
	session_info.maxActiveReferencePictures = 8;
	session_info.maxCodedExtent.width = dpb_info.width;
	session_info.maxCodedExtent.height = dpb_info.height;
	session_info.maxDpbSlots = 16;
	session_info.pVideoProfile = &profile_info;
	session_info.queueFamilyIndex = ctx.get_queue_info().family_indices[QUEUE_INDEX_VIDEO_ENCODE];
	session_info.pictureFormat = fmt;
	session_info.referencePictureFormat = fmt;
	session_info.pStdHeaderVersion = &video_caps.stdHeaderVersion;
	session_info.flags = VK_VIDEO_SESSION_CREATE_ALLOW_ENCODE_PARAMETER_OPTIMIZATIONS_BIT_KHR;

	VkVideoSessionKHR session;
	if (table.vkCreateVideoSessionKHR(dev.get_device(), &session_info, nullptr, &session) != VK_SUCCESS)
		return EXIT_FAILURE;

	table.vkGetVideoSessionMemoryRequirementsKHR(dev.get_device(), session, &count, nullptr);
	Util::SmallVector<VkVideoSessionMemoryRequirementsKHR> session_reqs(count);
	for (auto &req : session_reqs)
		req.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
	table.vkGetVideoSessionMemoryRequirementsKHR(dev.get_device(), session, &count, session_reqs.data());
	Util::SmallVector<DeviceAllocationOwnerHandle> allocs;
	Util::SmallVector<VkBindVideoSessionMemoryInfoKHR> binds;

	for (auto &req : session_reqs)
	{
		MemoryAllocateInfo alloc_info;
		alloc_info.mode = AllocationMode::OptimalResource;
		alloc_info.requirements = req.memoryRequirements;
		alloc_info.required_properties = 0;
		allocs.push_back(dev.allocate_memory(alloc_info));

		VkBindVideoSessionMemoryInfoKHR bind = { VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR };
		bind.memory = allocs.back()->get_allocation().get_memory();
		bind.memoryOffset = allocs.back()->get_allocation().get_offset();
		bind.memorySize = req.memoryRequirements.size;
		bind.memoryBindIndex = req.memoryBindIndex;
		binds.push_back(bind);
	}

	if (table.vkBindVideoSessionMemoryKHR(dev.get_device(), session, uint32_t(binds.size()), binds.data()) != VK_SUCCESS)
		return EXIT_FAILURE;

	VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR quality_level_info =
			{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR };
	VkVideoEncodeQualityLevelPropertiesKHR quality_level_props =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES_KHR };
	VkVideoEncodeH264QualityLevelPropertiesKHR h264_quality_level_props =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUALITY_LEVEL_PROPERTIES_KHR };
	quality_level_props.pNext = &h264_quality_level_props;
	quality_level_info.pVideoProfile = &profile_info;

	for (uint32_t i = 0; i < encode_caps.maxQualityLevels; i++)
	{
		quality_level_info.qualityLevel = i;

		if (vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR(
				dev.get_physical_device(),
				&quality_level_info,
				&quality_level_props) != VK_SUCCESS)
			return EXIT_FAILURE;

		LOGI("Got quality level %u.\n", i);
	}

	VkVideoSessionParametersKHR session_params = VK_NULL_HANDLE;
	VkVideoSessionParametersCreateInfoKHR session_param_info = { VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR };
	VkVideoEncodeH264SessionParametersCreateInfoKHR h264_session_param_info =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR };
	h264_session_param_info.maxStdPPSCount = 1;
	h264_session_param_info.maxStdSPSCount = 1;

	VkVideoEncodeH264SessionParametersAddInfoKHR add_info =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR };
	StdVideoH264SequenceParameterSet sps = {};
	StdVideoH264PictureParameterSet pps = {};

	sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
	sps.frame_crop_bottom_offset = dpb_info.height - 1080;
	sps.frame_crop_right_offset = dpb_info.width - 1920;
	sps.level_idc = h264_encode_caps.maxLevelIdc;
	sps.max_num_ref_frames = 4;
	sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
	sps.log2_max_frame_num_minus4 = 3;
	sps.pic_width_in_mbs_minus1 = dpb_info.width / 16 - 1;
	sps.pic_height_in_map_units_minus1 = dpb_info.height / 16 - 1;

	add_info.pStdPPSs = &pps;
	add_info.pStdSPSs = &sps;
	add_info.stdPPSCount = 1;
	add_info.stdSPSCount = 1;

	//h264_session_param_info.pParametersAddInfo = &add_info;
	session_param_info.pNext = &h264_session_param_info;
	session_param_info.videoSession = session;
	if (table.vkCreateVideoSessionParametersKHR(dev.get_device(), &session_param_info, nullptr, &session_params) != VK_SUCCESS)
		return EXIT_FAILURE;

	VkVideoEncodeSessionParametersGetInfoKHR params_get_info =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR };
	VkVideoEncodeH264SessionParametersGetInfoKHR h264_params_get_info =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR };
	VkVideoEncodeSessionParametersFeedbackInfoKHR feedback_info =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR };
	VkVideoEncodeH264SessionParametersFeedbackInfoKHR h264_feedback_info =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_FEEDBACK_INFO_KHR };
	size_t params_size = 0;
	params_get_info.pNext = &h264_params_get_info;
	feedback_info.pNext = &h264_feedback_info;

	params_get_info.videoSessionParameters = session_params;
	h264_params_get_info.writeStdPPS = VK_FALSE;
	h264_params_get_info.writeStdSPS = VK_TRUE;

#if 0
	auto res = table.vkGetEncodedVideoSessionParametersKHR(dev.get_device(),
	                                                       &params_get_info,
	                                                       &feedback_info,
	                                                       &params_size, nullptr);
	if (res != VK_SUCCESS)
		return EXIT_FAILURE;
#endif

	auto cmd = dev.request_command_buffer(CommandBuffer::Type::VideoEncode);
	VkVideoBeginCodingInfoKHR video_coding_info = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR };
	VkVideoEndCodingInfoKHR end_coding_info = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
	video_coding_info.videoSession = session;
	video_coding_info.videoSessionParameters = session_params;

	cmd->image_barrier(*dpb_layers, VK_IMAGE_LAYOUT_UNDEFINED,
	                   VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
	                   VK_PIPELINE_STAGE_NONE, VK_ACCESS_NONE,
	                   VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
	                   VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR |
	                   VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR);

	cmd->image_barrier(*encode_input, VK_IMAGE_LAYOUT_UNDEFINED,
	                   VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
	                   VK_PIPELINE_STAGE_NONE, VK_ACCESS_NONE,
	                   VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
	                   VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR);

	BufferCreateInfo buf_info;
	buf_info.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR;
	buf_info.size = 1024 * 1024;
	auto encode_buf = dev.create_buffer(buf_info);

	table.vkCmdBeginVideoCodingKHR(cmd->get_command_buffer(), &video_coding_info);
	{
		VkVideoEncodeInfoKHR encode_info = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR };
		encode_info.srcPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
		encode_info.srcPictureResource.baseArrayLayer = 0;
		encode_info.srcPictureResource.codedExtent = { dpb_info.width, dpb_info.height };
		encode_info.srcPictureResource.imageViewBinding = encode_input->get_view().get_view();

		VkVideoReferenceSlotInfoKHR setup_slot =
			{ VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR };
		VkVideoPictureResourceInfoKHR setup_slot_pic =
			{ VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR };
		VkVideoEncodeH264PictureInfoKHR h264_setup_slot_pic =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR };
		h264_setup_slot_pic.generatePrefixNalu = VK_TRUE;
		setup_slot_pic.imageViewBinding = dpb_layers->get_view().get_view();
		setup_slot_pic.codedExtent = { dpb_info.width, dpb_info.height };
		setup_slot_pic.baseArrayLayer = 0;
		setup_slot_pic.pNext = &h264_setup_slot_pic;

		setup_slot.pPictureResource = &setup_slot_pic;
		encode_info.pSetupReferenceSlot = &setup_slot;

		encode_info.dstBuffer = encode_buf->get_buffer();
		encode_info.dstBufferOffset = 0;
		encode_info.dstBufferRange = 1024 * 1024;

		table.vkCmdEncodeVideoKHR(cmd->get_command_buffer(), &encode_info);
	}
	table.vkCmdEndVideoCodingKHR(cmd->get_command_buffer(), &end_coding_info);

	Fence fence;
	dev.submit(cmd, &fence);
	fence->wait();

	table.vkDestroyVideoSessionKHR(dev.get_device(), session, nullptr);
	table.vkDestroyVideoSessionParametersKHR(dev.get_device(), session_params, nullptr);

	return EXIT_SUCCESS;
}