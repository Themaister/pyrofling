/* Copyright (c) 2023 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "client.hpp"
#include "listener.hpp"
#include "context.hpp"
#include "device.hpp"
#include "thread_group.hpp"
#include "intrusive.hpp"
#include "ffmpeg_encode.hpp"
#include "audio_interface.hpp"
#include "cli_parser.hpp"
#include "timer.hpp"
#include "slangmosh_encode_iface.hpp"
#include "slangmosh_encode.hpp"
#include "pyro_server.hpp"
#include "virtual_gamepad.hpp"
#include "timeline_trace_file.hpp"
#include <stdexcept>
#include <vector>
#include <thread>
#include <cmath>

#include <unistd.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <assert.h>

#ifdef HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/format.h>
#endif

using namespace PyroFling;

// Some loose copy-pasta from Gamescope code.
#ifdef HAVE_PIPEWIRE
class SwapchainServer;

class PipewireStream : public Handler
{
public:
	PipewireStream(Dispatcher &dispatcher, SwapchainServer &server, Vulkan::Device &device);
	bool init(unsigned width, unsigned height, unsigned fps);

	~PipewireStream();

	int get_fd();
	bool iterate();
	void process();

	void stream_state_changed(pw_stream_state old, pw_stream_state state, const char *error);
	void stream_param_changed(uint32_t id, const spa_pod *param);

	bool handle(const FileHandle &fd, uint32_t id) override;

	void release_id(uint32_t id) override;

	void send_encoding(const Vulkan::Image &img);

private:
	SwapchainServer &server;
	Vulkan::Device &device;
	pw_loop *loop = nullptr;
	pw_stream *stream = nullptr;
	spa_video_info_raw raw_video_info = {};
	uint32_t stride = 0;
};

static void on_stream_state_changed(void *data_, pw_stream_state old, pw_stream_state state, const char *error)
{
	static_cast<PipewireStream *>(data_)->stream_state_changed(old, state, error);
}

static void on_stream_param_changed(void *data_, uint32_t id, const spa_pod *param)
{
	static_cast<PipewireStream *>(data_)->stream_param_changed(id, param);
}

static void on_process(void *data_)
{
	static_cast<PipewireStream *>(data_)->process();
}

PipewireStream::PipewireStream(Dispatcher &dispatcher_, SwapchainServer &server_, Vulkan::Device &device_)
	: Handler(dispatcher_), server(server_), device(device_)
{
}

void PipewireStream::process()
{
	// Dequeue buffers and encode them.
	for (;;)
	{
		struct pw_buffer *t;
		if ((t = pw_stream_dequeue_buffer(stream)) == nullptr)
			break;

		spa_buffer *buffer = t->buffer;

		if (buffer->n_datas != 1)
		{
			// Planar data is unexpected ...
			LOGE("Got planar data. This should not happen.\n");
			pw_stream_queue_buffer(stream, t);
			continue;
		}

		auto info = Vulkan::ImageCreateInfo::immutable_2d_image(
				raw_video_info.size.width,
				raw_video_info.size.height,
				VK_FORMAT_B8G8R8A8_SRGB);

		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
		info.misc = Vulkan::IMAGE_MISC_MUTABLE_SRGB_BIT | Vulkan::IMAGE_MISC_EXTERNAL_MEMORY_BIT;
		// Vulkan takes ownership of the fd, so dup it first.
		info.external.handle = dup(int(buffer->datas[0].fd));
		info.external.memory_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

		VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_info =
				{ VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT };
		VkSubresourceLayout layout = {};
		explicit_info.drmFormatModifier = raw_video_info.modifier;
		explicit_info.drmFormatModifierPlaneCount = 1;
		explicit_info.pPlaneLayouts = &layout;
		layout.offset = buffer->datas[0].chunk->offset;
		layout.rowPitch = buffer->datas[0].chunk->stride;
		info.pnext = &explicit_info;

		// TODO: memoize the imports? fstat + inodes is apparently a thing.
		auto img = device.create_image(info);
		if (img)
			send_encoding(*img);
		else
			LOGE("Failed to import DMABUF.\n");

		// Pipewire relies on implicit sync supposedly,
		// so after we have submitted conversion work, we're done.
		pw_stream_queue_buffer(stream, t);
	}
}

void PipewireStream::stream_param_changed(uint32_t id, const spa_pod *param)
{
	uint8_t params_buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[1];

	/* nullptr means to clear the format */
	if (param == nullptr || id != SPA_PARAM_Format)
		return;

	uint32_t media_type, media_subtype;
	if (spa_format_parse(param, &media_type, &media_subtype) < 0)
	{
		LOGE("Failed to parse spa format.\n");
		return;
	}

	if (media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw)
	{
		LOGE("Received something not raw video.\n");
		return;
	}

	/* call a helper function to parse the format for us. */
	spa_format_video_raw_parse(param, &raw_video_info);

	if (raw_video_info.format != SPA_VIDEO_FORMAT_BGRx)
	{
		LOGE("Expected BGRx format.\n");
		pw_stream_set_error(stream, -EINVAL, "unknown pixel format");
		return;
	}

	if (raw_video_info.size.width == 0 || raw_video_info.size.height == 0)
	{
		pw_stream_set_error(stream, -EINVAL, "invalid size");
		return;
	}

	stride = SPA_ROUND_UP_N(raw_video_info.size.width * 4, 4);

	// A SPA_TYPE_OBJECT_ParamBuffers object defines the acceptable size, number, stride, etc of the buffers.
	params[0] = static_cast<const struct spa_pod *>(
			spa_pod_builder_add_object(&b,
			                           SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			                           SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 64),
			                           SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
			                           SPA_PARAM_BUFFERS_size, SPA_POD_Int(stride * raw_video_info.size.height),
			                           SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
			                           SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_DmaBuf))));

	// We are done
	pw_stream_update_params(stream, params, 1);
}

void PipewireStream::stream_state_changed(pw_stream_state, pw_stream_state state, const char *error)
{
	LOGI("PipeWire state: %s\n", pw_stream_state_as_string(state));
	if (error)
		LOGE("PipeWire error: \"%s\"\n", error);

	switch (state)
	{
	case PW_STREAM_STATE_UNCONNECTED:
		break;
	case PW_STREAM_STATE_PAUSED:
		break;
	case PW_STREAM_STATE_STREAMING:
	default:
		break;
	}
}

bool PipewireStream::iterate()
{
	return pw_loop_iterate(loop, -1) >= 0;
}

int PipewireStream::get_fd()
{
	return pw_loop_get_fd(loop);
}

bool PipewireStream::init(unsigned width, unsigned height, unsigned fps)
{
	if (!device.get_device_features().supports_drm_modifiers)
	{
		LOGW("Device does not support DRM modifiers. Ignoring pipewire.\n");
		return false;
	}

	int argc = 1;
	char arg[] = "pyrofling";
	char *argv[] = { arg, nullptr };
	char **argvs = argv;

	pw_init(&argc, &argvs);

	loop = pw_loop_new(nullptr);
	if (!loop)
		return false;

	auto *props = pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Camera",
			nullptr);

	static const struct pw_stream_events stream_events = {
		.version = PW_VERSION_STREAM_EVENTS,
		.state_changed = on_stream_state_changed,
		.param_changed = on_stream_param_changed,
		.process = on_process,
	};

	// props ownership is taken
	stream = pw_stream_new_simple(loop, "pyrofling-video", props, &stream_events, this);
	if (!stream)
		return false;

	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params = nullptr;
	struct spa_pod_frame f[2];

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(&b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx), 0);

	// Query which DRM format modifiers we can support for the default BGRA8 format.
	VkFormatProperties3 props3 = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 };
	VkDrmFormatModifierPropertiesListEXT list = { VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
	props3.pNext = &list;
	device.get_format_properties(VK_FORMAT_B8G8R8A8_SRGB, &props3);
	std::vector<VkDrmFormatModifierPropertiesEXT> drm_props(list.drmFormatModifierCount);
	list.pDrmFormatModifierProperties = drm_props.data();
	device.get_format_properties(VK_FORMAT_B8G8R8A8_SRGB, &props3);
	spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
	spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_Enum, 0);
	{
		for (auto &p : drm_props)
		{
			if (!(p.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
				continue;
			spa_pod_builder_long(&b, int64_t(p.drmFormatModifier));
			if (&p == &drm_props.front())
				spa_pod_builder_long(&b, int64_t(p.drmFormatModifier));
		}
	}
	spa_pod_builder_pop(&b, &f[1]);

	spa_rectangle default_size = SPA_RECTANGLE(width, height);
	spa_rectangle min_size = SPA_RECTANGLE(1, 1);
	spa_rectangle max_size = SPA_RECTANGLE(65535, 65535);

	spa_fraction default_fps = SPA_FRACTION(fps, 1);
	spa_fraction lo_fps = SPA_FRACTION(0, 1);
	spa_fraction hi_fps = SPA_FRACTION(fps, 1);

	spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_size,
	                    SPA_POD_CHOICE_RANGE_Rectangle(&default_size, &min_size, &max_size), 0);
	spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_framerate,
	                    SPA_POD_CHOICE_RANGE_Fraction(&default_fps, &lo_fps, &hi_fps), 0);

	params = static_cast<spa_pod *>(spa_pod_builder_pop(&b, &f[0]));

	if (pw_stream_connect(
			stream, PW_DIRECTION_INPUT, PW_ID_ANY,
			pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
			&params, 1))
	{
		return false;
	}

	//pw_loop_add_event(loop, reneg_formats, this);

	return true;
}

