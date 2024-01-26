#include "device.hpp"
#include "context.hpp"

using namespace Vulkan;

struct H264Profile
{
	VkVideoProfileInfoKHR profile_info = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR };
	VkVideoEncodeH264ProfileInfoKHR h264_profile = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR };
	VkVideoProfileListInfoKHR profile_list = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR };
	VkVideoEncodeUsageInfoKHR usage_info = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR };

	H264Profile();
};

struct EncoderCaps
{
	VkVideoCapabilitiesKHR video_caps = { VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR };
	VkVideoEncodeCapabilitiesKHR encode_caps = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR };
	VkVideoEncodeH264CapabilitiesKHR h264_encode_caps = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR };
	EncoderCaps(const Device &device, const H264Profile &profile);
	bool supports_resolution(uint32_t width, uint32_t height) const;

	uint32_t get_aligned_width(uint32_t width) const;
	uint32_t get_aligned_height(uint32_t height) const;
};

struct H264VideoSession
{
	H264VideoSession(Device &device, const H264Profile &profile, const EncoderCaps &caps,
	                 uint32_t width, uint32_t height, VkFormat fmt);
	~H264VideoSession();
	Util::SmallVector<DeviceAllocationOwnerHandle> allocs;
	VkVideoSessionKHR session = VK_NULL_HANDLE;
	Device &device;
};

struct H264VideoSessionParameters
{
	H264VideoSessionParameters(Device &device, const H264VideoSession &session,
	                           const H264Profile &profile,
	                           const EncoderCaps &caps,
	                           uint32_t width, uint32_t height);
	~H264VideoSessionParameters();
	VkVideoSessionParametersKHR params = VK_NULL_HANDLE;
	StdVideoH264SequenceParameterSet sps = {};
	StdVideoH264PictureParameterSet pps = {};
	Device &device;
	std::vector<uint8_t> encoded_params;
};

H264Profile::H264Profile()
{
	profile_info.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	profile_info.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile_info.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile_info.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;

	h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
	profile_info.pNext = &h264_profile;

	usage_info.tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_HIGH_QUALITY_KHR;
	usage_info.videoContentHints = VK_VIDEO_ENCODE_CONTENT_RENDERED_BIT_KHR;
	usage_info.videoUsageHints = VK_VIDEO_ENCODE_USAGE_RECORDING_BIT_KHR;
	h264_profile.pNext = &usage_info;

	profile_list.pProfiles = &profile_info;
	profile_list.profileCount = 1;
}

EncoderCaps::EncoderCaps(const Device &device, const H264Profile &profile)
{
	video_caps.pNext = &encode_caps;
	encode_caps.pNext = &h264_encode_caps;
	vkGetPhysicalDeviceVideoCapabilitiesKHR(device.get_physical_device(), &profile.profile_info, &video_caps);
}

bool EncoderCaps::supports_resolution(uint32_t width, uint32_t height) const
{
	return width >= video_caps.minCodedExtent.width && height >= video_caps.minCodedExtent.height &&
	       width <= video_caps.maxCodedExtent.width && height <= video_caps.maxCodedExtent.height;
}

uint32_t EncoderCaps::get_aligned_width(uint32_t width) const
{
	return (width + video_caps.pictureAccessGranularity.width - 1) & ~(video_caps.pictureAccessGranularity.width - 1);
}

uint32_t EncoderCaps::get_aligned_height(uint32_t height) const
{
	return (height + video_caps.pictureAccessGranularity.height - 1) & ~(video_caps.pictureAccessGranularity.height - 1);
}

