#include "voice/WebRtcProcessor.h"

#include <algorithm>
#include <array>

#ifdef VOICECHAT_ENABLE_WEBRTC
#include <api/audio/audio_processing.h>
#endif

namespace voicechat {

struct WebRtcProcessor::Impl {
    Options options;
#ifdef VOICECHAT_ENABLE_WEBRTC
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig capture{kRate, 1};
    webrtc::StreamConfig render{kRate, 2};
#endif
    explicit Impl(Options o) : options(o) { options.estimatedDelayMs = std::clamp(o.estimatedDelayMs, 0, 500); }
};

WebRtcProcessor::WebRtcProcessor(Options options) : d_(std::make_unique<Impl>(options)) {
#ifdef VOICECHAT_ENABLE_WEBRTC
    if (!options.voiceEchoCancellation && !options.noiseSuppression) return;

    webrtc::AudioProcessing::Config cfg;
    cfg.pipeline.multi_channel_render = true;
    cfg.echo_canceller.enabled = options.voiceEchoCancellation;
    cfg.echo_canceller.mobile_mode = false; // AEC3, rather than the older AECM
    cfg.noise_suppression.enabled = options.noiseSuppression;
    cfg.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
    // Keep automatic gain controllers off. The user controls sender gain, and
    // an unexpected automatic boost is undesirable in a feedback-prone setup.
    cfg.gain_controller1.enabled = false;
    cfg.gain_controller2.enabled = false;
    cfg.pre_amplifier.enabled = false;
    cfg.capture_level_adjustment.enabled = false;

    d_->apm = webrtc::AudioProcessingBuilder().SetConfig(cfg).Create();
    if (!d_->apm) return;
    webrtc::ProcessingConfig streams;
    streams.input_stream() = d_->capture;
    streams.output_stream() = d_->capture;
    streams.reverse_input_stream() = d_->render;
    streams.reverse_output_stream() = d_->render;
    if (d_->apm->Initialize(streams) != webrtc::AudioProcessing::kNoError) d_->apm = nullptr;
#endif
}

WebRtcProcessor::~WebRtcProcessor() = default;

bool WebRtcProcessor::compiledIn() {
#ifdef VOICECHAT_ENABLE_WEBRTC
    return true;
#else
    return false;
#endif
}

bool WebRtcProcessor::ready() const {
#ifdef VOICECHAT_ENABLE_WEBRTC
    return d_->apm != nullptr;
#else
    return false;
#endif
}

bool WebRtcProcessor::processRender(const std::int16_t *stereo) {
#ifdef VOICECHAT_ENABLE_WEBRTC
    if (!d_->apm || !d_->options.voiceEchoCancellation || !stereo) return false;
    std::array<std::int16_t, 2 * kBlock> scratch{};
    return d_->apm->ProcessReverseStream(stereo, d_->render, d_->render, scratch.data()) ==
           webrtc::AudioProcessing::kNoError;
#else
    (void)stereo;
    return false;
#endif
}

bool WebRtcProcessor::processCapture(const std::int16_t *input, std::int16_t *output) {
#ifdef VOICECHAT_ENABLE_WEBRTC
    if (!d_->apm || !input || !output) return false;
    for (int i = 0; i < kCaptureFrame; i += kBlock) {
        if (d_->options.voiceEchoCancellation &&
            d_->apm->set_stream_delay_ms(d_->options.estimatedDelayMs) != webrtc::AudioProcessing::kNoError)
            return false;
        if (d_->apm->ProcessStream(input + i, d_->capture, d_->capture, output + i) !=
            webrtc::AudioProcessing::kNoError)
            return false;
    }
    return true;
#else
    (void)input;
    (void)output;
    return false;
#endif
}

} // namespace voicechat
