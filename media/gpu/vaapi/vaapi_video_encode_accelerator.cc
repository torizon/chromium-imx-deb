// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/vaapi/vaapi_video_encode_accelerator.h"

#include <string.h>
#include <va/va.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_vp8.h>

#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>

#include "base/bind.h"
#include "base/bits.h"
#include "base/callback.h"
#include "base/callback_helpers.h"
#include "base/containers/contains.h"
#include "base/cxx17_backports.h"
#include "base/feature_list.h"
#include "base/memory/ptr_util.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/task/post_task.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/trace_event/memory_dump_manager.h"
#include "base/trace_event/trace_event.h"
#include "build/build_config.h"
#include "media/base/bind_to_current_loop.h"
#include "media/base/format_utils.h"
#include "media/base/media_log.h"
#include "media/base/media_switches.h"
#include "media/base/media_util.h"
#include "media/base/unaligned_shared_memory.h"
#include "media/base/video_bitrate_allocation.h"
#include "media/gpu/chromeos/platform_video_frame_utils.h"
#include "media/gpu/gpu_video_encode_accelerator_helpers.h"
#include "media/gpu/h264_dpb.h"
#include "media/gpu/macros.h"
#include "media/gpu/vaapi/h264_vaapi_video_encoder_delegate.h"
#include "media/gpu/vaapi/va_surface.h"
#include "media/gpu/vaapi/vaapi_common.h"
#include "media/gpu/vaapi/vaapi_utils.h"
#include "media/gpu/vaapi/vaapi_wrapper.h"
#include "media/gpu/vaapi/vp8_vaapi_video_encoder_delegate.h"
#include "media/gpu/vaapi/vp9_vaapi_video_encoder_delegate.h"
#include "media/gpu/vp8_reference_frame_vector.h"
#include "media/gpu/vp9_reference_frame_vector.h"
#include "media/gpu/vp9_svc_layers.h"

#define NOTIFY_ERROR(error, msg)                        \
  do {                                                  \
    SetState(kError);                                   \
    VLOGF(1) << msg;                                    \
    VLOGF(1) << "Calling NotifyError(" << error << ")"; \
    NotifyError(error);                                 \
  } while (0)

namespace media {

namespace {
// Minimum number of frames in flight for pipeline depth, adjust to this number
// if encoder requests less.
constexpr size_t kMinNumFramesInFlight = 4;

// VASurfaceIDs internal format.
constexpr unsigned int kVaSurfaceFormat = VA_RT_FORMAT_YUV420;

void FillVAEncRateControlParams(
    uint32_t bps,
    uint32_t window_size,
    uint32_t initial_qp,
    uint32_t min_qp,
    uint32_t max_qp,
    uint32_t framerate,
    uint32_t buffer_size,
    VAEncMiscParameterRateControl& rate_control_param,
    VAEncMiscParameterFrameRate& framerate_param,
    VAEncMiscParameterHRD& hrd_param) {
  memset(&rate_control_param, 0, sizeof(rate_control_param));
  rate_control_param.bits_per_second = bps;
  rate_control_param.window_size = window_size;
  rate_control_param.initial_qp = initial_qp;
  rate_control_param.min_qp = min_qp;
  rate_control_param.max_qp = max_qp;
  rate_control_param.rc_flags.bits.disable_frame_skip = true;

  memset(&framerate_param, 0, sizeof(framerate_param));
  framerate_param.framerate = framerate;

  memset(&hrd_param, 0, sizeof(hrd_param));
  hrd_param.buffer_size = buffer_size;
  hrd_param.initial_buffer_fullness = buffer_size / 2;
}

// Creates one |encode_size| ScopedVASurface using |vaapi_wrapper|.
std::unique_ptr<ScopedVASurface> CreateScopedSurface(
    VaapiWrapper& vaapi_wrapper,
    const gfx::Size& encode_size,
    const std::vector<VaapiWrapper::SurfaceUsageHint>& surface_usage_hints) {
  // iHD driver doesn't align a resolution for encoding properly. Align it only
  // with encoder driver.
  // TODO(https://github.com/intel/media-driver/issues/1232): Remove this
  // workaround of aligning |encode_size|.
  gfx::Size surface_size = encode_size;
  if (!base::Contains(surface_usage_hints,
                      VaapiWrapper::SurfaceUsageHint::kVideoProcessWrite)) {
    surface_size = gfx::Size(base::bits::AlignUp(encode_size.width(), 16u),
                             base::bits::AlignUp(encode_size.height(), 16u));
  }

  auto surfaces = vaapi_wrapper.CreateScopedVASurfaces(
      kVaSurfaceFormat, surface_size, surface_usage_hints, 1u,
      /*visible_size=*/absl::nullopt,
      /*va_fourcc=*/absl::nullopt);
  return surfaces.empty() ? nullptr : std::move(surfaces.front());
}
}  // namespace

struct VaapiVideoEncodeAccelerator::InputFrameRef {
  InputFrameRef(scoped_refptr<VideoFrame> frame, bool force_keyframe)
      : frame(frame), force_keyframe(force_keyframe) {}
  const scoped_refptr<VideoFrame> frame;
  const bool force_keyframe;
};

struct VaapiVideoEncodeAccelerator::BitstreamBufferRef {
  BitstreamBufferRef(int32_t id, BitstreamBuffer buffer)
      : id(id),
        shm(std::make_unique<UnalignedSharedMemory>(buffer.TakeRegion(),
                                                    buffer.size(),
                                                    false)),
        offset(buffer.offset()) {}
  const int32_t id;
  const std::unique_ptr<UnalignedSharedMemory> shm;
  const off_t offset;
};

VideoEncodeAccelerator::SupportedProfiles
VaapiVideoEncodeAccelerator::GetSupportedProfiles() {
  if (IsConfiguredForTesting())
    return supported_profiles_for_testing_;
  return VaapiWrapper::GetSupportedEncodeProfiles();
}

VaapiVideoEncodeAccelerator::VaapiVideoEncodeAccelerator()
    : output_buffer_byte_size_(0),
      state_(kUninitialized),
      child_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      // TODO(akahuang): Change to use SequencedTaskRunner to see if the
      // performance is affected.
      encoder_task_runner_(base::ThreadPool::CreateSingleThreadTaskRunner(
          {base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN, base::MayBlock()},
          base::SingleThreadTaskRunnerThreadMode::DEDICATED)) {
  VLOGF(2);
  DCHECK_CALLED_ON_VALID_SEQUENCE(child_sequence_checker_);
  DETACH_FROM_SEQUENCE(encoder_sequence_checker_);

  child_weak_this_ = child_weak_this_factory_.GetWeakPtr();
  encoder_weak_this_ = encoder_weak_this_factory_.GetWeakPtr();

  // The default value of VideoEncoderInfo of VaapiVideoEncodeAccelerator.
  encoder_info_.implementation_name = "VaapiVideoEncodeAccelerator";
  encoder_info_.has_trusted_rate_controller = true;
  DCHECK(encoder_info_.is_hardware_accelerated);
  DCHECK(encoder_info_.supports_native_handle);
  DCHECK(!encoder_info_.supports_simulcast);
}

VaapiVideoEncodeAccelerator::~VaapiVideoEncodeAccelerator() {
  VLOGF(2);
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  base::trace_event::MemoryDumpManager::GetInstance()->UnregisterDumpProvider(
      this);
}

bool VaapiVideoEncodeAccelerator::Initialize(
    const Config& config,
    Client* client,

    std::unique_ptr<MediaLog> media_log) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(child_sequence_checker_);
  DCHECK_EQ(state_, kUninitialized);
  VLOGF(2) << "Initializing VAVEA, " << config.AsHumanReadableString();

