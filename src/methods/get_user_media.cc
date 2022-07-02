/* Copyright (c) 2019 The node-webrtc project authors. All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be found
 * in the LICENSE.md file in the root of the source tree. All contributing
 * project authors may be found in the AUTHORS file in the root of the source
 * tree.
 */
#include "src/methods/get_user_media.h"

#include <webrtc/api/audio_options.h>
#include <webrtc/api/peer_connection_interface.h>
#include <webrtc/pc/video_track_source.h>
#include <webrtc/api/video/video_frame.h>
#include <webrtc/api/video/video_source_interface.h>
#include <webrtc/media/base/video_adapter.h>
#include <webrtc/media/base/video_broadcaster.h>
#include <webrtc/rtc_base/synchronization/mutex.h>
#include <webrtc/api/scoped_refptr.h>
#include <webrtc/api/video/i420_buffer.h>
#include <webrtc/api/video/video_frame_buffer.h>
#include <webrtc/api/video/video_rotation.h>
#include <webrtc/modules/video_capture/video_capture.h>
#include <webrtc/modules/video_capture/video_capture_factory.h>

#include "src/converters.h"
#include "src/converters/arguments.h"
#include "src/converters/interfaces.h"
#include "src/converters/napi.h"
#include "src/converters/object.h"
#include "src/functional/curry.h"
#include "src/functional/either.h"
#include "src/functional/maybe.h"
#include "src/functional/operators.h"
#include "src/functional/validation.h"
#include "src/interfaces/media_stream.h"
#include "src/interfaces/rtc_peer_connection/peer_connection_factory.h"
#include "src/interfaces/rtc_video_source.h"
#include "src/node/events.h"
#include "src/node/utility.h"


class TestVideoCapturer : public rtc::VideoSourceInterface<webrtc::VideoFrame> {
 public:
  class FramePreprocessor {
   public:
    virtual ~FramePreprocessor() = default;

    virtual webrtc::VideoFrame Preprocess(const webrtc::VideoFrame& frame) = 0;
  };

  ~TestVideoCapturer() override;

  void AddOrUpdateSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                       const rtc::VideoSinkWants& wants) override;
  void RemoveSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override;
  void SetFramePreprocessor(std::unique_ptr<FramePreprocessor> preprocessor) {
    webrtc::MutexLock lock(&lock_);
    preprocessor_ = std::move(preprocessor);
  }

 protected:
  void OnFrame(const webrtc::VideoFrame& frame);
  rtc::VideoSinkWants GetSinkWants();

 private:
  void UpdateVideoAdapter();
  webrtc::VideoFrame MaybePreprocess(const webrtc::VideoFrame& frame);

  webrtc::Mutex lock_;
  std::unique_ptr<FramePreprocessor> preprocessor_ RTC_GUARDED_BY(lock_);
  rtc::VideoBroadcaster broadcaster_;
  cricket::VideoAdapter video_adapter_;
};


TestVideoCapturer::~TestVideoCapturer() = default;

void TestVideoCapturer::OnFrame(const webrtc::VideoFrame& original_frame) {
  int cropped_width = 0;
  int cropped_height = 0;
  int out_width = 0;
  int out_height = 0;

  webrtc::VideoFrame frame = MaybePreprocess(original_frame);

  if (!video_adapter_.AdaptFrameResolution(
          frame.width(), frame.height(), frame.timestamp_us() * 1000,
          &cropped_width, &cropped_height, &out_width, &out_height)) {
    // Drop frame in order to respect frame rate constraint.
    return;
  }

  if (out_height != frame.height() || out_width != frame.width()) {
    // Video adapter has requested a down-scale. Allocate a new buffer and
    // return scaled version.
    // For simplicity, only scale here without cropping.
    rtc::scoped_refptr<webrtc::I420Buffer> scaled_buffer =
        webrtc::I420Buffer::Create(out_width, out_height);
    scaled_buffer->ScaleFrom(*frame.video_frame_buffer()->ToI420());
    webrtc::VideoFrame::Builder new_frame_builder =
        webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(scaled_buffer)
            .set_rotation(webrtc::kVideoRotation_0)
            .set_timestamp_us(frame.timestamp_us())
            .set_id(frame.id());
    if (frame.has_update_rect()) {
      webrtc::VideoFrame::UpdateRect new_rect = frame.update_rect().ScaleWithFrame(
          frame.width(), frame.height(), 0, 0, frame.width(), frame.height(),
          out_width, out_height);
      new_frame_builder.set_update_rect(new_rect);
    }
    broadcaster_.OnFrame(new_frame_builder.build());

  } else {
    // No adaptations needed, just return the frame as is.
    broadcaster_.OnFrame(frame);
  }
}

rtc::VideoSinkWants TestVideoCapturer::GetSinkWants() {
  return broadcaster_.wants();
}