H264VideoSession::H264VideoSession(
		Device &device_, const H264Profile &profile, const EncoderCaps &caps,
		uint32_t width, uint32_t height, VkFormat fmt)
	: device(device_)
{
	auto &table = device.get_device_table();

	VkVideoSessionCreateInfoKHR session_info = { VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR };
	session_info.maxActiveReferencePictures = 1;
	session_info.maxCodedExtent.width = caps.get_aligned_width(width);
	session_info.maxCodedExtent.height = caps.get_aligned_height(height);
	session_info.maxDpbSlots = 2;
	session_info.pVideoProfile = &profile.profile_info;
	session_info.queueFamilyIndex = device.get_queue_info().family_indices[QUEUE_INDEX_VIDEO_ENCODE];
	session_info.pictureFormat = fmt;
	session_info.referencePictureFormat = fmt;
	session_info.pStdHeaderVersion = &caps.video_caps.stdHeaderVersion;
	session_info.flags = VK_VIDEO_SESSION_CREATE_ALLOW_ENCODE_PARAMETER_OPTIMIZATIONS_BIT_KHR;

	if (table.vkCreateVideoSessionKHR(device.get_device(), &session_info, nullptr, &session) != VK_SUCCESS)
	{
		session = VK_NULL_HANDLE;
		return;
	}

	uint32_t count;
	table.vkGetVideoSessionMemoryRequirementsKHR(device.get_device(), session, &count, nullptr);
	Util::SmallVector<VkVideoSessionMemoryRequirementsKHR> session_reqs(count);
	for (auto &req : session_reqs)
		req.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
	table.vkGetVideoSessionMemoryRequirementsKHR(device.get_device(), session, &count, session_reqs.data());
	Util::SmallVector<VkBindVideoSessionMemoryInfoKHR> binds;

	for (auto &req : session_reqs)
	{
		MemoryAllocateInfo alloc_info;
		alloc_info.mode = AllocationMode::OptimalResource;
		alloc_info.requirements = req.memoryRequirements;
		alloc_info.required_properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

		auto mem = device.allocate_memory(alloc_info);
		if (!mem)
		{
			alloc_info.required_properties = 0;
			mem = device.allocate_memory(alloc_info);
		}

		if (!mem)
		{
			table.vkDestroyVideoSessionKHR(device.get_device(), session, nullptr);
			session = VK_NULL_HANDLE;
			return;
		}

		allocs.push_back(std::move(mem));

		VkBindVideoSessionMemoryInfoKHR bind = { VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR };
		bind.memory = allocs.back()->get_allocation().get_memory();
		bind.memoryOffset = allocs.back()->get_allocation().get_offset();
		bind.memorySize = req.memoryRequirements.size;
		bind.memoryBindIndex = req.memoryBindIndex;
		binds.push_back(bind);
	}

	if (table.vkBindVideoSessionMemoryKHR(device.get_device(), session, uint32_t(binds.size()), binds.data()) != VK_SUCCESS)
	{
		table.vkDestroyVideoSessionKHR(device.get_device(), session, nullptr);
		session = VK_NULL_HANDLE;
	}
}

H264VideoSession::~H264VideoSession()
{
	device.get_device_table().vkDestroyVideoSessionKHR(device.get_device(), session, nullptr);
}

H264VideoSessionParameters::~H264VideoSessionParameters()
{
	device.get_device_table().vkDestroyVideoSessionParametersKHR(
			device.get_device(), params, nullptr);
}