  // NullMediaLog silently and safely does nothing.
  if (!media_log)
    media_log = std::make_unique<media::NullMediaLog>();

  if (AttemptedInitialization()) {
    MEDIA_LOG(ERROR, media_log.get())
        << "Initialize() cannot be called more than once.";
    return false;
  }

  client_ptr_factory_.reset(new base::WeakPtrFactory<Client>(client));
  client_ = client_ptr_factory_->GetWeakPtr();

  if (config.HasSpatialLayer()) {
#if BUILDFLAG(IS_CHROMEOS)
    if (!base::FeatureList::IsEnabled(kVaapiVp9kSVCHWEncoding) &&
        !IsConfiguredForTesting()) {
      MEDIA_LOG(ERROR, media_log.get())
          << "Spatial layer encoding is not yet enabled by default";
      return false;
    }
#endif  // BUILDFLAG(IS_CHROMEOS)

    if (config.inter_layer_pred != Config::InterLayerPredMode::kOnKeyPic) {
      MEDIA_LOG(ERROR, media_log.get()) << "Only K-SVC encoding is supported.";
      return false;
    }

    if (config.output_profile != VideoCodecProfile::VP9PROFILE_PROFILE0) {
      MEDIA_LOG(ERROR, media_log.get())
          << "Spatial layers are only supported for VP9 encoding";
      return false;
    }

    // TODO(crbug.com/1186051): Remove this restriction.
    for (size_t i = 0; i < config.spatial_layers.size(); ++i) {
      for (size_t j = i + 1; j < config.spatial_layers.size(); ++j) {
        if (config.spatial_layers[i].width == config.spatial_layers[j].width &&
            config.spatial_layers[i].height ==
                config.spatial_layers[j].height) {
          MEDIA_LOG(ERROR, media_log.get())
              << "Doesn't support k-SVC encoding where spatial layers "
                 "have the same resolution";
          return false;
        }
      }
    }

    if (!IsConfiguredForTesting()) {
      VAProfile va_profile = VAProfileVP9Profile0;
      if (VaapiWrapper::GetDefaultVaEntryPoint(
              VaapiWrapper::kEncodeConstantQuantizationParameter, va_profile) !=
          VAEntrypointEncSliceLP) {
        MEDIA_LOG(ERROR, media_log.get())
            << "Currently spatial layer encoding is only supported by "
               "VAEntrypointEncSliceLP";
        return false;
      }
    }
  }

  const VideoCodec codec = VideoCodecProfileToVideoCodec(config.output_profile);
  if (codec != VideoCodec::kH264 && codec != VideoCodec::kVP8 &&
      codec != VideoCodec::kVP9) {
    MEDIA_LOG(ERROR, media_log.get())
        << "Unsupported profile: " << GetProfileName(config.output_profile);
    return false;
  }

  switch (config.input_format) {
    case PIXEL_FORMAT_I420:
    case PIXEL_FORMAT_NV12:
      break;
    default:
      MEDIA_LOG(ERROR, media_log.get())
          << "Unsupported input format: " << config.input_format;
      return false;
  }

  if (config.storage_type.value_or(Config::StorageType::kShmem) ==
      Config::StorageType::kGpuMemoryBuffer) {
#if !defined(USE_OZONE)
    MEDIA_LOG(ERROR, media_log.get())
        << "Native mode is only available on OZONE platform.";
    return false;
#else
    if (config.input_format != PIXEL_FORMAT_NV12) {
      // TODO(crbug.com/894381): Support other formats.
      MEDIA_LOG(ERROR, media_log.get())
          << "Unsupported format for native input mode: "
          << VideoPixelFormatToString(config.input_format);
      return false;
    }
    native_input_mode_ = true;
#endif  // USE_OZONE
  }

