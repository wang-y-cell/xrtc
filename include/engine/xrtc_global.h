#pragma once

#include <memory>

#include "api/scoped_refptr.h"

namespace webrtc {
class Thread;
class AudioDeviceModule;
class PeerConnectionFactoryInterface;
}  // namespace webrtc

namespace xrtc {

class XRtcEngineObserver;
class XrtcAudioDeviceModule;

class XRtcGlobal {
public:
    static XRtcGlobal& instance();

    webrtc::Thread* api_thread() const { return api_thread_.get(); }
    webrtc::Thread* worker_thread() const { return worker_thread_.get(); }
    webrtc::Thread* network_thread() const { return network_thread_.get(); }

    XRtcEngineObserver* observer() const { return observer_; }
    void set_observer(XRtcEngineObserver* observer) { observer_ = observer; }

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
    GetOrCreatePeerConnectionFactory();

    /// 进程内单例 ADM：多次 create/destroy engine 必须复用，否则 PC factory
    /// 仍持有旧 ADM，连续建会易在 Windows 上闪退。
    webrtc::scoped_refptr<XrtcAudioDeviceModule> GetOrCreateAudioDeviceModule();

    webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device() const;

    void InitAudioDeviceModule(
        webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm);

private:
    XRtcGlobal();
    ~XRtcGlobal();

    std::unique_ptr<webrtc::Thread> api_thread_;
    std::unique_ptr<webrtc::Thread> worker_thread_;
    std::unique_ptr<webrtc::Thread> network_thread_;
    XRtcEngineObserver* observer_ = nullptr;
    webrtc::scoped_refptr<XrtcAudioDeviceModule> xrtc_adm_;
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pc_factory_;
};

}  // namespace xrtc