H264VideoSessionParameters::H264VideoSessionParameters(
		Device &device_, const H264VideoSession &session,
		const H264Profile &profile,
		const EncoderCaps &caps,
		uint32_t width, uint32_t height)
	: device(device_)
{
	VkVideoSessionParametersCreateInfoKHR session_param_info =
		{ VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR };
	VkVideoEncodeH264SessionParametersCreateInfoKHR h264_session_param_info =
		{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR };
	h264_session_param_info.maxStdPPSCount = 1;
	h264_session_param_info.maxStdSPSCount = 1;

	VkVideoEncodeH264SessionParametersAddInfoKHR add_info =
		{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR };

	sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
	sps.profile_idc = profile.h264_profile.stdProfileIdc;
	sps.level_idc = caps.h264_encode_caps.maxLevelIdc;

	uint32_t aligned_width = caps.get_aligned_width(width);
	uint32_t aligned_height = caps.get_aligned_height(height);

	if (aligned_width != width || aligned_height != height)
	{
		sps.flags.frame_cropping_flag = 1;
		sps.frame_crop_right_offset = aligned_width - width;
		sps.frame_crop_bottom_offset = aligned_height - height;

		// For 4:2:0, we crop in chroma pixels.
		sps.frame_crop_right_offset >>= 1;
		sps.frame_crop_bottom_offset >>= 1;
	}

	sps.max_num_ref_frames = 1;
	sps.flags.frame_mbs_only_flag = 1;
	sps.flags.direct_8x8_inference_flag = 1;
	sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;

	constexpr uint32_t H264MacroBlockSize = 16;
	sps.pic_width_in_mbs_minus1 = aligned_width / H264MacroBlockSize - 1;
	sps.pic_height_in_map_units_minus1 = aligned_height / H264MacroBlockSize - 1;
	sps.log2_max_pic_order_cnt_lsb_minus4 = 4;

	if (caps.h264_encode_caps.stdSyntaxFlags & VK_VIDEO_ENCODE_H264_STD_TRANSFORM_8X8_MODE_FLAG_SET_BIT_KHR)
		pps.flags.transform_8x8_mode_flag = 1;
	if (caps.h264_encode_caps.stdSyntaxFlags & VK_VIDEO_ENCODE_H264_STD_ENTROPY_CODING_MODE_FLAG_SET_BIT_KHR)
		pps.flags.entropy_coding_mode_flag = 1;
	pps.flags.deblocking_filter_control_present_flag = 1;

	add_info.pStdPPSs = &pps;
	add_info.pStdSPSs = &sps;
	add_info.stdPPSCount = 1;
	add_info.stdSPSCount = 1;

	h264_session_param_info.pParametersAddInfo = &add_info;
	session_param_info.pNext = &h264_session_param_info;
	session_param_info.videoSession = session.session;

	VkVideoEncodeQualityLevelInfoKHR quality_level =
		{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR };
	quality_level.qualityLevel = caps.encode_caps.maxQualityLevels - 1;
	h264_session_param_info.pNext = &quality_level;

	auto &table = device.get_device_table();
	if (table.vkCreateVideoSessionParametersKHR(
			device.get_device(),
			&session_param_info, nullptr, &params) != VK_SUCCESS)
	{
		params = VK_NULL_HANDLE;
		return;
	}

	VkVideoEncodeSessionParametersGetInfoKHR params_get_info =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR };
	VkVideoEncodeH264SessionParametersGetInfoKHR h264_params_get_info =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR };
	VkVideoEncodeSessionParametersFeedbackInfoKHR feedback_info =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR };
	VkVideoEncodeH264SessionParametersFeedbackInfoKHR h264_feedback_info =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_FEEDBACK_INFO_KHR };
	params_get_info.pNext = &h264_params_get_info;
	feedback_info.pNext = &h264_feedback_info;

	params_get_info.videoSessionParameters = params;
	h264_params_get_info.writeStdPPS = VK_TRUE;
	h264_params_get_info.writeStdSPS = VK_TRUE;

	uint8_t params_buffer[256];
	size_t params_size = sizeof(params_buffer);
	auto res = table.vkGetEncodedVideoSessionParametersKHR(device.get_device(),
	                                                       &params_get_info,
	                                                       &feedback_info,
	                                                       &params_size, params_buffer);
	if (res != VK_SUCCESS)
	{
		table.vkDestroyVideoSessionParametersKHR(device.get_device(), params, nullptr);
		params = VK_NULL_HANDLE;
	}

	encoded_params = { params_buffer, params_buffer + params_size };
}

static VkFormat get_h264_8bit_encode_format(const Device &device, uint32_t width, uint32_t height, uint32_t layers)
{
	// Query supported formats.
	VkPhysicalDeviceVideoFormatInfoKHR format_info = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR };
	format_info.imageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;

	H264Profile profile;
	format_info.pNext = &profile.profile_list;

	uint32_t count;
	vkGetPhysicalDeviceVideoFormatPropertiesKHR(device.get_physical_device(), &format_info, &count, nullptr);

	if (count == 0)
		return VK_FORMAT_UNDEFINED;

	Util::SmallVector<VkVideoFormatPropertiesKHR> props(count);
	for (auto &p : props)
		p.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
	vkGetPhysicalDeviceVideoFormatPropertiesKHR(device.get_physical_device(), &format_info, &count, props.data());

	VkFormat fmt = props.front().format;

	// Sanity check
	VkFormatProperties3 props3 = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 };
	device.get_format_properties(fmt, &props3);

#if 0
	if ((props3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_VIDEO_ENCODE_INPUT_BIT_KHR) == 0)
		return VK_FORMAT_UNDEFINED;
	if ((props3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_VIDEO_ENCODE_DPB_BIT_KHR) == 0)
		return VK_FORMAT_UNDEFINED;
