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
    /**
     * @brief 获取XRtcGlobal实例
     * @return XRtcGlobal实例
    */
    static XRtcGlobal& instance();

    /**
     * @brief 获取API线程
     * @return API线程
    */
    webrtc::Thread* api_thread() const { return api_thread_.get(); }
    /**
     * @brief 获取Worker线程
     * @return Worker线程
    */
    webrtc::Thread* worker_thread() const { return worker_thread_.get(); }
    /**
     * @brief 获取Network线程
     * @return Network线程
    */
    webrtc::Thread* network_thread() const { return network_thread_.get(); }
    /**
     * @brief 获取观察者
     * @return 观察者
    */
    XRtcEngineObserver* observer() const { return observer_; }
    /**
     * @brief 设置观察者
     * @param observer 观察者
    */
    void set_observer(XRtcEngineObserver* observer) { observer_ = observer; }

    /// 懒创建 PeerConnectionFactory（必须在 api_thread 上调用）
    ///WebRTC 全局工厂，用来创建 AudioTrack、VideoTrack 等。
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

    /**
     *@brief  API线程
     * 在构造函数创建,并设置名称,并启动
    */
    std::unique_ptr<webrtc::Thread> api_thread_;
    /**
     *@brief  Worker线程
     * 在构造函数创建,并设置名称,并启动
    */
    std::unique_ptr<webrtc::Thread> worker_thread_;
    /**
     *@brief  Network线程
     * network_thread 延迟到创建 PeerConnectionFactory 时再启动
    */
    std::unique_ptr<webrtc::Thread> network_thread_;
    /**
     *@brief 观察者,设置观察者时候,在观察者实现重载函数中,会调用观察者实现的重载函数
     * 在set_observer函数中设置观察者对象
     * 这样我们可以在任何地方调用观察者对象的函数,实现回调
    */
    XRtcEngineObserver* observer_ = nullptr;

    /**
     *@brief 音频设备模块
     * 在InitAudioDeviceModule函数中创建
    */
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
    /**
     *@brief 媒体处理工厂
     * 在GetOrCreatePeerConnectionFactory函数中创建,用于创建媒体处理对象
    */
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pc_factory_;
};

}  // namespace xrtc