void TestVideoCapturer::AddOrUpdateSink(
    rtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
    const rtc::VideoSinkWants& wants) {
  broadcaster_.AddOrUpdateSink(sink, wants);
  UpdateVideoAdapter();
}

void TestVideoCapturer::RemoveSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) {
  broadcaster_.RemoveSink(sink);
  UpdateVideoAdapter();
}

void TestVideoCapturer::UpdateVideoAdapter() {
  video_adapter_.OnSinkWants(broadcaster_.wants());
}

webrtc::VideoFrame TestVideoCapturer::MaybePreprocess(const webrtc::VideoFrame& frame) {
  webrtc::MutexLock lock(&lock_);
  if (preprocessor_ != nullptr) {
    return preprocessor_->Preprocess(frame);
  } else {
    return frame;
  }
}

class VcmCapturer : public TestVideoCapturer,
                    public rtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  static VcmCapturer* Create(size_t width,
                             size_t height,
                             size_t target_fps,
                             size_t capture_device_index);
  virtual ~VcmCapturer();

  void OnFrame(const webrtc::VideoFrame& frame) override;

 private:
  VcmCapturer();
  bool Init(size_t width,
            size_t height,
            size_t target_fps,
            size_t capture_device_index);
  void Destroy();

  rtc::scoped_refptr<webrtc::VideoCaptureModule> vcm_;
  webrtc::VideoCaptureCapability capability_;
};

VcmCapturer::VcmCapturer() : vcm_(nullptr) {}

bool VcmCapturer::Init(size_t width,
                       size_t height,
                       size_t target_fps,
                       size_t capture_device_index) {
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> device_info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());

  char device_name[256];
  char unique_name[256];
  if (device_info->GetDeviceName(static_cast<uint32_t>(capture_device_index),
                                 device_name, sizeof(device_name), unique_name,
                                 sizeof(unique_name)) != 0) {
    Destroy();
    return false;
  }

  vcm_ = webrtc::VideoCaptureFactory::Create(unique_name);
  if (!vcm_) {
    return false;
  }
  vcm_->RegisterCaptureDataCallback(this);

  device_info->GetCapability(vcm_->CurrentDeviceName(), 0, capability_);

  capability_.width = static_cast<int32_t>(width);
  capability_.height = static_cast<int32_t>(height);
  capability_.maxFPS = static_cast<int32_t>(target_fps);
  capability_.videoType = webrtc::VideoType::kI420;

  if (vcm_->StartCapture(capability_) != 0) {
    Destroy();
    return false;
  }

  RTC_CHECK(vcm_->CaptureStarted());

  return true;
}

VcmCapturer* VcmCapturer::Create(size_t width,
                                 size_t height,
                                 size_t target_fps,
                                 size_t capture_device_index) {
  std::unique_ptr<VcmCapturer> vcm_capturer(new VcmCapturer());
  if (!vcm_capturer->Init(width, height, target_fps, capture_device_index)) {
    RTC_LOG(LS_WARNING) << "Failed to create VcmCapturer(w = " << width
                        << ", h = " << height << ", fps = " << target_fps
                        << ")";
    return nullptr;
  }
  return vcm_capturer.release();
}

void VcmCapturer::Destroy() {
  if (!vcm_)
    return;

  vcm_->StopCapture();
  vcm_->DeRegisterCaptureDataCallback();
  // Release reference to VCM.
  vcm_ = nullptr;
}

VcmCapturer::~VcmCapturer() {
  Destroy();
}

void VcmCapturer::OnFrame(const webrtc::VideoFrame& frame) {
  TestVideoCapturer::OnFrame(frame);
}

class CapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static rtc::scoped_refptr<CapturerTrackSource> Create() {
    const size_t kWidth = 640;
    const size_t kHeight = 480;
    const size_t kFps = 30;
    const size_t kDeviceIndex = 0;
    std::unique_ptr<VcmCapturer> capturer = absl::WrapUnique(
        VcmCapturer::Create(kWidth, kHeight, kFps, kDeviceIndex));
    if (!capturer) {
      return nullptr;
    }
    return new rtc::RefCountedObject<CapturerTrackSource>(std::move(capturer));
  }

 protected:
  explicit CapturerTrackSource(
      std::unique_ptr<VcmCapturer> capturer)
      : VideoTrackSource(/*remote=*/false), capturer_(std::move(capturer)) {}

 private:
  rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return capturer_.get();
  }
  std::unique_ptr<VcmCapturer> capturer_;
};

// TODO(mroberts): Expand support for other members.
struct MediaTrackConstraintSet {
  node_webrtc::Maybe<uint16_t> width;
  node_webrtc::Maybe<uint16_t> height;

  static MediaTrackConstraintSet Create(
      const node_webrtc::Maybe<uint16_t> width,
      const node_webrtc::Maybe<uint16_t> height
  ) {
    return {width, height};
  }
};