#endif

	VkImageFormatProperties2 props2 = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
	device.get_image_format_properties(fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
	                                   VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, 0,
	                                   &profile.profile_list, &props2);

	if (props2.imageFormatProperties.maxArrayLayers < layers)
		return VK_FORMAT_UNDEFINED;
	if (props2.imageFormatProperties.maxExtent.width < width)
		return VK_FORMAT_UNDEFINED;
	if (props2.imageFormatProperties.maxExtent.height < height)
		return VK_FORMAT_UNDEFINED;

	return fmt;
}

struct H264RateControl
{
	VkVideoEncodeRateControlInfoKHR rate_info =
		{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR };
	VkVideoEncodeH264RateControlInfoKHR h264_rate_control =
		{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR };
	VkVideoEncodeH264RateControlLayerInfoKHR h264_layer =
		{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR };
	VkVideoEncodeRateControlLayerInfoKHR layer =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR };
};

static constexpr uint32_t IDR_PERIOD = 4096;

static void reset_rate_control(CommandBuffer &cmd,
							   H264RateControl &rate,
							   const EncoderCaps &caps,
							   const H264VideoSession &sess,
							   const H264VideoSessionParameters &params)
{
	auto &dev = cmd.get_device();
	auto &table = dev.get_device_table();

	VkVideoBeginCodingInfoKHR video_coding_info = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR };
	VkVideoEndCodingInfoKHR end_coding_info = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
	video_coding_info.videoSession = sess.session;
	video_coding_info.videoSessionParameters = params.params;

	VkVideoCodingControlInfoKHR ctrl_info = { VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR };
	ctrl_info.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;

	table.vkCmdBeginVideoCodingKHR(cmd.get_command_buffer(), &video_coding_info);
	table.vkCmdControlVideoCodingKHR(cmd.get_command_buffer(), &ctrl_info);
	table.vkCmdEndVideoCodingKHR(cmd.get_command_buffer(), &end_coding_info);

	ctrl_info.flags = VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
	ctrl_info.pNext = &rate.rate_info;

	if (caps.encode_caps.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR)
		rate.rate_info.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
	else if (caps.encode_caps.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR)
		rate.rate_info.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
	else if (caps.encode_caps.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR)
		rate.rate_info.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
	else
		rate.rate_info.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;

	if (rate.rate_info.rateControlMode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR &&
	    rate.rate_info.rateControlMode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR)
	{
		rate.h264_rate_control.consecutiveBFrameCount = 0;
		rate.h264_rate_control.idrPeriod = IDR_PERIOD;
		rate.h264_rate_control.gopFrameCount = IDR_PERIOD;
		rate.h264_rate_control.temporalLayerCount = 1;
		rate.h264_rate_control.flags = VK_VIDEO_ENCODE_H264_RATE_CONTROL_REGULAR_GOP_BIT_KHR |
		                               VK_VIDEO_ENCODE_H264_RATE_CONTROL_ATTEMPT_HRD_COMPLIANCE_BIT_KHR;

		rate.rate_info.pNext = &rate.h264_rate_control;
		rate.rate_info.virtualBufferSizeInMs = 100;
		rate.rate_info.initialVirtualBufferSizeInMs = 0;
		rate.rate_info.layerCount = 1;
		rate.rate_info.pLayers = &rate.layer;

		rate.h264_layer.useMinQp = VK_TRUE;
		rate.h264_layer.useMaxQp = VK_TRUE;
		rate.h264_layer.minQp.qpI = 18;
		rate.h264_layer.maxQp.qpI = 34;
		rate.h264_layer.minQp.qpP = 22;
		rate.h264_layer.maxQp.qpP = 38;
		rate.h264_layer.minQp.qpB = 24;
		rate.h264_layer.maxQp.qpB = 40;

		rate.layer.frameRateNumerator = 24;
		rate.layer.frameRateDenominator = 1;
		rate.layer.averageBitrate = 20 * 1000 * 1000;
		rate.layer.maxBitrate = 20 * 1000 * 1000;
		rate.layer.pNext = &rate.h264_layer;
	}

	table.vkCmdBeginVideoCodingKHR(cmd.get_command_buffer(), &video_coding_info);
	table.vkCmdControlVideoCodingKHR(cmd.get_command_buffer(), &ctrl_info);
	table.vkCmdEndVideoCodingKHR(cmd.get_command_buffer(), &end_coding_info);

	VkVideoEncodeQualityLevelInfoKHR quality_level =
		{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR };
	quality_level.qualityLevel = caps.encode_caps.maxQualityLevels - 1;
	ctrl_info.flags = VK_VIDEO_CODING_CONTROL_ENCODE_QUALITY_LEVEL_BIT_KHR;
	ctrl_info.pNext = &quality_level;

	table.vkCmdBeginVideoCodingKHR(cmd.get_command_buffer(), &video_coding_info);
	table.vkCmdControlVideoCodingKHR(cmd.get_command_buffer(), &ctrl_info);
	table.vkCmdEndVideoCodingKHR(cmd.get_command_buffer(), &end_coding_info);
}

