#include "device.hpp"
#include "context.hpp"

using namespace Vulkan;

struct H264Profile
{
	VkVideoProfileInfoKHR profile_info = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR };
	VkVideoEncodeH264ProfileInfoKHR h264_profile = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR };
	VkVideoProfileListInfoKHR profile_list = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR };

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
};

H264Profile::H264Profile()
{
	profile_info.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	profile_info.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile_info.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile_info.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;

	h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
	profile_info.pNext = &h264_profile;

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
	session_info.maxDpbSlots = 1;
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
		alloc_info.required_properties = 0;
		allocs.push_back(device.allocate_memory(alloc_info));

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

	if ((props3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_VIDEO_ENCODE_INPUT_BIT_KHR) == 0)
		return VK_FORMAT_UNDEFINED;
	if ((props3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_VIDEO_ENCODE_DPB_BIT_KHR) == 0)
		return VK_FORMAT_UNDEFINED;

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

	constexpr uint32_t WIDTH = 1920;
	constexpr uint32_t HEIGHT = 1080;
	constexpr uint32_t LAYERS = 2;

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
	dpb_info.layers = 2; // Ping-pong DPB.
	dpb_info.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;
	dpb_info.format = fmt;
	// Can avoid with video maint1.
	dpb_info.pnext = &profile.profile_list;
	auto dpb_layers = dev.create_image(dpb_info);

	dpb_info.layers = 1;
	dpb_info.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	auto encode_input = dev.create_image(dpb_info);
	///

	H264VideoSession sess(dev, profile, caps, WIDTH, HEIGHT, fmt);
	if (!sess.session)
		return EXIT_FAILURE;

	H264VideoSessionParameters params(dev, sess, profile, caps, WIDTH, HEIGHT);
	if (!params.params)
		return EXIT_FAILURE;

	auto &table = dev.get_device_table();

	auto cmd = dev.request_command_buffer(CommandBuffer::Type::VideoEncode);
	VkVideoBeginCodingInfoKHR video_coding_info = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR };
	VkVideoEndCodingInfoKHR end_coding_info = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
	video_coding_info.videoSession = sess.session;
	video_coding_info.videoSessionParameters = params.params;

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

	table.vkDestroyVideoSessionKHR(dev.get_device(), sess.session, nullptr);
	table.vkDestroyVideoSessionParametersKHR(dev.get_device(), params.params, nullptr);

	return EXIT_SUCCESS;
}