  if (config.HasSpatialLayer() && !native_input_mode_) {
    MEDIA_LOG(ERROR, media_log.get())
        << "Spatial scalability is only supported for native input now";
    return false;
  }

  const SupportedProfiles& profiles = GetSupportedProfiles();
  const auto profile = find_if(profiles.begin(), profiles.end(),
                               [output_profile = config.output_profile](
                                   const SupportedProfile& profile) {
                                 return profile.profile == output_profile;
                               });
  if (profile == profiles.end()) {
    MEDIA_LOG(ERROR, media_log.get()) << "Unsupported output profile "
                                      << GetProfileName(config.output_profile);
    return false;
  }

  if (config.input_visible_size.width() > profile->max_resolution.width() ||
      config.input_visible_size.height() > profile->max_resolution.height()) {
    MEDIA_LOG(ERROR, media_log.get())
        << "Input size too big: " << config.input_visible_size.ToString()
        << ", max supported size: " << profile->max_resolution.ToString();
    return false;
  }

  // Finish remaining initialization on the encoder thread.
  encoder_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&VaapiVideoEncodeAccelerator::InitializeTask,
                                encoder_weak_this_, config));
  return true;
}

void VaapiVideoEncodeAccelerator::InitializeTask(const Config& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  DCHECK_EQ(state_, kUninitialized);
  VLOGF(2);

  output_codec_ = VideoCodecProfileToVideoCodec(config.output_profile);
  DCHECK_EQ(IsConfiguredForTesting(), !!vaapi_wrapper_);
  if (!IsConfiguredForTesting()) {
    const auto mode =
        (output_codec_ == VideoCodec::kVP9 || output_codec_ == VideoCodec::kVP8)
            ? VaapiWrapper::kEncodeConstantQuantizationParameter
            : VaapiWrapper::kEncodeConstantBitrate;
    vaapi_wrapper_ = VaapiWrapper::CreateForVideoCodec(
        mode, config.output_profile, EncryptionScheme::kUnencrypted,
        base::BindRepeating(&ReportVaapiErrorToUMA,
                            "Media.VaapiVideoEncodeAccelerator.VAAPIError"));

    if (!vaapi_wrapper_) {
      NOTIFY_ERROR(kPlatformFailureError,
                   "Failed initializing VAAPI for profile " +
                       GetProfileName(config.output_profile));
      return;
    }
  }

  DCHECK_EQ(IsConfiguredForTesting(), !!encoder_);
  // Base::Unretained(this) is safe because |error_cb| is called by
  // |encoder_| and |this| outlives |encoder_|.
  auto error_cb = base::BindRepeating(
      [](VaapiVideoEncodeAccelerator* const vea) {
        vea->SetState(kError);
        vea->NotifyError(kPlatformFailureError);
      },
      base::Unretained(this));

  VaapiVideoEncoderDelegate::Config ave_config{.native_input_mode =
                                                   native_input_mode_};
  switch (output_codec_) {
    case VideoCodec::kH264:
      if (!IsConfiguredForTesting()) {
        encoder_ = std::make_unique<H264VaapiVideoEncoderDelegate>(
            vaapi_wrapper_, error_cb);
      }

      DCHECK_EQ(ave_config.bitrate_control,
                VaapiVideoEncoderDelegate::BitrateControl::kConstantBitrate);
      break;
    case VideoCodec::kVP8:
      if (!IsConfiguredForTesting()) {
        encoder_ = std::make_unique<VP8VaapiVideoEncoderDelegate>(
            vaapi_wrapper_, error_cb);
      }

      ave_config.bitrate_control = VaapiVideoEncoderDelegate::BitrateControl::
          kConstantQuantizationParameter;
      break;
    case VideoCodec::kVP9:
      if (!IsConfiguredForTesting()) {
        encoder_ = std::make_unique<VP9VaapiVideoEncoderDelegate>(
            vaapi_wrapper_, error_cb);
      }

      ave_config.bitrate_control = VaapiVideoEncoderDelegate::BitrateControl::
          kConstantQuantizationParameter;
      break;
    default:
      NOTREACHED() << "Unsupported codec type " << GetCodecName(output_codec_);
      return;
  }

  if (!vaapi_wrapper_->GetVAEncMaxNumOfRefFrames(
          config.output_profile, &ave_config.max_num_ref_frames)) {
    NOTIFY_ERROR(kPlatformFailureError,
                 "Failed getting max number of reference frames"
                 "supported by the driver");
    return;
  }
  DCHECK_GT(ave_config.max_num_ref_frames, 0u);
  if (!encoder_->Initialize(config, ave_config)) {
    NOTIFY_ERROR(kInvalidArgumentError, "Failed initializing encoder");
    return;
  }

  output_buffer_byte_size_ = encoder_->GetBitstreamBufferSize();

  visible_rect_ = gfx::Rect(config.input_visible_size);
  expected_input_coded_size_ = VideoFrame::DetermineAlignedSize(
      config.input_format, config.input_visible_size);
  DCHECK(
      expected_input_coded_size_.width() <= encoder_->GetCodedSize().width() &&
      expected_input_coded_size_.height() <= encoder_->GetCodedSize().height());

  // The number of required buffers is the number of required reference frames
  // + 1 for the current frame to be encoded.
  const size_t max_ref_frames = encoder_->GetMaxNumOfRefFrames();
  num_frames_in_flight_ = std::max(kMinNumFramesInFlight, max_ref_frames);
  DVLOGF(1) << "Frames in flight: " << num_frames_in_flight_;

  if (!vaapi_wrapper_->CreateContext(encoder_->GetCodedSize())) {
    NOTIFY_ERROR(kPlatformFailureError, "Failed creating VAContext");
    return;
  }

  child_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&Client::RequireBitstreamBuffers, client_,
                     num_frames_in_flight_, expected_input_coded_size_,
                     output_buffer_byte_size_));

  if (config.HasSpatialLayer() || config.HasTemporalLayer()) {
    DCHECK(!config.spatial_layers.empty());
    for (size_t i = 0; i < config.spatial_layers.size(); ++i) {
      encoder_info_.fps_allocation[i] =
          GetFpsAllocation(config.spatial_layers[i].num_of_temporal_layers);
    }
  } else {
    constexpr uint8_t kFullFramerate = 255;
    encoder_info_.fps_allocation[0] = {kFullFramerate};
  }

  // Notify VideoEncoderInfo after initialization.
  child_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&Client::NotifyEncoderInfoChange, client_, encoder_info_));
  SetState(kEncoding);

  base::trace_event::MemoryDumpManager::GetInstance()->RegisterDumpProvider(
      this, "media::VaapiVideoEncodeAccelerator", encoder_task_runner_);
}

