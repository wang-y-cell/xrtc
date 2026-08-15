#pragma once

#include <memory>

#include "api/audio/audio_device.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "rtc_base/thread.h"

namespace xrtc {

class XRtcEngineObserver;

class XRtcGlobal {
public:
    static XRtcGlobal& instance();

    webrtc::Thread* api_thread() const { return api_thread_.get(); }
    webrtc::Thread* worker_thread() const { return worker_thread_.get(); }
    webrtc::Thread* network_thread() const { return network_thread_.get(); }

    XRtcEngineObserver* observer() const { return observer_; }
    void set_observer(XRtcEngineObserver* observer) { observer_ = observer; }

    /// 懒创建 PeerConnectionFactory（必须在 api_thread 上调用）
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
    GetOrCreatePeerConnectionFactory();

    webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device() const {
        return adm_;
    }

    /// 在 api_thread 上初始化共享 ADM（引擎构造时调用）
    void InitAudioDeviceModule(
        webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm);

private:
    XRtcGlobal();
    ~XRtcGlobal();

    std::unique_ptr<webrtc::Thread> api_thread_;
    std::unique_ptr<webrtc::Thread> worker_thread_;
    std::unique_ptr<webrtc::Thread> network_thread_;
    XRtcEngineObserver* observer_ = nullptr;

    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pc_factory_;
};

}  // namespace xrtc