PipewireStream::~PipewireStream()
{
	if (stream)
		pw_stream_destroy(stream);
	if (loop)
		pw_loop_destroy(loop);
}

bool PipewireStream::handle(const FileHandle &, uint32_t)
{
	return iterate();
}

void PipewireStream::release_id(uint32_t)
{
}
#endif

struct InstanceDeleter
{
	inline void operator()(VkInstance instance) const
	{
		vkDestroyInstance(instance, nullptr);
	}
};

struct SwapchainServer final : HandlerFactoryInterface, Vulkan::InstanceFactory, Granite::MuxStreamCallback
{
	~SwapchainServer() override
	{
		group.wait_idle();
		assert(handlers.empty());
	}

	explicit SwapchainServer(Dispatcher &dispatcher_)
		: dispatcher(dispatcher_)
	{
		if (!Vulkan::Context::init_loader(nullptr))
			throw std::runtime_error("Failed to load Vulkan.");

		Vulkan::Context instance_context;
		if (!instance_context.init_instance(nullptr, 0,
		                                    Vulkan::CONTEXT_CREATION_ENABLE_VIDEO_ENCODE_BIT |
		                                    Vulkan::CONTEXT_CREATION_ENABLE_VIDEO_H265_BIT |
		                                    Vulkan::CONTEXT_CREATION_ENABLE_VIDEO_H264_BIT))
			throw std::runtime_error("Failed to create Vulkan instance.");
		instance.reset(instance_context.get_instance());
		instance_context.release_instance();

		uint32_t gpu_count;
		vkEnumeratePhysicalDevices(instance.get(), &gpu_count, nullptr);
		gpus.reserve(gpu_count);
		std::vector<VkPhysicalDevice> gpu_handles(gpu_count);
		vkEnumeratePhysicalDevices(instance.get(), &gpu_count, gpu_handles.data());

		for (auto gpu : gpu_handles)
		{
			VkPhysicalDeviceIDProperties id_props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
			VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
			props2.pNext = &id_props;
			vkGetPhysicalDeviceProperties2(gpu, &props2);

			PhysicalDevice dev = {};
			dev.gpu = gpu;
			memcpy(dev.driver_id, id_props.driverUUID, VK_UUID_SIZE);
			memcpy(dev.device_uuid, id_props.deviceUUID, VK_UUID_SIZE);

			if (id_props.deviceLUIDValid)
				memcpy(dev.luid, id_props.deviceLUID, VK_LUID_SIZE);
			else
				memset(dev.luid, 0xff, VK_LUID_SIZE);
			dev.luid_valid = id_props.deviceLUIDValid;

			gpus.push_back(std::move(dev));
		}

		group.start(4, 0, {});
	}

	struct DeviceContext
	{
		Vulkan::Context context;
		Vulkan::Device device;
	};

	struct DeviceContextAssociation
	{
		DeviceContext *ctx;
	};

	struct Swapchain final : Handler, Util::ThreadSafeIntrusivePtrEnabled<Swapchain>
	{
		Swapchain(Dispatcher &dispatcher_, SwapchainServer &server_)
				: Handler(dispatcher_), server(server_)
		{
			int fds[2];
			if (::pipe2(fds, O_CLOEXEC | O_DIRECT) < 0)
				throw std::runtime_error("Failed to create pipe.");
			pipe_fd = FileHandle{fds[1]};

			add_reference();
			dispatcher.add_connection(FileHandle{fds[0]}, this, 1, Dispatcher::ConnectionType::Input);
			LOGI("Swapchain init.\n");
		}

		~Swapchain() override
		{
			LOGI("Swapchain teardown.\n");
			clear_images();
		}

		void release_id(uint32_t id) override
		{
			if (id == 0)
			{
				server.unregister_handler(this);

				// Make sure we tear down the pipe handler as well.
				const uint64_t sentinel[2] = { uint64_t(-1), uint64_t(-1) };
				ssize_t ret = ::write(pipe_fd.get_native_handle(), &sentinel, sizeof(sentinel));
				if (ret < 0 && errno != EPIPE)
					LOGE("Failed to terminate pipe.\n");
			}

			release_reference();
		}

		void clear_images()
		{
			bool need_wait_idle = false;
			for (auto &img : images)
				if (img.cross_device_host_pointer)
					need_wait_idle = true;

			images.clear();

			// Need to garbage collect all VkDeviceMemory immediately before any subsequent submit happens.
			// If device memory is live during a submit that references freed host pointers,
			// device lost can happen at least on amdgpu and ANV.
			if (need_wait_idle)
			{
				association.ctx->device.wait_idle();
				if (server.encoder_device)
					server.encoder_device->wait_idle();
			}
		}