void VaapiVideoEncodeAccelerator::RecycleVASurface(
    std::vector<std::unique_ptr<ScopedVASurface>>* va_surfaces,
    std::unique_ptr<ScopedVASurface> va_surface,
    VASurfaceID va_surface_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  DCHECK(va_surface);
  DCHECK_EQ(va_surface_id, va_surface->id());
  DVLOGF(4) << "va_surface_id: " << va_surface_id;

  va_surfaces->push_back(std::move(va_surface));

  // At least one surface must available in |available_encode_surfaces_|
  // to succeed in EncodePendingInputs(). Checks here to avoid redundant
  // EncodePendingInputs() call.
  for (const auto& surfaces : available_encode_surfaces_) {
    if (surfaces.second.empty())
      return;
  }

  EncodePendingInputs();
}

void VaapiVideoEncodeAccelerator::TryToReturnBitstreamBuffers() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);

  if (state_ != kEncoding)
    return;

  TRACE_EVENT1("media,gpu", "VAVEA::TryToReturnBitstreamBuffers",
               "pending encode results", pending_encode_results_.size());
  while (!pending_encode_results_.empty()) {
    if (pending_encode_results_.front() == nullptr) {
      // A null job indicates a flush command.
      pending_encode_results_.pop();
      DVLOGF(2) << "FlushDone";
      DCHECK(flush_callback_);
      child_task_runner_->PostTask(
          FROM_HERE, base::BindOnce(std::move(flush_callback_), true));
      continue;
    }

    if (available_bitstream_buffers_.empty())
      return;

    auto buffer = std::move(available_bitstream_buffers_.front());
    available_bitstream_buffers_.pop();
    auto encode_result = std::move(pending_encode_results_.front());
    pending_encode_results_.pop();

    ReturnBitstreamBuffer(std::move(encode_result), std::move(buffer));
  }
}

void VaapiVideoEncodeAccelerator::ReturnBitstreamBuffer(
    std::unique_ptr<EncodeResult> encode_result,
    std::unique_ptr<BitstreamBufferRef> buffer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  uint8_t* target_data = static_cast<uint8_t*>(buffer->shm->memory());
  size_t data_size = 0;
  // vaSyncSurface() is not necessary because GetEncodedChunkSize() has been
  // called in VaapiVideoEncoderDelegate::Encode().
  if (!vaapi_wrapper_->DownloadFromVABuffer(
          encode_result->coded_buffer_id(), /*sync_surface_id=*/absl::nullopt,
          target_data, buffer->shm->size(), &data_size)) {
    NOTIFY_ERROR(kPlatformFailureError, "Failed downloading coded buffer");
    return;
  }

  auto metadata = encode_result->metadata();
  DCHECK_NE(metadata.payload_size_bytes, 0u);
  encode_result.reset();

  DVLOGF(4) << "Returning bitstream buffer "
            << (metadata.key_frame ? "(keyframe)" : "") << " id: " << buffer->id
            << " size: " << data_size;

  child_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&Client::BitstreamBufferReady, client_,
                                buffer->id, std::move(metadata)));
}

void VaapiVideoEncodeAccelerator::Encode(scoped_refptr<VideoFrame> frame,
                                         bool force_keyframe) {
  DVLOGF(4) << "Frame timestamp: " << frame->timestamp().InMilliseconds()
            << " force_keyframe: " << force_keyframe;
  DCHECK_CALLED_ON_VALID_SEQUENCE(child_sequence_checker_);

  encoder_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&VaapiVideoEncodeAccelerator::EncodeTask,
                     encoder_weak_this_, std::move(frame), force_keyframe));
}

void VaapiVideoEncodeAccelerator::EncodeTask(scoped_refptr<VideoFrame> frame,
                                             bool force_keyframe) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  DCHECK_NE(state_, kUninitialized);

  input_queue_.push(
      std::make_unique<InputFrameRef>(std::move(frame), force_keyframe));
  EncodePendingInputs();
}

