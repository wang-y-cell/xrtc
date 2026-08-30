#include <engine/xrtc_global.h>

#include "api/audio/audio_device.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/peer_connection_interface.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_dav1d_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"
#include <spdlog/spdlog.h>

namespace xrtc {

XRtcGlobal& XRtcGlobal::instance() {
    static XRtcGlobal instance;
    return instance;
}

XRtcGlobal::XRtcGlobal()
    : api_thread_(webrtc::Thread::Create()),
      worker_thread_(webrtc::Thread::Create()) {
    api_thread_->SetName("api_thread", nullptr);
    api_thread_->Start();
    worker_thread_->SetName("worker_thread", nullptr);
    worker_thread_->Start();
    /*
     * network_thread 延迟到创建 PeerConnectionFactory 时再启动
     *（避免过早拉起 PhysicalSocketServer）。
     */
}

XRtcGlobal::~XRtcGlobal() {
    api_thread_->BlockingCall([this]() {
        pc_factory_ = nullptr;
        adm_ = nullptr;
    });
    api_thread_->Stop();
    worker_thread_->Stop();
    if (network_thread_) {
        network_thread_->Stop();
    }
}

void XRtcGlobal::InitAudioDeviceModule(
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm) {
    adm_ = std::move(adm);
}

webrtc::scoped_refptr<webrtc::AudioDeviceModule> XRtcGlobal::audio_device()
    const {
    return adm_;
}

webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
XRtcGlobal::GetOrCreatePeerConnectionFactory() {
    if (pc_factory_) {
        return pc_factory_;
    }

    if (!network_thread_) {
        network_thread_ = webrtc::Thread::CreateWithSocketServer();
        network_thread_->SetName("network_thread", nullptr);
        network_thread_->Start();
        RTC_LOG(LS_INFO) << "network_thread started (deferred until PC factory)";
    }

    if (adm_) {
        adm_->Init();
    }

    pc_factory_ = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(), worker_thread_.get(), api_thread_.get(), adm_,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        //视频编码器工厂,用来创建视频编码器,目前写死可以使用以下编解码器
        std::make_unique<webrtc::VideoEncoderFactoryTemplate<
            webrtc::LibvpxVp8EncoderTemplateAdapter,
            webrtc::LibvpxVp9EncoderTemplateAdapter,
            webrtc::OpenH264EncoderTemplateAdapter,
            webrtc::LibaomAv1EncoderTemplateAdapter>>(),
        std::make_unique<webrtc::VideoDecoderFactoryTemplate<
            webrtc::LibvpxVp8DecoderTemplateAdapter,
            webrtc::LibvpxVp9DecoderTemplateAdapter,
            webrtc::OpenH264DecoderTemplateAdapter,
            webrtc::Dav1dDecoderTemplateAdapter>>(),
        /*audio_mixer=*/nullptr,
        /*audio_processing=*/nullptr);

    if (!pc_factory_) {
        spdlog::error("创建PeerConnectionFactory失败");
    }
    return pc_factory_;
}

}  // namespace xrtc