template <>
struct node_webrtc::Converter<Napi::Value, MediaTrackConstraintSet> {
  static node_webrtc::Validation<MediaTrackConstraintSet> Convert(const Napi::Value value) {
    return node_webrtc::From<Napi::Object>(value).FlatMap<MediaTrackConstraintSet>([](auto object) {
      return curry(MediaTrackConstraintSet::Create)
          % node_webrtc::GetOptional<uint16_t>(object, "width")
          * node_webrtc::GetOptional<uint16_t>(object, "height");
    });
  }
};

struct MediaTrackConstraints: public MediaTrackConstraintSet {
  std::vector<MediaTrackConstraintSet> advanced;

  static MediaTrackConstraints Create(
      const MediaTrackConstraintSet set,
      const std::vector<MediaTrackConstraintSet>& advanced
  ) {
    MediaTrackConstraints constraints;
    constraints.width = set.width;
    constraints.height = set.height;
    constraints.advanced = advanced;
    return constraints;
  }
};

template <>
struct node_webrtc::Converter<Napi::Value, MediaTrackConstraints> {
  static node_webrtc::Validation<MediaTrackConstraints> Convert(const Napi::Value value) {
    return node_webrtc::From<Napi::Object>(value).FlatMap<MediaTrackConstraints>([&value](auto object) {
      return curry(MediaTrackConstraints::Create)
          % node_webrtc::From<MediaTrackConstraintSet>(value)
          * node_webrtc::GetOptional<std::vector<MediaTrackConstraintSet>>(object, "advanced", std::vector<MediaTrackConstraintSet>());
    });
  }
};


struct MediaStreamConstraints {
  node_webrtc::Maybe<node_webrtc::Either<bool, MediaTrackConstraints>> audio;
  node_webrtc::Maybe<node_webrtc::Either<bool, MediaTrackConstraints>> video;

  static node_webrtc::Validation<MediaStreamConstraints> Create(
      const node_webrtc::Maybe<node_webrtc::Either<bool, MediaTrackConstraints>>& audio,
      const node_webrtc::Maybe<node_webrtc::Either<bool, MediaTrackConstraints>>& video
  ) {
    return audio.IsNothing() && video.IsNothing()
        ? node_webrtc::Validation<MediaStreamConstraints>::Invalid(R"(Must specify at least "audio" or "video")")
        : node_webrtc::Validation<MediaStreamConstraints>::Valid({audio, video});
  }
};

template <>
struct node_webrtc::Converter<Napi::Value, MediaStreamConstraints> {
  static node_webrtc::Validation<MediaStreamConstraints> Convert(const Napi::Value value) {
    return node_webrtc::From<Napi::Object>(value).FlatMap<MediaStreamConstraints>([](auto object) {
      return node_webrtc::Validation<MediaStreamConstraints>::Join(curry(MediaStreamConstraints::Create)
              % node_webrtc::GetOptional<node_webrtc::Either<bool, MediaTrackConstraints>>(object, "audio")
              * node_webrtc::GetOptional<node_webrtc::Either<bool, MediaTrackConstraints>>(object, "video"));
    });
  }
};

Napi::Value node_webrtc::GetUserMedia::GetUserMediaImpl(const Napi::CallbackInfo& info) {
  CREATE_DEFERRED(info.Env(), deferred)

  CONVERT_ARGS_OR_REJECT_AND_RETURN_NAPI(deferred, info, constraints, MediaStreamConstraints)

  auto factory = node_webrtc::PeerConnectionFactory::GetOrCreateDefault();
  auto stream = factory->factory()->CreateLocalMediaStream(rtc::CreateRandomUuid());

  auto audio = constraints.audio.Map([](auto constraint) {
    return constraint.FromLeft(true);
  }).FromMaybe(false);

  auto video = constraints.video.Map([](auto constraint) {
    return constraint.FromLeft(true);
  }).FromMaybe(false);

  if (audio) {
    cricket::AudioOptions options;
    auto source = factory->factory()->CreateAudioSource(options);
    auto track = factory->factory()->CreateAudioTrack(rtc::CreateRandomUuid(), source);
    std::string id = track->id();
    stream->AddTrack(track);
  }

  if (video) {
    auto source = CapturerTrackSource::Create();
    auto track = factory->factory()->CreateVideoTrack(rtc::CreateRandomUuid(), source);
    stream->AddTrack(track);
  }

  node_webrtc::Resolve(deferred, MediaStream::wrap()->GetOrCreate(factory, stream));
  return deferred.Promise();
}

void node_webrtc::GetUserMedia::Init(Napi::Env env, Napi::Object exports) {
  exports.Set("getUserMedia", Napi::Function::New(env, GetUserMediaImpl));
}