bool VaapiVideoEncodeAccelerator::CreateSurfacesForGpuMemoryBufferEncoding(
    const VideoFrame& frame,
    const std::vector<gfx::Size>& spatial_layer_resolutions,
    std::vector<scoped_refptr<VASurface>>* input_surfaces,
    std::vector<scoped_refptr<VASurface>>* reconstructed_surfaces) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  DCHECK(native_input_mode_);
  TRACE_EVENT0("media,gpu", "VAVEA::CreateSurfacesForGpuMemoryBuffer");

  if (frame.storage_type() != VideoFrame::STORAGE_GPU_MEMORY_BUFFER) {
    NOTIFY_ERROR(kPlatformFailureError,
                 "Unexpected storage: "
                     << VideoFrame::StorageTypeToString(frame.storage_type()));
    return false;
  }
  if (frame.format() != PIXEL_FORMAT_NV12) {
    NOTIFY_ERROR(
        kPlatformFailureError,
        "Expected NV12, got: " << VideoPixelFormatToString(frame.format()));
    return false;
  }

  scoped_refptr<VASurface> source_surface;
  {
    TRACE_EVENT0("media,gpu", "VAVEA::ImportGpuMemoryBufferToVASurface");

    // Create VASurface from GpuMemory-based VideoFrame.
    scoped_refptr<gfx::NativePixmap> pixmap = CreateNativePixmapDmaBuf(&frame);
    if (!pixmap) {
      NOTIFY_ERROR(kPlatformFailureError,
                   "Failed to create NativePixmap from VideoFrame");
      return false;
    }

    source_surface =
        vaapi_wrapper_->CreateVASurfaceForPixmap(std::move(pixmap));
    if (!source_surface) {
      NOTIFY_ERROR(kPlatformFailureError, "Failed to create VASurface");
      return false;
    }
  }

  // Create input and reconstructed surfaces.
  TRACE_EVENT1("media,gpu", "VAVEA::ConstructSurfaces", "layers",
               spatial_layer_resolutions.size());
  input_surfaces->reserve(spatial_layer_resolutions.size());
  reconstructed_surfaces->reserve(spatial_layer_resolutions.size());
  for (const gfx::Size& encode_size : spatial_layer_resolutions) {
    const bool engage_vpp = frame.visible_rect() != gfx::Rect(encode_size);
    // Crop and Scale input surface to a surface whose size is |encode_size|.
    // The size of a reconstructed surface is also |encode_size|.
    if (engage_vpp) {
      auto blit_surface = ExecuteBlitSurface(*source_surface,
                                             frame.visible_rect(), encode_size);
      if (!blit_surface)
        return false;

      input_surfaces->push_back(std::move(blit_surface));
    } else {
      input_surfaces->emplace_back(source_surface);
    }

    reconstructed_surfaces->emplace_back(CreateEncodeSurface(encode_size));
    if (!reconstructed_surfaces->back())
      return false;
  }

  DCHECK(!base::Contains(*input_surfaces, nullptr));
  DCHECK(!base::Contains(*reconstructed_surfaces, nullptr));
  return true;
}

bool VaapiVideoEncodeAccelerator::CreateSurfacesForShmemEncoding(
    const VideoFrame& frame,
    scoped_refptr<VASurface>* input_surface,
    scoped_refptr<VASurface>* reconstructed_surface) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  DCHECK(!native_input_mode_);
  DCHECK(frame.IsMappable());
  TRACE_EVENT0("media,gpu", "VAVEA::CreateSurfacesForShmem");

  if (expected_input_coded_size_ != frame.coded_size()) {
    // In non-zero copy mode, the coded size of the incoming frame should be
    // the same as the one we requested through
    // Client::RequireBitstreamBuffers().
    NOTIFY_ERROR(kPlatformFailureError,
                 "Expected frame coded size: "
                     << expected_input_coded_size_.ToString()
                     << ", but got: " << frame.coded_size().ToString());
    return false;
  }

  DCHECK(visible_rect_.origin().IsOrigin());
  if (visible_rect_ != frame.visible_rect()) {
    // In non-zero copy mode, the client is responsible for scaling and
    // cropping.
    NOTIFY_ERROR(kPlatformFailureError,
                 "Expected frame visible rectangle: "
                     << visible_rect_.ToString()
                     << ", but got: " << frame.visible_rect().ToString());
    return false;
  }

  const gfx::Size& encode_size = encoder_->GetCodedSize();
  *input_surface =
      CreateInputSurface(*vaapi_wrapper_, encode_size,
                         {VaapiWrapper::SurfaceUsageHint::kVideoEncoder});
  if (!*input_surface) {
    NOTIFY_ERROR(kPlatformFailureError, "Failed to create input surface");
    return false;
  }

  *reconstructed_surface = CreateEncodeSurface(encode_size);
  return !!*reconstructed_surface;
}

scoped_refptr<VASurface> VaapiVideoEncodeAccelerator::CreateInputSurface(
    VaapiWrapper& vaapi_wrapper,
    const gfx::Size& encode_size,
    const std::vector<VaapiWrapper::SurfaceUsageHint>& surface_usage_hints) {
  if (!base::Contains(input_surfaces_, encode_size)) {
    auto surface =
        CreateScopedSurface(vaapi_wrapper, encode_size, surface_usage_hints);
    if (!surface) {
      NOTIFY_ERROR(kPlatformFailureError, "Failed to create surface");
      return nullptr;
    }

    input_surfaces_[encode_size] = std::move(surface);
  }

  const ScopedVASurface& surface = *input_surfaces_[encode_size];
  return base::MakeRefCounted<VASurface>(surface.id(), surface.size(),
                                         surface.format(), base::DoNothing());
}

