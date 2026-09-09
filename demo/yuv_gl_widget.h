#pragma once

#include <QMutex>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <xrtc/xrtc_defines.h>

/// I420 → OpenGL 三平面纹理，片元着色器转 RGB（KeepAspectRatio）
class YuvGlWidget : public QOpenGLWidget, protected QOpenGLExtraFunctions {
    Q_OBJECT

public:
    explicit YuvGlWidget(QWidget* parent = nullptr);
    ~YuvGlWidget() override;

    /// UI 线程调用：缓存最新一帧并触发重绘
    void setFrame(const xrtc::XRTCVideoFrame& frame);
    void clearFrame();

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

private:
    void ensureProgram();
    ///确保纹理已分配
    void ensureTextures(int width, int height);
    void uploadFrame(const xrtc::XRTCVideoFrame& frame);
    void destroyGl();

    QMutex mutex_;
    //最新一帧 I420（含 storage，保证平面指针有效）
    xrtc::XRTCVideoFrame pending_; 
    //true 表示有新帧待上传；paintGL 取走后清掉
    bool dirty_ = false; 
    ///clearFrame() 置位，让 paintGL 在 GL 线程清画面，避免跨线程乱改纹理
    bool clear_requested_ = false; 
    //是否已有成功上传的纹理可画；没有就只清背景
    bool has_frame_ = false;

    ///YUV→RGB 的 shader 程序
    QOpenGLShaderProgram* program_ = nullptr;
    GLuint vao_ = 0; ///顶点数组对象（全屏四边形）
    GLuint vbo_ = 0; ///顶点缓冲（位置 + UV）
    GLuint tex_y_ = 0; ///Y 平面纹理
    GLuint tex_u_ = 0; ///U 平面纹理
    GLuint tex_v_ = 0; ///V 平面纹理
    int tex_w_ = 0; //纹理宽度
    int tex_h_ = 0; ///纹理高度
    int video_w_ = 0; ///视频宽度
    int video_h_ = 0; ///视频高度
};
