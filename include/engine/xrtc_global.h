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
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pc_factory_;
};

}  // namespace xrtc