scoped_refptr<VASurface> VaapiVideoEncodeAccelerator::CreateEncodeSurface(
    const gfx::Size& encode_size) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  const size_t max_allocated_surfaces = num_frames_in_flight_ + 1;
  const bool no_surfaces_available =
      !base::Contains(available_encode_surfaces_, encode_size) ||
      available_encode_surfaces_[encode_size].empty();
  if (no_surfaces_available &&
      encode_surfaces_count_[encode_size] >= max_allocated_surfaces) {
    DVLOGF(4) << "Not enough surfaces available";
    return nullptr;
  }

  if (no_surfaces_available) {
    auto surface =
        CreateScopedSurface(*vaapi_wrapper_, encode_size,
                            {VaapiWrapper::SurfaceUsageHint::kVideoEncoder});
    if (!surface) {
      NOTIFY_ERROR(kPlatformFailureError, "Failed creating surfaces");
      return nullptr;
    }

    available_encode_surfaces_[encode_size].push_back(std::move(surface));
    encode_surfaces_count_[encode_size] += 1;
  }

  auto& surfaces = available_encode_surfaces_[encode_size];
  auto scoped_va_surface = std::move(surfaces.back());
  surfaces.pop_back();

  const VASurfaceID id = scoped_va_surface->id();
  const gfx::Size& size = scoped_va_surface->size();
  const unsigned int format = scoped_va_surface->format();
  VASurface::ReleaseCB release_cb = BindToCurrentLoop(base::BindOnce(
      &VaapiVideoEncodeAccelerator::RecycleVASurface, encoder_weak_this_,
      &surfaces, std::move(scoped_va_surface)));

  return base::MakeRefCounted<VASurface>(id, size, format,
                                         std::move(release_cb));
}

scoped_refptr<VaapiWrapper>
VaapiVideoEncodeAccelerator::CreateVppVaapiWrapper() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  DCHECK(!vpp_vaapi_wrapper_);
  auto vpp_vaapi_wrapper = VaapiWrapper::Create(
      VaapiWrapper::kVideoProcess, VAProfileNone,
      EncryptionScheme::kUnencrypted,
      base::BindRepeating(&ReportVaapiErrorToUMA,
                          "Media.VaapiVideoEncodeAccelerator.Vpp.VAAPIError"));
  if (!vpp_vaapi_wrapper) {
    NOTIFY_ERROR(kPlatformFailureError, "Failed to initialize VppVaapiWrapper");
    return nullptr;
  }
  // VA context for VPP is not associated with a specific resolution.
  if (!vpp_vaapi_wrapper->CreateContext(gfx::Size())) {
    NOTIFY_ERROR(kPlatformFailureError, "Failed creating Context for VPP");
    return nullptr;
  }

  return vpp_vaapi_wrapper;
}

scoped_refptr<VASurface> VaapiVideoEncodeAccelerator::ExecuteBlitSurface(
    const VASurface& source_surface,
    const gfx::Rect source_visible_rect,
    const gfx::Size& encode_size) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  if (!vpp_vaapi_wrapper_) {
    vpp_vaapi_wrapper_ = CreateVppVaapiWrapper();
    if (!vpp_vaapi_wrapper_) {
      NOTIFY_ERROR(kPlatformFailureError, "Failed to create Vpp");
      return nullptr;
    }
  }

  auto blit_surface =
      CreateInputSurface(*vpp_vaapi_wrapper_, encode_size,
                         {VaapiWrapper::SurfaceUsageHint::kVideoProcessWrite,
                          VaapiWrapper::SurfaceUsageHint::kVideoEncoder});
  if (!blit_surface)
    return nullptr;

  DCHECK(vpp_vaapi_wrapper_);
  if (!vpp_vaapi_wrapper_->BlitSurface(source_surface, *blit_surface,
                                       source_visible_rect,
                                       gfx::Rect(encode_size))) {
    NOTIFY_ERROR(kPlatformFailureError,
                 "Failed BlitSurface on frame size: "
                     << source_surface.size().ToString()
                     << " (visible rect: " << source_visible_rect.ToString()
                     << ") -> encode size: " << encode_size.ToString());
    return nullptr;
  }

  return blit_surface;
}

std::unique_ptr<VaapiVideoEncoderDelegate::EncodeJob>
VaapiVideoEncodeAccelerator::CreateEncodeJob(
    scoped_refptr<VideoFrame> frame,
    bool force_keyframe,
    const VASurface& input_surface,
    scoped_refptr<VASurface> reconstructed_surface) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  DCHECK(frame);
  DCHECK_NE(input_surface.id(), VA_INVALID_ID);
  DCHECK(!input_surface.size().IsEmpty());
  DCHECK(reconstructed_surface);

  std::unique_ptr<ScopedVABuffer> coded_buffer;
  {
    TRACE_EVENT1("media,gpu", "VAVEA::CreateVABuffer", "buffer size",
                 output_buffer_byte_size_);
    coded_buffer = vaapi_wrapper_->CreateVABuffer(VAEncCodedBufferType,
                                                  output_buffer_byte_size_);
    if (!coded_buffer) {
      NOTIFY_ERROR(kPlatformFailureError, "Failed creating coded buffer");
      return nullptr;
    }
  }

  scoped_refptr<CodecPicture> picture;
  switch (output_codec_) {
    case VideoCodec::kH264:
      picture = new VaapiH264Picture(std::move(reconstructed_surface));
      break;
    case VideoCodec::kVP8:
      picture = new VaapiVP8Picture(std::move(reconstructed_surface));
      break;
    case VideoCodec::kVP9:
      picture = new VaapiVP9Picture(std::move(reconstructed_surface));
      break;
    default:
      return nullptr;
  }

  return std::make_unique<EncodeJob>(frame, force_keyframe, input_surface.id(),
                                     input_surface.size(), std::move(picture),
                                     std::move(coded_buffer));
}