static void encode_frame(FILE *file, Device &device, const Image &input, const ImageHandle *dpb,
						 const Buffer &encode_buffer,
						 const H264VideoSession &session, const H264VideoSessionParameters &params,
						 const H264RateControl &rate,
						 VkQueryPool query_pool, uint32_t frame_index, uint32_t &idr_num)
{
	auto &table = device.get_device_table();
	auto cmd = device.request_command_buffer(CommandBuffer::Type::VideoEncode);

	cmd->image_barrier_acquire(input, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                           VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
	                           VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
	                           device.get_queue_info().family_indices[QUEUE_INDEX_TRANSFER],
	                           VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
	                           VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR);

	VkVideoBeginCodingInfoKHR video_coding_info = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR };
	VkVideoEndCodingInfoKHR end_coding_info = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
	video_coding_info.videoSession = session.session;
	video_coding_info.videoSessionParameters = params.params;

	video_coding_info.pNext = &rate.rate_info;

	uint32_t frame_index_last_idr = frame_index & ~(IDR_PERIOD - 1);
	bool is_idr = frame_index == frame_index_last_idr;
	uint32_t delta_frame = frame_index - frame_index_last_idr;
	uint32_t prev_delta_frame = delta_frame - 1;

	VkVideoPictureResourceInfoKHR reconstructed_slot_pic =
		{ VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR };
	reconstructed_slot_pic.imageViewBinding = dpb[frame_index & 1]->get_view().get_view();
	reconstructed_slot_pic.codedExtent = { dpb[0]->get_width(), dpb[0]->get_height() };

	VkVideoPictureResourceInfoKHR reference_slot_pic =
		{ VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR };
	reference_slot_pic.imageViewBinding = dpb[(frame_index - 1) & 1]->get_view().get_view();
	reference_slot_pic.codedExtent = { dpb[0]->get_width(), dpb[0]->get_height() };

	VkVideoReferenceSlotInfoKHR init_slots[2] = {};

	init_slots[0].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	init_slots[0].slotIndex = -1;
	init_slots[0].pPictureResource = &reconstructed_slot_pic;

	init_slots[1].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	init_slots[1].slotIndex = int((frame_index - 1) & 1);
	init_slots[1].pPictureResource = &reference_slot_pic;

	video_coding_info.referenceSlotCount = is_idr ? 1 : 2;
	video_coding_info.pReferenceSlots = init_slots;

	VkVideoEncodeInfoKHR encode_info = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR };
	encode_info.srcPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
	encode_info.srcPictureResource.codedExtent = { input.get_width(), input.get_height() };
	encode_info.srcPictureResource.imageViewBinding = input.get_view().get_view();

	VkVideoEncodeH264PictureInfoKHR h264_src_info = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR };
	VkVideoEncodeH264NaluSliceInfoKHR slice[3] = {};
	for (auto &s : slice)
		s.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR;

	StdVideoEncodeH264SliceHeader slice_header[3] = {};
	StdVideoEncodeH264PictureInfo pic = {};

	StdVideoEncodeH264ReferenceListsInfo ref_lists = {};
	for (uint32_t i = 0; i < STD_VIDEO_H264_MAX_NUM_LIST_REF; i++)
	{
		ref_lists.RefPicList0[i] = i || is_idr ? STD_VIDEO_H264_NO_REFERENCE_PICTURE : ((frame_index - 1) & 1);
		ref_lists.RefPicList1[i] = STD_VIDEO_H264_NO_REFERENCE_PICTURE;
	}

	pic.flags.IdrPicFlag = is_idr ? 1 : 0;
	pic.flags.is_reference = 1;
	if (is_idr)
		pic.idr_pic_id = idr_num++;
	pic.pRefLists = &ref_lists;

	constexpr unsigned H264MacroBlockSize = 16;
	unsigned num_mb_x = input.get_width() / H264MacroBlockSize;
	unsigned num_mb_y = input.get_height() / H264MacroBlockSize;
	unsigned mb_y = delta_frame % num_mb_y;

	for (unsigned i = 0; i < 3; i++)
	{
		slice[i].pStdSliceHeader = &slice_header[i];
		slice_header[i].cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0;
		if (rate.rate_info.rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR)
			slice[i].constantQp = 28;
	}

	if (is_idr)
	{
		h264_src_info.naluSliceEntryCount = 1;
		slice_header[0].slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
	}
	else if (mb_y == 0)
	{
		h264_src_info.naluSliceEntryCount = 2;
		slice_header[0].slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
		slice_header[1].slice_type = STD_VIDEO_H264_SLICE_TYPE_P;
		slice_header[1].first_mb_in_slice = num_mb_x;
	}
	else if (mb_y == num_mb_y - 1)
	{
		h264_src_info.naluSliceEntryCount = 2;
		slice_header[0].slice_type = STD_VIDEO_H264_SLICE_TYPE_P;
		slice_header[1].slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
		slice_header[1].first_mb_in_slice = mb_y * num_mb_x;
	}
	else
	{
		h264_src_info.naluSliceEntryCount = 3;
		slice_header[0].slice_type = STD_VIDEO_H264_SLICE_TYPE_P;
		slice_header[1].slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
		slice_header[2].slice_type = STD_VIDEO_H264_SLICE_TYPE_P;
		slice_header[1].first_mb_in_slice = mb_y * num_mb_x;
		slice_header[2].first_mb_in_slice = (mb_y + 1) * num_mb_x;
	}

	h264_src_info.pNaluSliceEntries = slice;
	h264_src_info.pStdPictureInfo = &pic;
	h264_src_info.pNext = encode_info.pNext;
	encode_info.pNext = &h264_src_info;

	// Setup DPB entry for reconstructed IDR frame
	VkVideoReferenceSlotInfoKHR reconstructed_setup_slot =
		{ VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR };
	reconstructed_setup_slot.pPictureResource = &reconstructed_slot_pic;
	VkVideoEncodeH264DpbSlotInfoKHR h264_reconstructed_dpb_slot =
		{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR };
	StdVideoEncodeH264ReferenceInfo h264_reconstructed_ref = {};
	h264_reconstructed_dpb_slot.pStdReferenceInfo = &h264_reconstructed_ref;

	if (is_idr)
	{
		h264_reconstructed_ref.primary_pic_type = STD_VIDEO_H264_PICTURE_TYPE_IDR;
		pic.primary_pic_type = STD_VIDEO_H264_PICTURE_TYPE_IDR;
	}
	else
	{
		// There are always some I slices.
		h264_reconstructed_ref.primary_pic_type = STD_VIDEO_H264_PICTURE_TYPE_I;
		pic.primary_pic_type = STD_VIDEO_H264_PICTURE_TYPE_I;
	}

	h264_reconstructed_ref.FrameNum = delta_frame & ((1u << (params.sps.log2_max_frame_num_minus4 + 4)) - 1u);
	h264_reconstructed_ref.PicOrderCnt = int(delta_frame & ((1u << (params.sps.log2_max_pic_order_cnt_lsb_minus4 + 4)) - 1u));
	reconstructed_setup_slot.slotIndex = int32_t(delta_frame & 1);
	reconstructed_setup_slot.pNext = &h264_reconstructed_dpb_slot;
	encode_info.pSetupReferenceSlot = &reconstructed_setup_slot;

	VkVideoReferenceSlotInfoKHR prev_ref_slot =
			{ VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR };
	VkVideoEncodeH264DpbSlotInfoKHR h264_prev_ref_slot =
			{ VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR };
	StdVideoEncodeH264ReferenceInfo h264_prev_ref = {};

	if (!is_idr)
	{
		prev_ref_slot.pPictureResource = &reference_slot_pic;
		prev_ref_slot.slotIndex = int(prev_delta_frame & 1);
		prev_ref_slot.pNext = &h264_prev_ref_slot;
		h264_prev_ref_slot.pStdReferenceInfo = &h264_prev_ref;

		h264_prev_ref.FrameNum =
				prev_delta_frame &
				((1u << (params.sps.log2_max_frame_num_minus4 + 4)) - 1u);
		h264_prev_ref.PicOrderCnt =
				int(prev_delta_frame &
				    ((1u << (params.sps.log2_max_pic_order_cnt_lsb_minus4 + 4)) - 1u));

		// Does this matter?
		if (prev_delta_frame == 0)
			h264_prev_ref.primary_pic_type = STD_VIDEO_H264_PICTURE_TYPE_IDR;
		else
			h264_prev_ref.primary_pic_type = STD_VIDEO_H264_PICTURE_TYPE_I;

		encode_info.pReferenceSlots = &prev_ref_slot;
		encode_info.referenceSlotCount = 1;
	}

	encode_info.dstBuffer = encode_buffer.get_buffer();
	encode_info.dstBufferOffset = 0;
	encode_info.dstBufferRange = encode_buffer.get_create_info().size;

	table.vkCmdResetQueryPool(cmd->get_command_buffer(), query_pool, 0, 1);
	table.vkCmdBeginVideoCodingKHR(cmd->get_command_buffer(), &video_coding_info);
	table.vkCmdBeginQuery(cmd->get_command_buffer(), query_pool, 0, 0);
	table.vkCmdEncodeVideoKHR(cmd->get_command_buffer(), &encode_info);
	table.vkCmdEndQuery(cmd->get_command_buffer(), query_pool, 0);
	table.vkCmdEndVideoCodingKHR(cmd->get_command_buffer(), &end_coding_info);

	cmd->barrier(VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
	             VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
	             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);

	Fence fence;
	device.submit(cmd, &fence);
	fence->wait();

	uint32_t query_data[3] = {};
	table.vkGetQueryPoolResults(device.get_device(), query_pool, 0, 1, sizeof(query_data),
	                            query_data, sizeof(query_data),
	                            VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);

	LOGI("Offset = %u, Bytes = %u, Status = %u\n", query_data[0], query_data[1], query_data[2]);

	if (file && query_data[2] == VK_QUERY_RESULT_STATUS_COMPLETE_KHR)
	{
		auto *payload = static_cast<const uint8_t *>(device.map_host_buffer(encode_buffer, MEMORY_ACCESS_READ_BIT));
		fwrite(payload + query_data[0], query_data[1], 1, file);
	}
}