		bool handle_swapchain_create(const FileHandle &fd, ImageGroupMessage &image_create)
		{
			auto &device = association.ctx->device;

			image_group_serial = image_create.get_serial();

			LOGI("Image group request.\n");

			if (image_create.wire.num_images != image_create.fds.size())
			{
				LOGE("Invalid num images.\n");
				return send_message(fd, MessageType::ErrorParameter, image_create.get_serial());
			}

			Vulkan::ImageCreateInfo info = {};
			info.domain = Vulkan::ImageDomain::Physical;
			info.width = image_create.wire.width;
			info.height = image_create.wire.height;
			info.depth = 1;
			info.levels = 1;
			info.layers = 1;
			info.usage = image_create.wire.vk_image_usage;
			info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			info.type = VK_IMAGE_TYPE_2D;
			info.format = static_cast<VkFormat>(image_create.wire.vk_format);
			info.flags = image_create.wire.vk_image_flags;
			info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.misc = Vulkan::IMAGE_MISC_EXTERNAL_MEMORY_BIT;

			if ((image_create.wire.vk_image_usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0)
			{
				LOGE("VK_IMAGE_USAGE_SAMPLED_BIT required.\n");
				return send_message(fd, MessageType::ErrorParameter, image_create.get_serial());
			}

			if ((image_create.wire.vk_image_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0)
			{
				LOGE("VK_IMAGE_USAGE_TRANSFER_SRC required.\n");
				return send_message(fd, MessageType::ErrorParameter, image_create.get_serial());
			}

			if (image_create.wire.vk_external_memory_type != Vulkan::ExternalHandle::get_opaque_memory_handle_type())
			{
				LOGE("Only opaque FD is currently supported.\n");
				return send_message(fd, MessageType::ErrorParameter, image_create.get_serial());
			}

			// TODO: Add query for formats ala vkGetPhysicalDeviceSurfaceFormatsKHR.
			if (image_create.wire.vk_color_space != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
			    image_create.wire.vk_color_space != VK_COLOR_SPACE_HDR10_ST2084_EXT &&
			    image_create.wire.vk_color_space != VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
			{
				LOGE("Unrecognized sRGB color space used.\n");
				return send_message(fd, MessageType::ErrorParameter, image_create.get_serial());
			}

			color_space = VkColorSpaceKHR(image_create.wire.vk_color_space);

			LOGI("Received image group request: format %d, color space %d\n",
			     image_create.wire.vk_format, image_create.wire.vk_color_space);

			VkImageFormatListCreateInfoKHR format_info = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR };
			if (image_create.wire.vk_num_view_formats)
			{
				format_info.viewFormatCount = image_create.wire.vk_num_view_formats;
				format_info.pViewFormats = reinterpret_cast<const VkFormat *>(image_create.wire.vk_view_formats);
				info.pnext = &format_info;

				for (uint32_t i = 0; i < format_info.viewFormatCount; i++)
				{
					if (format_info.pViewFormats[i] == VK_FORMAT_R8G8B8A8_UNORM && info.format == VK_FORMAT_R8G8B8A8_SRGB)
						info.misc |= Vulkan::IMAGE_MISC_MUTABLE_SRGB_BIT;
					else if (format_info.pViewFormats[i] == VK_FORMAT_B8G8R8A8_UNORM && info.format == VK_FORMAT_B8G8R8A8_SRGB)
						info.misc |= Vulkan::IMAGE_MISC_MUTABLE_SRGB_BIT;
					if (format_info.pViewFormats[i] == VK_FORMAT_R8G8B8A8_SRGB && info.format == VK_FORMAT_R8G8B8A8_UNORM)
						info.misc |= Vulkan::IMAGE_MISC_MUTABLE_SRGB_BIT;
					else if (format_info.pViewFormats[i] == VK_FORMAT_B8G8R8A8_SRGB && info.format == VK_FORMAT_B8G8R8A8_UNORM)
						info.misc |= Vulkan::IMAGE_MISC_MUTABLE_SRGB_BIT;
				}
			}
			else if (Vulkan::format_is_srgb(info.format))
			{
				LOGE("Format is sRGB, but must be created with mutable format.\n");
				return send_message(fd, MessageType::ErrorParameter, image_create.get_serial());
			}

			// Sanity check dimensions.
			VkImageFormatProperties2 props2 = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
			if (!device.get_image_format_properties(info.format, info.type, VK_IMAGE_TILING_OPTIMAL,
			                                        info.usage, info.flags, nullptr, &props2))
			{
				return send_message(fd, MessageType::ErrorParameter, image_create.get_serial());
			}

			if (info.width > props2.imageFormatProperties.maxExtent.width || info.width == 0 ||
			    info.height > props2.imageFormatProperties.maxExtent.height || info.height == 0)
			{
				return send_message(fd, MessageType::ErrorParameter, image_create.get_serial());
			}

			for (uint32_t i = 0; i < image_create.wire.num_images; i++)
			{
				info.external.memory_handle_type =
						static_cast<VkExternalMemoryHandleTypeFlagBits>(image_create.wire.vk_external_memory_type);
				info.external.handle = image_create.fds[i].get_native_handle();

				auto cross_info = info;
				cross_info.external = {};
				cross_info.misc &= ~Vulkan::IMAGE_MISC_EXTERNAL_MEMORY_BIT;
				cross_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

				SwapchainImage image;
				image.image = device.create_image(info);

				// Cross device scenario. Need to use external host memory and roundtrip through sysmem.
				if (&device != server.encoder_device && server.encoder_device)
				{
					auto layer_size = Vulkan::format_get_layer_size(info.format, VK_IMAGE_ASPECT_COLOR_BIT,
					                                                info.width, info.height, 1);
					Vulkan::BufferCreateInfo cross_buffer_info = {};
					cross_buffer_info.size = layer_size;
					cross_buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
					cross_buffer_info.domain = Vulkan::BufferDomain::CachedHost;

					if (device.get_device_features().supports_external_memory_host &&
					    server.encoder_device->get_device_features().supports_external_memory_host)
					{
						LOGI("Creating cross-device buffer.\n");
						// May require up to 64k alignment.
						image.cross_device_host_pointer.reset(
								static_cast<uint8_t *>(Util::memalign_alloc(64 * 1024, layer_size)));
					}

					if (image.cross_device_host_pointer)
					{
						image.src_cross_device_buffer = device.create_imported_host_buffer(
								cross_buffer_info, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
								image.cross_device_host_pointer.get());

						image.dst_cross_device_buffer = server.encoder_device->create_imported_host_buffer(
								cross_buffer_info, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
								image.cross_device_host_pointer.get());

						if (!image.src_cross_device_buffer || !image.dst_cross_device_buffer)
						{
							LOGE("Failed to create cross device buffer.\n");
							image.cross_device_host_pointer.reset();
							image.src_cross_device_buffer.reset();
							image.dst_cross_device_buffer.reset();
						}
					}

					if (!image.cross_device_host_pointer)
					{
						LOGW("Falling back to manual buffer copy path on CPU.\n");
						image.src_cross_device_buffer = device.create_buffer(cross_buffer_info);
						cross_buffer_info.domain = Vulkan::BufferDomain::Host;
						image.dst_cross_device_buffer = server.encoder_device->create_buffer(cross_buffer_info);
					}

					image.dst_cross_device_image = server.encoder_device->create_image(cross_info);
				}

				if (image.image)
				{
					image_create.fds[i].release();
				}
				else
				{
					send_message(fd, MessageType::Error, image_create.get_serial());
					return false;
				}

				images.push_back(std::move(image));
			}

			return send_message(fd, MessageType::OK, image_create.get_serial());
		}

		bool handle_present(const FileHandle &fd, PresentImageMessage &present)
		{
			auto &device = association.ctx->device;

			if (present.wire.image_group_serial != image_group_serial)
				return send_message(fd, MessageType::ErrorParameter, present.get_serial());
			if (present.wire.index >= images.size())
				return send_message(fd, MessageType::ErrorProtocol, present.get_serial());

			if (present.wire.id <= last_present_id)
				return send_message(fd, MessageType::ErrorParameter, present.get_serial());
			last_present_id = present.wire.id;

			auto &img = images[present.wire.index];
			if (img.state != State::ClientOwned)
				return send_message(fd, MessageType::ErrorProtocol, present.get_serial());

			Vulkan::Semaphore sem;
			Vulkan::CommandBuffer::Type cmd_type =
					img.src_cross_device_buffer ? Vulkan::CommandBuffer::Type::AsyncTransfer :
					Vulkan::CommandBuffer::Type::AsyncCompute;

			if (present.wire.vk_external_semaphore_type != 0)
			{
				sem = device.request_semaphore_external(
						VK_SEMAPHORE_TYPE_BINARY_KHR,
						static_cast<VkExternalSemaphoreHandleTypeFlagBits>(present.wire.vk_external_semaphore_type));

				if (!sem)
				{
					send_message(fd, MessageType::Error, present.get_serial());
					LOGE("Server: failed to create semaphore.\n");
					return false;
				}

				Vulkan::ExternalHandle h;
				h.handle = present.fd.get_native_handle();
				h.semaphore_handle_type = sem->get_external_handle_type();
				if (sem->import_from_handle(h))
				{
					present.fd.release();
				}
				else
				{
					send_message(fd, MessageType::Error, present.get_serial());
					LOGE("Server: failed to import from handle.\n");
					return false;
				}

				device.add_wait_semaphore(cmd_type, std::move(sem), 0, true);
			}

			auto old_layout = static_cast<VkImageLayout>(present.wire.vk_old_layout);
			auto new_layout = static_cast<VkImageLayout>(present.wire.vk_new_layout);
			img.target_period = present.wire.period;
			img.target_timestamp = compute_next_target_timestamp();
			img.present_id = present.wire.id;
			img.pts = Util::get_current_time_nsecs() / 1000;
			img.state = State::PresentQueued;
			img.event = { server.group.get_timeline_trace_file(), "PresentQueue", present.wire.index };

			// No future present can be locked in earlier than this one.
			earliest_next_timestamp = img.target_timestamp + img.target_period;

			auto cmd = device.request_command_buffer(cmd_type);

			if (img.src_cross_device_buffer)
			{
				cmd->acquire_image_barrier(*img.image, old_layout, new_layout,
				                           VK_PIPELINE_STAGE_2_COPY_BIT,
				                           new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ?
				                           VK_ACCESS_TRANSFER_READ_BIT : 0);

				if (new_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
				{
					cmd->image_barrier(*img.image, new_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					                   VK_PIPELINE_STAGE_2_COPY_BIT, 0,
					                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_READ_BIT);
				}

				cmd->copy_image_to_buffer(*img.src_cross_device_buffer, *img.image, 0, {},
				                          { img.image->get_width(), img.image->get_height(), 1 },
				                          0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });

				cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
			}
			else
			{
				cmd->acquire_image_barrier(*img.image, old_layout, new_layout, 0, 0);

				if (new_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
				{
					cmd->image_barrier(*img.image, new_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
					                   0, 0);
				}
			}

			Vulkan::Fence fence;
			device.submit(cmd, &fence);

			// Mark the buffer async.
			add_reference();
			server.group.create_task(
					[this, index = present.wire.index, serial = image_group_serial, f = std::move(fence)]() mutable {
						f->wait();
						const uint64_t buf[2] = { serial, index };
						ssize_t ret = ::write(pipe_fd.get_native_handle(), buf, sizeof(buf));
						if (ret < 0 && errno != EPIPE)
							LOGE("Failed to write to pipe.\n");
						release_reference();
					});

			if (!send_message(fd, MessageType::OK, present.get_serial()))
				return false;

			if (!retire_obsolete_images())
				return false;

			return true;
		}

		uint64_t compute_next_target_timestamp() const
		{
			// If there are no pending presentations in flight, lock-in for the next cycle.
			// Move any target forward to next pending timestamp.
			uint64_t ts = timestamp_completed + 1;
			if (ts < earliest_next_timestamp)
				ts = earliest_next_timestamp;

			return ts;
		}

		bool handle_async(const FileHandle &fd)
		{
			uint64_t buf[2];
			if (size_t(::read(fd.get_native_handle(), buf, sizeof(buf))) != sizeof(buf))
				return false;

			// Sentinel.
			if (buf[1] == uint64_t(-1))
				return false;

			// Out of date event, just ignore.
			if (buf[0] != image_group_serial)
				return true;

			uint64_t index = buf[1];
			assert(index < images.size());

			assert(images[index].state == State::PresentQueued);
			images[index].state = State::PresentReady;
			images[index].event = { server.group.get_timeline_trace_file(), "PresentReady", uint32_t(index) };

			// If we're using immediate mode, we'll want to encode this frame right away and send it.
			// It will be retired late.
			timestamp_complete_mappings.push_back({ images[index].target_timestamp, images[index].present_id });
			server.notify_async_surface({ this, int(index) });

			// If we're doing immediate encode, every user image can be retired now.
			// We've already blitted it.
			if (server.video_encode.immediate)
			{
				images[index].event = {};
				return retire_obsolete_images(UINT64_MAX);
			}
			else
				return retire_obsolete_images();
		}

		bool handle(const FileHandle &fd, uint32_t id) override
		{
			if (id != 0)
				return handle_async(fd);

			auto msg = parse_message(fd);
			if (!msg)
				return false;

			if (auto *image_create = maybe_get<ImageGroupMessage>(*msg))
			{
				return handle_swapchain_create(fd, *image_create);
			}
			else if (auto *present_msg = maybe_get<PresentImageMessage>(*msg))
			{
				return handle_present(fd, *present_msg);
			}
			else
			{
				send_message(fd, MessageType::ErrorProtocol, msg->get_serial());
				return false;
			}
		}

		bool send_acquire_retire(uint32_t index)
		{
			auto &img = images[index];
			// Synchronous acquire. Retire == acquire more or less.
			AcquireImageMessage::WireFormat acquire = {};
			acquire.index = index;
			acquire.image_group_serial = image_group_serial;

			// If there's a pending blit pass, have to handle write-after-read.
			FileHandle fd;
			if (img.last_read_semaphore)
			{
				auto h = img.last_read_semaphore->export_to_handle();
				fd = FileHandle{h.handle};
				acquire.vk_external_semaphore_type = h.semaphore_handle_type;
				img.last_read_semaphore.reset();
			}

			if (!send_wire_message(async_fd, 0, acquire, &fd, fd ? 1 : 0))
				return false;

			RetireImageMessage::WireFormat retire = {};
			retire.image_group_serial = image_group_serial;
			retire.index = index;
			if (!send_wire_message(async_fd, 0, retire))
				return false;

			return true;
		}

		bool retire_obsolete_images(uint64_t current_present_id)
		{
			for (size_t i = 0, n = images.size(); i < n; i++)
			{
				auto &img = images[i];
				if (img.state == State::PresentReady && img.present_id < current_present_id)
				{
					img.state = State::ClientOwned;
					if (!send_acquire_retire(i))
						return false;
				}
			}

			return true;
		}

		bool retire_obsolete_images()
		{
			int target_index = get_target_image_index_for_timestamp(timestamp_completed + 1);
			return target_index < 0 || retire_obsolete_images(images[target_index].present_id);
		}

		int get_target_image_index_for_timestamp(uint64_t ts)
		{
			int target_index = -1;
			for (size_t i = 0, n = images.size(); i < n; i++)
			{
				auto &img = images[i];
				if (img.state != State::PresentReady)
					continue;

				if (img.target_timestamp > ts)
					continue;

				// Among candidates, pick the one with largest present ID.
				if (target_index < 0 || img.present_id > images[target_index].present_id)
					target_index = int(i);
			}

			return target_index;
		}

		bool heartbeat_stalled(uint64_t)
		{
			timestamp_completed++;
			timestamp_stalled_count++;
			LOGW("Frame dropped. Total %llu, dropped %llu.\n",
			     static_cast<unsigned long long>(timestamp_completed),
			     static_cast<unsigned long long>(timestamp_stalled_count));
			return retire_obsolete_images();
		}

		bool heartbeat(uint64_t time_ns, int &scanout_index)
		{
			timestamp_completed++;
			scanout_index = -1;
			if (images.empty())
				return true;

			scanout_index = get_target_image_index_for_timestamp(timestamp_completed);

			uint64_t complete_id = 0;
			if (scanout_index >= 0)
				images[scanout_index].event = {};

			// Mark completion IDs based on current timestamp.
			for (auto &mapping : timestamp_complete_mappings)
				if (timestamp_completed >= mapping.timestamp && mapping.complete_id > complete_id)
					complete_id = mapping.complete_id;

			// Retire timestamps that we have processed.
			auto itr = std::remove_if(timestamp_complete_mappings.begin(),
			                          timestamp_complete_mappings.end(),
			                          [this](const TimestampCompleteMapping &m) {
				                          return m.timestamp <= timestamp_completed;
			                          });
			timestamp_complete_mappings.erase(itr, timestamp_complete_mappings.end());

			if (complete_id)
			{
				FrameCompleteMessage::WireFormat complete = {};
				complete.image_group_serial = image_group_serial;
				complete.period_ns = time_ns;
				complete.presented_id = complete_id;
				complete.timestamp = timestamp_completed;
				if (!send_wire_message(async_fd, 0, complete))
					return false;
				if (!retire_obsolete_images(complete_id))
					return false;
			}

			return true;
		}

		enum class State : unsigned
		{
			ClientOwned,
			PresentQueued,
			PresentReady
		};

		struct SwapchainImage
		{
			Vulkan::ImageHandle image;
			Vulkan::Semaphore last_read_semaphore;
			uint64_t target_timestamp = 0;
			uint32_t target_period = 0;
			uint64_t present_id = 0;
			int64_t pts = 0;
			State state = State::ClientOwned;
			Util::TimelineTraceFile::ScopedEvent event;

			struct Deleter { void operator()(uint8_t *ptr) { Util::memalign_free(ptr); }};
			std::unique_ptr<uint8_t, Deleter> cross_device_host_pointer;
			Vulkan::BufferHandle src_cross_device_buffer;
			Vulkan::BufferHandle dst_cross_device_buffer;
			Vulkan::ImageHandle dst_cross_device_image;
		};

		SwapchainServer &server;
		DeviceContextAssociation association = {};
		std::vector<SwapchainImage> images;
		VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

		struct TimestampCompleteMapping
		{
			uint64_t timestamp;
			uint64_t complete_id;
		};
		std::vector<TimestampCompleteMapping> timestamp_complete_mappings;

		uint64_t image_group_serial = 0;
		uint64_t timestamp_completed = 0;
		uint64_t timestamp_stalled_count = 0;
		uint64_t last_present_id = 0;
		uint64_t earliest_next_timestamp = 0;
		FileHandle async_fd;
		FileHandle pipe_fd;
	};

	bool heartbeat_stalled(uint64_t period_ns)
	{
		for (auto &handler : handlers)
		{
			if (!handler->heartbeat_stalled(period_ns))
			{
				dispatcher.cancel_connection(handler.get(), 0);
				return false;
			}
		}

		return true;
	}

	struct ReadySurface
	{
		Swapchain *chain;
		int index;
		const Vulkan::Image *img;
	};

	void encode_surface(const ReadySurface &surface, uint64_t period_ns)
	{
		Granite::VideoEncoder::YCbCrPipeline *ycbcr_pipeline = nullptr;
		if (encoder)
			ycbcr_pipeline = &pipeline[next_encode_task_slot];

		client_heartbeat_count++;
		bool encode_frame = client_heartbeat_count >= client_rate_multiplier;
		if (encode_frame)
			client_heartbeat_count = 0;

		if (encoder && encoder_device && ycbcr_pipeline && encode_frame)
		{
			auto pts = encoder->sample_realtime_pts();

			// Composite the final YCbCr frame here.
			// For now, just select one candidate and pretend it's the foreground flip.
			auto cmd = encoder_device->request_command_buffer(Vulkan::CommandBuffer::Type::AsyncCompute);

			if (surface.img)
			{
				// External image from pipewire. Acquire and release properly from external queue.
				cmd->acquire_image_barrier(*surface.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
				encoder->process_rgb(*cmd, *ycbcr_pipeline, surface.img->get_view(), VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
				cmd->release_image_barrier(*surface.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
				                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0);
			}
			else if (!surface.chain)
			{
				// Some dummy background thing.
				auto info = Vulkan::ImageCreateInfo::immutable_2d_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM);
				info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
				info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
				info.misc |= Vulkan::IMAGE_MISC_MUTABLE_SRGB_BIT;
				auto img = encoder_device->create_image(info);
				cmd->image_barrier(*img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				                   0, 0,
				                   VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

				VkClearValue value = {};
				value.color.float32[0] = 0.1f;
				value.color.float32[1] = 0.2f;
				value.color.float32[2] = 0.3f;
				cmd->clear_image(*img, value);
				cmd->image_barrier(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				                   VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
				encoder->process_rgb(*cmd, *ycbcr_pipeline, img->get_view());
			}
			else
			{
				auto &surf = surface.chain->images[surface.index];
				const Vulkan::ImageView *view;

				if (surf.src_cross_device_buffer)
				{
					// For cross device, the memory is in system memory once the fence has signalled.
					// If we have external host memory, we can bypass the memcpy since both GPUs will see the system memory.
					if (!surf.cross_device_host_pointer)
					{
						auto &src_device = surface.chain->association.ctx->device;
						auto *src = src_device.map_host_buffer(*surf.src_cross_device_buffer, Vulkan::MEMORY_ACCESS_READ_BIT);
						auto *dst = encoder_device->map_host_buffer(*surf.dst_cross_device_buffer, Vulkan::MEMORY_ACCESS_WRITE_BIT);
						memcpy(dst, src, surf.src_cross_device_buffer->get_create_info().size);
						src_device.unmap_host_buffer(*surf.src_cross_device_buffer, Vulkan::MEMORY_ACCESS_READ_BIT);
						encoder_device->unmap_host_buffer(*surf.dst_cross_device_buffer, Vulkan::MEMORY_ACCESS_WRITE_BIT);
					}

					cmd->image_barrier(*surf.dst_cross_device_image, VK_IMAGE_LAYOUT_UNDEFINED,
					                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
					                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

					cmd->copy_buffer_to_image(*surf.dst_cross_device_image, *surf.dst_cross_device_buffer,
					                          0, {}, { surf.dst_cross_device_image->get_width(), surf.dst_cross_device_image->get_height(), 1 },
					                          0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });

					cmd->image_barrier(*surf.dst_cross_device_image,
					                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
					                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

					view = &surf.dst_cross_device_image->get_view();
				}
				else
				{
					view = &surf.image->get_view();
				}

				if (view)
					encoder->process_rgb(*cmd, *ycbcr_pipeline, *view, surface.chain->color_space);
			}

			encoder->submit_process_rgb(cmd, *ycbcr_pipeline);

			// Need one binary semaphore for every composited surface.
			// Relying on external timelines would be nice though,
			// but won't play nice with WSI layering :(
			int compensate_audio_us = 0;
			if (surface.chain)
			{
				auto &surf = surface.chain->images[surface.index];

				// If we're cross device, we're done reading the image when the present fence signals,
				// so don't need a semaphore. It will also not work since we have to send a semaphore
				// for the expected device.
				if (&surface.chain->association.ctx->device == encoder_device)
				{
					auto sem = encoder_device->request_semaphore_external(
							VK_SEMAPHORE_TYPE_BINARY_KHR, Vulkan::ExternalHandle::get_opaque_semaphore_handle_type());
					encoder_device->submit_empty(Vulkan::CommandBuffer::Type::AsyncCompute, nullptr, sem.get());
					surf.last_read_semaphore = std::move(sem);
				}

				// Compensate audio latency with FIFO latency here.
				// Somewhat crude, but hey.
				// Our PTS really should be a little in the past since that was when the frame was presented to us.
				// Compensate by delaying audio instead.
				compensate_audio_us = int(surface.chain->images.size() - 1) *
				                      int(surf.target_period) *
				                      int(period_ns / 1000);
			}

			encode_tasks[next_encode_task_slot] = group.create_task(
					[this, ycbcr_pipeline, pts, compensate_audio_us]()
					{
						if (!encoder->encode_frame(*ycbcr_pipeline, pts, compensate_audio_us))
							LOGE("Failed to encode frame.\n");
					});

			encode_tasks[next_encode_task_slot]->set_desc("FFmpeg encode");

			// Ensure ordering between encode operations.
			if (last_encode_dependency)
				group.add_dependency(*encode_tasks[next_encode_task_slot], *last_encode_dependency);
			last_encode_dependency = group.create_task();
			group.add_dependency(*last_encode_dependency, *encode_tasks[next_encode_task_slot]);
			encode_tasks[next_encode_task_slot]->flush();

			next_encode_task_slot = (next_encode_task_slot + 1) % NumEncodeTasks;
		}

		for (auto &gpu : gpus)
			if (gpu.context)
				gpu.context->device.next_frame_context();
	}

	void notify_async_surface(const ReadySurface &surface)
	{
		// Defer encode / composite to heartbeat vblank.
		if (!video_encode.immediate)
			return;

		// If video encode threads are busy, defer.
		if (encode_tasks[next_encode_task_slot] && !encode_tasks[next_encode_task_slot]->poll())
			return;
		encode_tasks[next_encode_task_slot].reset();

		// Ignore period since we're doing unlocked rendering.
		encode_surface(surface, 0);
	}

	bool heartbeat(uint64_t period_ns)
	{
		// Only relevant if we performed encode out of band.
		if (!video_encode.immediate && encode_tasks[next_encode_task_slot])
		{
			if (!encode_tasks[next_encode_task_slot]->poll())
			{
				// Encoding is too slow. This is considered a stalled heartbeat.
				return heartbeat_stalled(period_ns);
			}
			encode_tasks[next_encode_task_slot].reset();
		}

		// Latch ready surfaces. If we did out of band encode, we will signal the completion here
		// to ensure stable frame pacing.
		ReadySurface ready_surface = {};

		for (auto &handler : handlers)
		{
			int index;
			if (!handler->heartbeat(period_ns, index))
			{
				dispatcher.cancel_connection(handler.get(), 0);
				return false;
			}

			if (index >= 0)
				ready_surface = { handler.get(), index };
		}

		// If we're using pipewire, never encode dummy frames since we won't have normal handlers.
		if (!video_encode.pipewire && (!video_encode.immediate || handlers.empty()))
			encode_surface(ready_surface, period_ns);
		return true;
	}

	bool register_tcp_handler(Dispatcher &dispatcher_, const FileHandle &fd,
	                          const RemoteAddress &remote, Handler *&handler) override
	{
		return pyro.register_tcp_handler(dispatcher_, fd, remote, handler);
	}

	void handle_udp_datagram(Dispatcher &dispatcher_, const RemoteAddress &remote,
	                         const void *msg, unsigned size) override
	{
		pyro.handle_udp_datagram(dispatcher_, remote, msg, size);
		if (const auto *state = pyro.get_updated_gamepad_state())
			uinput.report_state(*state);
	}

	bool register_handler(Dispatcher &dispatcher_, const FileHandle &fd, Handler *&handler) override
	{
		auto msg = parse_message(fd);
		if (!msg)
			return false;

		if (auto *hello = maybe_get<ClientHelloMessage>(*msg))
		{
			if (hello->wire.intent != ClientIntent::VulkanExternalStream)
			{
				send_message(fd, MessageType::ErrorProtocol, msg->get_serial());
				return false;
			}

			ServerHelloMessage::WireFormat server_hello = {};
			return send_wire_message(fd, msg->get_serial(), server_hello);
		}
		else if (auto *device = maybe_get<DeviceMessage>(*msg))
		{
			auto swap = Util::make_handle<Swapchain>(dispatcher_, *this);
			swap->async_fd = fd.dup();

			if (create_device(swap->association,
			                  device->wire.device_uuid,
			                  device->wire.driver_uuid,
			                  device->wire.luid, device->wire.luid_valid))
			{
				swap->add_reference();
				handler = swap.get();
				handlers.push_back(std::move(swap));
				return send_message(fd, MessageType::OK, msg->get_serial());
			}
			else
				return send_message(fd, MessageType::Error, msg->get_serial());
		}
		else
		{
			send_message(fd, MessageType::ErrorProtocol, msg->get_serial());
			return false;
		}
	}

	bool create_device(DeviceContextAssociation &association,
	                   const uint8_t (&device_uuid)[VK_UUID_SIZE],
	                   const uint8_t (&driver_uuid)[VK_UUID_SIZE],
	                   const uint8_t (&luid)[VK_LUID_SIZE], VkBool32 luid_valid)
	{
		DeviceContext *target_context = nullptr;
		unsigned gpu_index = 0;
		for (auto &gpu : gpus)
		{
			bool luid_equal = luid_valid && gpu.luid_valid && memcmp(gpu.luid, luid, VK_LUID_SIZE) == 0;
			bool id_equal = memcmp(gpu.device_uuid, device_uuid, VK_UUID_SIZE) == 0 &&
			                memcmp(gpu.driver_id, driver_uuid, VK_UUID_SIZE) == 0;

			if (luid_equal || id_equal)
			{
				if (!init_encoder_for_device(gpu_index))
					return false;

				target_context = gpu.context.get();
				break;
			}

			gpu_index++;
		}

		if (target_context)
		{
			DeviceContextAssociation assoc = {};
			assoc.ctx = target_context;
			associations.push_back(assoc);
			association = assoc;
		}
		else
			association = {};

		return target_context != nullptr;
	}

	VkInstance create_instance(const VkInstanceCreateInfo *) override
	{
		return instance.get();
	}

	enum { NumEncodeTasks = 8 };
	struct PhysicalDevice
	{
		VkPhysicalDevice gpu;
		uint8_t device_uuid[VK_UUID_SIZE];
		uint8_t driver_id[VK_UUID_SIZE];
		uint8_t luid[VK_UUID_SIZE];
		VkBool32 luid_valid;
		std::unique_ptr<DeviceContext> context;
	};

	Granite::ThreadGroup group;
	Dispatcher &dispatcher;
	std::unique_ptr<VkInstance_T, InstanceDeleter> instance;
	std::vector<Util::IntrusivePtr<Swapchain>> handlers;
	std::vector<PhysicalDevice> gpus;
	std::vector<DeviceContextAssociation> associations;

	Granite::VideoEncoder::YCbCrPipeline pipeline[NumEncodeTasks];
	std::unique_ptr<Granite::VideoEncoder> encoder;
	Vulkan::Device *encoder_device = nullptr;

	// Audio recorder must be destroyed before encoder.
	std::unique_ptr<Granite::Audio::RecordStream> audio_record;

	Granite::TaskGroupHandle last_encode_dependency;
	Granite::TaskGroupHandle encode_tasks[NumEncodeTasks];
	unsigned next_encode_task_slot = 0;
	VirtualGamepad uinput;

	PyroStreamServer pyro;

	struct Options
	{
		std::string path;
		unsigned width = 0;
		unsigned height = 0;
		unsigned fps = 0;

		unsigned bitrate_kbits = 6000;
		unsigned max_bitrate_kbits = 8000;
		unsigned vbv_size_kbits = 6000;
		unsigned threads = 0;
		unsigned audio_rate = 44100;
		float gop_seconds = 2.0f;
		bool low_latency = false;
		bool audio = true;
		bool immediate = false;
		unsigned bit_depth = 8;
		bool hdr10 = false;
		bool fec = false;
		bool walltime_to_pts = true;
		bool pipewire = false;
		bool chroma_444 = false;
		std::string x264_preset = "fast";
		std::string x264_tune;
		std::string local_backup_path;
		std::string encoder = "libx264";
		std::string muxer;
	} video_encode;

	unsigned client_rate_multiplier = 1;
	unsigned client_heartbeat_count = 0;

	void set_encode_options(const Options &opts)
	{
		video_encode = opts;
	}

	void set_client_rate_multiplier(unsigned rate)
	{
		client_rate_multiplier = rate;
		client_heartbeat_count = 0;
	}

	bool init_encoder_for_device(unsigned index)
	{
		if (index >= gpus.size())
		{
			LOGE("Device index %u out of bounds (%zu GPUs in system).\n", index, gpus.size());
			return false;
		}

		auto &gpu = gpus[index];

		if (!gpu.context)
		{
			gpu.context = std::make_unique<DeviceContext>();

			Vulkan::Context::SystemHandles handles = {};
			handles.thread_group = &group;
			handles.timeline_trace_file = group.get_timeline_trace_file();
			gpu.context->context.set_num_thread_indices(group.get_num_threads() + 1);
			gpu.context->context.set_system_handles(handles);

			gpu.context->context.set_instance_factory(this);
			if (!gpu.context->context.init_instance(nullptr, 0,
			                                        Vulkan::CONTEXT_CREATION_ENABLE_VIDEO_H264_BIT |
			                                        Vulkan::CONTEXT_CREATION_ENABLE_VIDEO_H265_BIT |
			                                        Vulkan::CONTEXT_CREATION_ENABLE_VIDEO_ENCODE_BIT |
			                                        Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT))
				return false;

			gpu.context->context.release_instance();
			if (!gpu.context->context.init_device(gpu.gpu, VK_NULL_HANDLE, nullptr, 0,
			                                      Vulkan::CONTEXT_CREATION_ENABLE_VIDEO_H264_BIT |
			                                      Vulkan::CONTEXT_CREATION_ENABLE_VIDEO_H265_BIT |
			                                      Vulkan::CONTEXT_CREATION_ENABLE_VIDEO_ENCODE_BIT |
			                                      Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT))
			{
				gpu.context.reset();
				return false;
			}

			gpu.context->device.set_context(gpu.context->context);
		}

		// Let a "primary" GPU be the composition owner.
		if (!encoder)
		{
			encoder = std::make_unique<Granite::VideoEncoder>();
			Granite::VideoEncoder::Options options = {};
			options.width = video_encode.width;
			options.height = video_encode.height;
			options.frame_timebase.num = 1;
			options.frame_timebase.den = int(video_encode.fps);
			options.encoder = video_encode.encoder.c_str();
			options.walltime_to_pts = video_encode.walltime_to_pts;

			// For now, just assume the inputs are properly PQ encoded.
			// TODO: Deal with color space conversion as needed, etc.
			options.hdr10 = video_encode.hdr10;

			if (!video_encode.muxer.empty())
				options.muxer_format = video_encode.muxer.c_str();
			else if (video_encode.path.find("://") != std::string::npos)
				options.muxer_format = "flv";

			options.bitrate_kbits = video_encode.bitrate_kbits;
			options.max_bitrate_kbits = video_encode.max_bitrate_kbits;
			options.gop_seconds = video_encode.gop_seconds;
			options.low_latency = video_encode.low_latency;
			options.vbv_size_kbits = video_encode.vbv_size_kbits;
			options.x264_preset = video_encode.x264_preset.empty() ? nullptr : video_encode.x264_preset.c_str();
			options.x264_tune = video_encode.x264_tune.empty() ? nullptr : video_encode.x264_tune.c_str();
			options.threads = video_encode.threads;
			options.local_backup_path = video_encode.local_backup_path.empty() ? nullptr : video_encode.local_backup_path.c_str();

			// Software codecs in FFmpeg all use yuv420p.
			options.format = video_encode.chroma_444 ?
			                 Granite::VideoEncoder::Format::YUV444P :
							 Granite::VideoEncoder::Format::YUV420P;

			if (video_encode.bit_depth > 8 && options.encoder)
			{
				if (strstr(options.encoder, "nvenc") != nullptr)
					options.format = Granite::VideoEncoder::Format::P016;
				else if (strstr(options.encoder, "vaapi") != nullptr)
					options.format = Granite::VideoEncoder::Format::P010;
				else if (strstr(options.encoder, "pyro") != nullptr)
					options.format = Granite::VideoEncoder::Format::P016;
			}

			if (options.hdr10 && (strcmp(options.encoder, "pyrowave") == 0 || strcmp(options.encoder, "rawvideo") == 0))
			{
				options.format = video_encode.chroma_444 ?
				                 Granite::VideoEncoder::Format::YUV444P16 :
				                 Granite::VideoEncoder::Format::YUV420P16;
			}
			else if (options.format == Granite::VideoEncoder::Format::YUV420P &&
			         (strstr(options.encoder, "nvenc") != nullptr ||
			          strstr(options.encoder, "vaapi") != nullptr ||
			          strstr(options.encoder, "_pyro") != nullptr))
			{
				// GPU encoders only understand NV12.
				options.format = Granite::VideoEncoder::Format::NV12;
			}

			if (video_encode.audio)
				audio_record.reset(Granite::Audio::create_default_audio_record_backend("Stream", float(video_encode.audio_rate), 2));

			pyro.set_forward_error_correction(video_encode.fec);
			pyro.set_idr_on_packet_loss(video_encode.gop_seconds < 0.0f);
			encoder->set_audio_record_stream(audio_record.get());
			if (video_encode.path.empty())
				encoder->set_mux_stream_callback(this);
			encoder_device = &gpu.context->device;
			if (encoder->init(encoder_device, video_encode.path.empty() ? nullptr : video_encode.path.c_str(), options))
			{
				Vulkan::ResourceLayout layout;
				FFmpegEncode::Shaders<> bank{gpu.context->device, layout, 0};

				for (auto &pipe : pipeline)
					pipe = encoder->create_ycbcr_pipeline(bank);

				if (audio_record && !audio_record->start())
				{
					LOGE("Failed to initialize audio recorder.\n");
					encoder.reset();
					encoder_device = nullptr;
					audio_record.reset();
					return false;
				}
			}
			else
			{
				encoder.reset();
				encoder_device = nullptr;
				audio_record.reset();
				LOGE("Failed to initialize encoder.\n");
				return false;
			}
		}

		return true;
	}

	void unregister_handler(Swapchain *handler)
	{
		auto itr = std::find_if(handlers.begin(), handlers.end(), [handler](const Util::IntrusivePtr<Swapchain> &ptr_handler) {
			return ptr_handler.get() == handler;
		});
		assert(itr != handlers.end());
		handlers.erase(itr);
	}

	void set_codec_parameters(const pyro_codec_parameters &codec) override
	{
		pyro.set_codec_parameters(codec);
	}

	void write_video_packet(int64_t pts, int64_t dts, const void *data, size_t size, bool is_key_frame) override
	{
		pyro.write_video_packet(pts, dts, data, size, is_key_frame);
	}

	void write_audio_packet(int64_t pts, int64_t dts, const void *data, size_t size) override
	{
		pyro.write_audio_packet(pts, dts, data, size);
	}

	bool should_force_idr() override
	{
		return pyro.should_force_idr();
	}
};

#ifdef HAVE_PIPEWIRE
void PipewireStream::send_encoding(const Vulkan::Image &img)
{
	server.encode_surface({ nullptr, -1, &img }, 0);
}
#endif

struct HeartbeatHandler final : Handler
{
	HeartbeatHandler(Dispatcher &dispatcher_, SwapchainServer &server_, unsigned fps)
			: Handler(dispatcher_), server(server_), timebase_ns(1000000000u / fps)
	{
		// Nudge the timebase by up to 1% in 0.01% increments.
		timebase_ns_fraction = timebase_ns / 10000;
		target_interval_ns = timebase_ns;
	}

	bool handle(const FileHandle &fd, uint32_t) override
	{
		uint64_t timeouts = 0;
		if (::read(fd.get_native_handle(), &timeouts, sizeof(timeouts)) <= 0)
			return false;

		update_loop(fd, server.pyro.get_phase_offset_us());

		for (uint64_t i = 1; i < timeouts; i++)
		{
			if (!server.heartbeat_stalled(timebase_ns))
			{
				dispatcher.kill();
				return false;
			}
		}

		if (!server.heartbeat(timebase_ns))
		{
			dispatcher.kill();
			return false;
		}

		return true;
	}

	void release_id(uint32_t) override
	{
		delete this;
	}

	void update_loop(const FileHandle &fd, int phase_offset_us)
	{
		timespec tv = {};
		itimerspec itimer = {};
		clock_gettime(CLOCK_MONOTONIC, &tv);

		// The gettime result is always relative to next tick, regardless of ABSTIME being used.
		timerfd_gettime(fd.get_native_handle(), &itimer);

		uint64_t target_time_ns =
				(tv.tv_sec + itimer.it_value.tv_sec) * 1000000000ull +
				tv.tv_nsec + itimer.it_value.tv_nsec;

		// Add larger immediate offsets, but accelerate slowly.
		constexpr int respond_factor = 60;
		bool sets_time = false;

		if (abs(phase_offset_us) > 200)
		{
			if (phase_offset_us > 0 && tick_interval_offset < 50)
			{
				// Client want frame delivered later, increase interval.
				// Offset the interval more sharply so we can respond in time.
				tick_interval_offset += 2;
				target_time_ns += respond_factor * timebase_ns_fraction;
				target_interval_ns += 2 * timebase_ns_fraction;
				sets_time = true;
			}
			else if (phase_offset_us < 0 && tick_interval_offset > -50)
			{
				// Client want frame delivered sooner, reduce interval.
				// Offset the interval more sharply so we can respond in time.
				tick_interval_offset -= 2;
				target_time_ns -= respond_factor * timebase_ns_fraction;
				target_interval_ns -= 2 * timebase_ns_fraction;
				sets_time = true;
			}
		}

		// When we get no messages, move towards center.
		if (tick_interval_offset > 0)
		{
			tick_interval_offset--;
			target_interval_ns -= timebase_ns_fraction;
			sets_time = true;
		}
		else if (tick_interval_offset < 0)
		{
			tick_interval_offset++;
			target_interval_ns += timebase_ns_fraction;
			sets_time = true;
		}

		if (sets_time)
		{
			itimer.it_value.tv_nsec = long(target_time_ns % 1000000000);
			itimer.it_value.tv_sec = time_t(target_time_ns / 1000000000);
			itimer.it_interval.tv_nsec = long(target_interval_ns % 1000000000);
			itimer.it_interval.tv_sec = time_t(target_interval_ns / 1000000000);

			timerfd_settime(fd.get_native_handle(), TFD_TIMER_ABSTIME, &itimer, nullptr);
		}
	}

	SwapchainServer &server;
	uint64_t timebase_ns;
	uint64_t timebase_ns_fraction = 0;
	uint64_t target_interval_ns = 0;
	int tick_interval_offset = 0;
};

static void print_help()
{
	LOGI("Usage: pyrofling\n"
		 "\t[--socket SOCKET_PATH]\n"
		 "\t[--width WIDTH]\n"
		 "\t[--height HEIGHT]\n"
		 "\t[--fps FPS]\n"
		 "\t[--device-index INDEX]\n"
		 "\t[--client-rate-multiplier RATE]\n"
	     "\t[--threads THREADS]\n"
	     "\t[--preset PRESET]\n"
	     "\t[--tune PRESET]\n"
	     "\t[--gop-seconds GOP_SECONDS (negative for IDR-on-demand mode if intra-refresh is not supported)]\n"
	     "\t[--bitrate-kbits SIZE]\n"
	     "\t[--max-bitrate-kbits SIZE]\n"
	     "\t[--vbv-size-kbits SIZE]\n"
	     "\t[--local-backup PATH]\n"
	     "\t[--encoder ENCODER]\n"
	     "\t[--muxer MUXER]\n"
	     "\t[--port PORT]\n"
	     "\t[--audio-rate RATE]\n"
	     "\t[--low-latency]\n"
	     "\t[--no-audio]\n"
	     "\t[--immediate-encode]\n"
#ifdef HAVE_PIPEWIRE
		 "\t[--pipewire]\n"
#endif
		 "\turl\n");
}

static int main_inner(int argc, char **argv)
{
	std::string socket_path = "/tmp/pyrofling-socket";
	unsigned client_rate_multiplier = 1;
	SwapchainServer::Options opts;
	unsigned device_index = 0;
	std::string port;

	opts.width = 1280;
	opts.height = 720;
	opts.fps = 60;

	Util::CLICallbacks cbs;
	cbs.add("--fps", [&](Util::CLIParser &parser) { opts.fps = parser.next_uint(); });
	cbs.add("--client-rate-multiplier", [&](Util::CLIParser &parser) { client_rate_multiplier = parser.next_uint(); });
	cbs.add("--width", [&](Util::CLIParser &parser) { opts.width = parser.next_uint(); });
	cbs.add("--height", [&](Util::CLIParser &parser) { opts.height = parser.next_uint(); });
	cbs.add("--device-index", [&](Util::CLIParser &parser) { device_index = int(parser.next_uint()); });
	cbs.add("--help", [&](Util::CLIParser &parser) { parser.end(); });
	cbs.add("--socket", [&](Util::CLIParser &parser) { socket_path = parser.next_string(); });
	cbs.add("--gop-seconds", [&](Util::CLIParser &parser) { opts.gop_seconds = float(parser.next_double()); });
	cbs.add("--preset", [&](Util::CLIParser &parser) { opts.x264_preset = parser.next_string(); });
	cbs.add("--tune", [&](Util::CLIParser &parser) { opts.x264_tune = parser.next_string(); });
	cbs.add("--bitrate-kbits", [&](Util::CLIParser &parser) { opts.bitrate_kbits = parser.next_uint(); });
	cbs.add("--vbv-size-kbits", [&](Util::CLIParser &parser) { opts.vbv_size_kbits = parser.next_uint(); });
	cbs.add("--max-bitrate-kbits", [&](Util::CLIParser &parser) { opts.max_bitrate_kbits = parser.next_uint(); });
	cbs.add("--threads", [&](Util::CLIParser &parser) { opts.threads = parser.next_uint(); });
	cbs.add("--local-backup", [&](Util::CLIParser &parser) { opts.local_backup_path = parser.next_string(); });
	cbs.add("--encoder", [&](Util::CLIParser &parser) { opts.encoder = parser.next_string(); });
	cbs.add("--muxer", [&](Util::CLIParser &parser) { opts.muxer = parser.next_string(); });
	cbs.add("--port", [&](Util::CLIParser &parser) { port = parser.next_string(); });
	cbs.add("--audio-rate", [&](Util::CLIParser &parser) { opts.audio_rate = parser.next_uint(); });
	cbs.add("--low-latency", [&](Util::CLIParser &) { opts.low_latency = true; });
	cbs.add("--no-audio", [&](Util::CLIParser &) { opts.audio = false; });
	cbs.add("--immediate-encode", [&](Util::CLIParser &) { opts.immediate = true; });
	cbs.add("--10-bit", [&](Util::CLIParser &) { opts.bit_depth = 10; });
	cbs.add("--hdr10", [&](Util::CLIParser &) { opts.hdr10 = true; opts.bit_depth = 10; });
	cbs.add("--444", [&](Util::CLIParser &) { opts.chroma_444 = true; });
	cbs.add("--fec", [&](Util::CLIParser &) { opts.fec = true; });
	cbs.add("--offline", [&](Util::CLIParser &) { opts.walltime_to_pts = false; });
#ifdef HAVE_PIPEWIRE
	cbs.add("--pipewire", [&](Util::CLIParser &) { opts.pipewire = true; });
#endif
	cbs.default_handler = [&](const char *def) { opts.path = def; };

	Util::CLIParser parser(std::move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;

	if (parser.is_ended_state())
	{
		print_help();
		return EXIT_SUCCESS;
	}

	if (opts.path.empty() && port.empty())
	{
		LOGE("Encode URL required.\n");
		print_help();
		return EXIT_FAILURE;
	}

	if (!opts.path.empty() && !port.empty())
	{
		LOGE("Cannot use both TCP output and URL output.\n");
		print_help();
		return EXIT_FAILURE;
	}

	LOGI("Encoding: %u x %u @ %u fps (client %u fps) to \"%s\" || rate = %u kb/s || maxrate = %u kb/s || vbvsize = %u kb/s || gop = %f seconds\n",
	     opts.width, opts.height, opts.fps, opts.fps * client_rate_multiplier, opts.path.c_str(),
	     opts.bitrate_kbits, opts.max_bitrate_kbits, opts.vbv_size_kbits, opts.gop_seconds);

	Dispatcher dispatcher{socket_path.c_str(), port.c_str()};
	SwapchainServer server{dispatcher};
	server.set_client_rate_multiplier(client_rate_multiplier);
	server.set_encode_options(opts);
	if (!server.init_encoder_for_device(device_index))
		return EXIT_FAILURE;
	dispatcher.set_handler_factory_interface(&server);

	FileHandle timer_fd{timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC)};
	if (!timer_fd)
		return EXIT_FAILURE;

	struct itimerspec new_period = {};
	new_period.it_value.tv_nsec = 1000000000 / (opts.fps * client_rate_multiplier);
	new_period.it_interval.tv_nsec = 1000000000 / (opts.fps * client_rate_multiplier);
	if (timerfd_settime(timer_fd.get_native_handle(), 0, &new_period, nullptr) < 0)
		return EXIT_FAILURE;

	if (!dispatcher.add_connection(
			std::move(timer_fd), new HeartbeatHandler(dispatcher, server, opts.fps * client_rate_multiplier), 0,
			Dispatcher::ConnectionType::Input))
	{
		return EXIT_FAILURE;
	}

#ifdef HAVE_PIPEWIRE
	PipewireStream pipewire{dispatcher, server, *server.encoder_device};

	if (opts.pipewire)
	{
		if (pipewire.init(opts.width, opts.height, opts.fps))
		{
			FileHandle fd{dup(pipewire.get_fd())};
			dispatcher.add_connection(std::move(fd), &pipewire, 0, Dispatcher::ConnectionType::Input);
		}
		else
		{
			LOGE("Failed to initialize pipewire!\n");
		}
	}
#endif

	while (dispatcher.iterate()) {}
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	Dispatcher::block_signals();

	int ret;
	try
	{
		ret = main_inner(argc, argv);
	}
	catch (const std::exception &e)
	{
		LOGE("Caught fatal exception: %s.\n", e.what());
		ret = EXIT_FAILURE;
	}

	return ret;
}