void VaapiVideoEncodeAccelerator::EncodePendingInputs() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  DVLOGF(4);

  std::vector<gfx::Size> spatial_layer_resolutions =
      encoder_->GetSVCLayerResolutions();
  if (spatial_layer_resolutions.empty()) {
    VLOGF(1) << " Failed to get SVC layer resolutions";
    return;
  }

  while (state_ == kEncoding && !input_queue_.empty()) {
    const std::unique_ptr<InputFrameRef>& input_frame = input_queue_.front();
    if (!input_frame) {
      // If this is a flush (null) frame, don't create/submit a new encode
      // result for it, but forward a null result to the
      // |pending_encode_results_| queue.
      pending_encode_results_.push(nullptr);
      input_queue_.pop();
      TryToReturnBitstreamBuffers();
      continue;
    }

    TRACE_EVENT0("media,gpu",
                 "VAVEA::EncodeOneInputFrameAndReturnEncodedChunks");
    const size_t num_spatial_layers = spatial_layer_resolutions.size();
    std::vector<scoped_refptr<VASurface>> input_surfaces;
    std::vector<scoped_refptr<VASurface>> reconstructed_surfaces;
    if (native_input_mode_) {
      if (!CreateSurfacesForGpuMemoryBufferEncoding(
              *input_frame->frame, spatial_layer_resolutions, &input_surfaces,
              &reconstructed_surfaces)) {
        return;
      }
    } else {
      DCHECK_EQ(num_spatial_layers, 1u);
      input_surfaces.resize(1u);
      reconstructed_surfaces.resize(1u);
      if (!CreateSurfacesForShmemEncoding(*input_frame->frame,
                                          &input_surfaces[0],
                                          &reconstructed_surfaces[0])) {
        return;
      }
    }

    // Encoding different spatial layers for |input_frame|.
    std::vector<std::unique_ptr<EncodeJob>> jobs;
    for (size_t spatial_idx = 0; spatial_idx < num_spatial_layers;
         ++spatial_idx) {
      std::unique_ptr<EncodeJob> job;
      TRACE_EVENT0("media,gpu", "VAVEA::FromCreateEncodeJobToReturn");
      const bool force_key =
          (spatial_idx == 0 ? input_frame->force_keyframe : false);
      job = CreateEncodeJob(input_frame->frame, force_key,
                            *input_surfaces[spatial_idx],
                            std::move(reconstructed_surfaces[spatial_idx]));
      if (!job)
        return;

      jobs.emplace_back(std::move(job));
    }

    for (auto& job : jobs) {
      TRACE_EVENT0("media,gpu", "VAVEA::Encode");
      if (!encoder_->Encode(*job)) {
        NOTIFY_ERROR(kPlatformFailureError, "Failed encoding job");
        return;
      }
    }

    for (auto&& job : jobs) {
      TRACE_EVENT0("media,gpu", "VAVEA::GetEncodeResult");
      std::unique_ptr<EncodeResult> result =
          encoder_->GetEncodeResult(std::move(job));
      if (!result) {
        NOTIFY_ERROR(kPlatformFailureError, "Failed getting encode result");
        return;
      }

      pending_encode_results_.push(std::move(result));
    }

    TryToReturnBitstreamBuffers();
    input_queue_.pop();
  }
}

void VaapiVideoEncodeAccelerator::UseOutputBitstreamBuffer(
    BitstreamBuffer buffer) {
  DVLOGF(4) << "id: " << buffer.id();
  DCHECK_CALLED_ON_VALID_SEQUENCE(child_sequence_checker_);

  if (buffer.size() < output_buffer_byte_size_) {
    NOTIFY_ERROR(kInvalidArgumentError, "Provided bitstream buffer too small");
    return;
  }

  auto buffer_ref =
      std::make_unique<BitstreamBufferRef>(buffer.id(), std::move(buffer));

  encoder_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&VaapiVideoEncodeAccelerator::UseOutputBitstreamBufferTask,
                     encoder_weak_this_, std::move(buffer_ref)));
}

void VaapiVideoEncodeAccelerator::UseOutputBitstreamBufferTask(
    std::unique_ptr<BitstreamBufferRef> buffer_ref) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  DCHECK_NE(state_, kUninitialized);

  if (!buffer_ref->shm->MapAt(buffer_ref->offset, buffer_ref->shm->size())) {
    NOTIFY_ERROR(kPlatformFailureError, "Failed mapping shared memory.");
    return;
  }

  available_bitstream_buffers_.push(std::move(buffer_ref));
  TryToReturnBitstreamBuffers();
}

void VaapiVideoEncodeAccelerator::RequestEncodingParametersChange(
    const Bitrate& bitrate,
    uint32_t framerate) {
  // If this is changed to use variable bitrate encoding, change the mode check
  // to check that the mode matches the current mode.
  if (bitrate.mode() != Bitrate::Mode::kConstant) {
    VLOGF(1) << "Failed to update rates due to invalid bitrate mode.";
    return;
  }

  DCHECK_CALLED_ON_VALID_SEQUENCE(child_sequence_checker_);

  VideoBitrateAllocation allocation;
  allocation.SetBitrate(0, 0, bitrate.target_bps());
  encoder_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &VaapiVideoEncodeAccelerator::RequestEncodingParametersChangeTask,
          encoder_weak_this_, allocation, framerate));
}

void VaapiVideoEncodeAccelerator::RequestEncodingParametersChange(
    const VideoBitrateAllocation& bitrate_allocation,
    uint32_t framerate) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(child_sequence_checker_);

  encoder_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &VaapiVideoEncodeAccelerator::RequestEncodingParametersChangeTask,
          encoder_weak_this_, bitrate_allocation, framerate));
}

void VaapiVideoEncodeAccelerator::RequestEncodingParametersChangeTask(
    VideoBitrateAllocation bitrate_allocation,
    uint32_t framerate) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  DCHECK_NE(state_, kUninitialized);

  if (!encoder_->UpdateRates(bitrate_allocation, framerate)) {
    VLOGF(1) << "Failed to update rates to " << bitrate_allocation.GetSumBps()
             << " " << framerate;
  }
}