static bool upload_file(FILE *file, Device &device, const Image &image, uint32_t width, uint32_t height)
{
	auto cmd = device.request_command_buffer(CommandBuffer::Type::AsyncTransfer);

	cmd->image_barrier(image, VK_IMAGE_LAYOUT_UNDEFINED,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   VK_PIPELINE_STAGE_NONE, VK_ACCESS_NONE,
	                   VK_PIPELINE_STAGE_2_COPY_BIT,
	                   VK_ACCESS_TRANSFER_WRITE_BIT);

	auto *luma = static_cast<uint8_t *>(
			cmd->update_image(image, {}, { width, height, 1 },
			                  0, 0, { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 }));
	auto *chroma = static_cast<uint16_t *>(
			cmd->update_image(image, {}, { width / 2, height / 2, 1 },
			                  0, 0, { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 }));

	// TODO: Fill in padding region.

	if (fread(luma, width * height, 1, file) == 0)
	{
		device.submit_discard(cmd);
		return false;
	}

	if (fread(chroma, width * height / 2, 1, file) == 0)
	{
		device.submit_discard(cmd);
		return false;
	}

	cmd->image_barrier_release(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
	                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	                           device.get_queue_info().family_indices[QUEUE_INDEX_VIDEO_ENCODE]);

	Fence fence;
	Semaphore sem;
	device.submit(cmd, &fence, 1, &sem);
	device.add_wait_semaphore(CommandBuffer::Type::VideoEncode, std::move(sem),
	                          VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, true);

	fence->wait();

	return true;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return EXIT_FAILURE;

	FILE *input_file = fopen(argv[1], "rb");
	if (!input_file)
		return EXIT_FAILURE;

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

	constexpr uint32_t WIDTH = 1280;
	constexpr uint32_t HEIGHT = 720;
	constexpr uint32_t LAYERS = 1;

	VkFormat fmt = get_h264_8bit_encode_format(dev, WIDTH, HEIGHT, LAYERS);
	if (fmt == VK_FORMAT_UNDEFINED)
		return EXIT_FAILURE;

	H264Profile profile;
	EncoderCaps caps(dev, profile);
	if (!caps.supports_resolution(WIDTH, HEIGHT))
		return EXIT_FAILURE;

	// Create DPB layers and input image
	ImageCreateInfo dpb_info;
	dpb_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	dpb_info.width = caps.get_aligned_width(WIDTH);
	dpb_info.height = caps.get_aligned_height(HEIGHT);
	dpb_info.levels = 1;
	dpb_info.layers = 1; // Ping-pong DPB.
	dpb_info.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;
	dpb_info.format = fmt;
	// Can avoid with video maint1.
	dpb_info.pnext = &profile.profile_list;
	ImageHandle dpb_layers[2];
	dpb_layers[0] = dev.create_image(dpb_info);
	dpb_layers[1] = dev.create_image(dpb_info);
	dev.set_name(*dpb_layers[0], "dpb_layers");
	dev.set_name(*dpb_layers[1], "dpb_layers");

	dpb_info.layers = 1;
	dpb_info.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	auto encode_input = dev.create_image(dpb_info);
	dev.set_name(*encode_input, "encode_input");
	///

	H264VideoSession sess(dev, profile, caps, WIDTH, HEIGHT, fmt);
	if (!sess.session)
		return EXIT_FAILURE;

	H264VideoSessionParameters params(dev, sess, profile, caps, WIDTH, HEIGHT);
	if (!params.params)
		return EXIT_FAILURE;

	auto &table = dev.get_device_table();
	H264RateControl rate;

	{
		auto cmd = dev.request_command_buffer(CommandBuffer::Type::VideoEncode);
		reset_rate_control(*cmd, rate, caps, sess, params);
		for (auto &dpb : dpb_layers)
		{
			cmd->image_barrier(*dpb, VK_IMAGE_LAYOUT_UNDEFINED,
			                   VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
			                   VK_PIPELINE_STAGE_NONE, VK_ACCESS_NONE,
			                   VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
			                   VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR |
			                   VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR);
		}
		dev.submit(cmd);
	}

	BufferCreateInfo buf_info;
	buf_info.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR;
	buf_info.size = 1024 * 1024;
	buf_info.domain = BufferDomain::CachedHost;
	buf_info.pnext = &profile.profile_list;
	auto encode_buf = dev.create_buffer(buf_info);

	VkQueryPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	VkQueryPoolVideoEncodeFeedbackCreateInfoKHR feedback_pool_info =
		{ VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR };
	feedback_pool_info.encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
	                                         VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
	feedback_pool_info.pNext = &profile.profile_info;
	pool_info.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
	pool_info.queryCount = 1;
	pool_info.pNext = &feedback_pool_info;
	VkQueryPool query_pool = VK_NULL_HANDLE;
	if (table.vkCreateQueryPool(dev.get_device(), &pool_info, nullptr, &query_pool) != VK_SUCCESS)
		return EXIT_FAILURE;

	FILE *output_file = fopen("/tmp/test.h264", "wb");
	if (output_file)
		fwrite(params.encoded_params.data(), params.encoded_params.size(), 1, output_file);

	uint32_t frame_count = 0;
	uint32_t idr_num = 0;

	while (upload_file(input_file, dev, *encode_input, WIDTH, HEIGHT))
	{
		encode_frame(output_file, dev,
					 *encode_input, dpb_layers, *encode_buf,
					 sess, params, rate,
					 query_pool, frame_count, idr_num);

		frame_count += 1;
		dev.next_frame_context();
	}

	table.vkDestroyQueryPool(dev.get_device(), query_pool, nullptr);

	if (output_file)
		fclose(output_file);
	if (input_file)
		fclose(input_file);

	return EXIT_SUCCESS;
}