void VaapiVideoEncodeAccelerator::Flush(FlushCallback flush_callback) {
  DVLOGF(2);
  DCHECK_CALLED_ON_VALID_SEQUENCE(child_sequence_checker_);

  encoder_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&VaapiVideoEncodeAccelerator::FlushTask,
                                encoder_weak_this_, std::move(flush_callback)));
}

void VaapiVideoEncodeAccelerator::FlushTask(FlushCallback flush_callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);

  if (flush_callback_) {
    NOTIFY_ERROR(kIllegalStateError, "There is a pending flush");
    child_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(std::move(flush_callback), false));
    return;
  }
  flush_callback_ = std::move(flush_callback);

  // Insert an null job to indicate a flush command.
  input_queue_.push(std::unique_ptr<InputFrameRef>(nullptr));
  EncodePendingInputs();
}

bool VaapiVideoEncodeAccelerator::IsFlushSupported() {
  return true;
}

void VaapiVideoEncodeAccelerator::Destroy() {
  DVLOGF(2);
  DCHECK_CALLED_ON_VALID_SEQUENCE(child_sequence_checker_);

  child_weak_this_factory_.InvalidateWeakPtrs();

  // We're destroying; cancel all callbacks.
  if (client_ptr_factory_)
    client_ptr_factory_->InvalidateWeakPtrs();

  encoder_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&VaapiVideoEncodeAccelerator::DestroyTask,
                                encoder_weak_this_));
}

void VaapiVideoEncodeAccelerator::DestroyTask() {
  VLOGF(2);
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);

  encoder_weak_this_factory_.InvalidateWeakPtrs();

  if (flush_callback_) {
    child_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(std::move(flush_callback_), false));
  }

  // Clean up members that are to be accessed on the encoder thread only.
  // Call DestroyContext() explicitly to make sure it's destroyed before
  // VA surfaces.
  if (vaapi_wrapper_)
    vaapi_wrapper_->DestroyContext();

  available_encode_surfaces_.clear();
  available_va_buffer_ids_.clear();

  if (vpp_vaapi_wrapper_)
    vpp_vaapi_wrapper_->DestroyContext();

  input_surfaces_.clear();

  while (!available_bitstream_buffers_.empty())
    available_bitstream_buffers_.pop();

  while (!input_queue_.empty())
    input_queue_.pop();

  // Note ScopedVABuffer owned by EncodeResults must be destroyed before
  // |vaapi_wrapper_| is destroyed to ensure VADisplay is valid on the
  // ScopedVABuffer's destruction.
  DCHECK(vaapi_wrapper_ || pending_encode_results_.empty());
  while (!pending_encode_results_.empty())
    pending_encode_results_.pop();

  encoder_ = nullptr;

  delete this;
}

void VaapiVideoEncodeAccelerator::SetState(State state) {
  // Only touch state on encoder thread, unless it's not running.
  if (!encoder_task_runner_->BelongsToCurrentThread()) {
    encoder_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&VaapiVideoEncodeAccelerator::SetState,
                                  encoder_weak_this_, state));
    return;
  }

  VLOGF(2) << "setting state to: " << state;
  state_ = state;
}

void VaapiVideoEncodeAccelerator::NotifyError(Error error) {
  if (!child_task_runner_->BelongsToCurrentThread()) {
    child_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&VaapiVideoEncodeAccelerator::NotifyError,
                                  child_weak_this_, error));
    return;
  }

  if (client_) {
    client_->NotifyError(error);
    client_ptr_factory_->InvalidateWeakPtrs();
  }
}

bool VaapiVideoEncodeAccelerator::OnMemoryDump(
    const base::trace_event::MemoryDumpArgs& args,
    base::trace_event::ProcessMemoryDump* pmd) {
  using base::trace_event::MemoryAllocatorDump;
  DCHECK_CALLED_ON_VALID_SEQUENCE(encoder_sequence_checker_);
  auto dump_name = base::StringPrintf("gpu/vaapi/encoder/0x%" PRIxPTR,
                                      reinterpret_cast<uintptr_t>(this));

  MemoryAllocatorDump* dump = pmd->CreateAllocatorDump(dump_name);
  dump->AddString("encoder native input mode", "",
                  native_input_mode_ ? "true" : "false");

  constexpr double kNumBytesPerPixelYUV420 = 12.0 / 8;

  for (const auto& surface : encode_surfaces_count_) {
    const gfx::Size& resolution = surface.first;
    const size_t count = surface.second;
    MemoryAllocatorDump* sub_dump = pmd->CreateAllocatorDump(
        dump_name + "/encode surface/" + resolution.ToString());
    sub_dump->AddScalar(MemoryAllocatorDump::kNameObjectCount,
                        MemoryAllocatorDump::kUnitsObjects,
                        static_cast<uint64_t>(count));

    const uint64_t surfaces_packed_size = static_cast<uint64_t>(
        resolution.GetArea() * kNumBytesPerPixelYUV420 * count);
    sub_dump->AddScalar(MemoryAllocatorDump::kNameSize,
                        MemoryAllocatorDump::kUnitsBytes, surfaces_packed_size);
  }

  for (const auto& surface : input_surfaces_) {
    const gfx::Size& resolution = surface.first;
    MemoryAllocatorDump* sub_dump = pmd->CreateAllocatorDump(
        dump_name + "/input surface/" + resolution.ToString());

    const uint64_t surfaces_packed_size =
        static_cast<uint64_t>(resolution.GetArea() * kNumBytesPerPixelYUV420);
    sub_dump->AddScalar(MemoryAllocatorDump::kNameSize,
                        MemoryAllocatorDump::kUnitsBytes, surfaces_packed_size);
  }

  return true;
}
}  // namespace media
