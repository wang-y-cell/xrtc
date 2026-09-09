#include "yuv_gl_widget.h"

#include <algorithm>

namespace {

constexpr const char* kVertSrc = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv;
}
)";

// BT.601 limited-range YUV → RGB
constexpr const char* kFragSrc = R"(#version 330 core
in vec2 v_uv;
out vec4 fragColor;
uniform sampler2D tex_y;
uniform sampler2D tex_u;
uniform sampler2D tex_v;
void main() {
    float y = texture(tex_y, v_uv).r;
    float u = texture(tex_u, v_uv).r - 0.5;
    float v = texture(tex_v, v_uv).r - 0.5;
    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;
    fragColor = vec4(r, g, b, 1.0);
}
)";

}  // namespace

YuvGlWidget::YuvGlWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(160, 120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
}

YuvGlWidget::~YuvGlWidget() {
    makeCurrent();
    destroyGl();
    doneCurrent();
}

void YuvGlWidget::setFrame(const xrtc::XRTCVideoFrame& frame) {
    if (!frame.valid()) {
        return;
    }
    {
        QMutexLocker lock(&mutex_);
        pending_ = frame;
        dirty_ = true;
    }
    update();
}

void YuvGlWidget::clearFrame() {
    {
        QMutexLocker lock(&mutex_);
        pending_ = {};
        dirty_ = false;
        clear_requested_ = true;
    }
    update();
}

void YuvGlWidget::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    ensureProgram();

    static const float kQuad[] = {
        // pos      uv
        -1.f, -1.f, 0.f, 1.f, 1.f,  -1.f, 1.f, 1.f,
        -1.f, 1.f,  0.f, 0.f, 1.f,  1.f,  1.f, 0.f,
    };

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glBindVertexArray(0);
}

void YuvGlWidget::ensureProgram() {
    if (program_) {
        return;
    }
    program_ = new QOpenGLShaderProgram(this);
    if (!program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertSrc) ||
        !program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragSrc) ||
        !program_->link()) {
        qWarning("YuvGlWidget: shader compile/link failed: %s",
                 qPrintable(program_->log()));
        delete program_;
        program_ = nullptr;
    }
}

void YuvGlWidget::ensureTextures(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    if (tex_y_ != 0 && tex_w_ == width && tex_h_ == height) {
        return;
    }

    auto alloc_tex = [this](GLuint& tex, int w, int h) {
        if (tex == 0) {
            glGenTextures(1, &tex);
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED,
                     GL_UNSIGNED_BYTE, nullptr);
    };

    const int cw = std::max(1, (width + 1) / 2);
    const int ch = std::max(1, (height + 1) / 2);
    alloc_tex(tex_y_, width, height); //分配Y平面纹理
    alloc_tex(tex_u_, cw, ch); //分配U平面纹理  
    alloc_tex(tex_v_, cw, ch); //分配V平面纹理
    tex_w_ = width; //纹理宽度
    tex_h_ = height; //纹理高度
}

void YuvGlWidget::uploadFrame(const xrtc::XRTCVideoFrame& frame) {
    ensureTextures(frame.width, frame.height);
    if (tex_y_ == 0) {
        return;
    }

    const int cw = std::max(1, (frame.width + 1) / 2);
    const int ch = std::max(1, (frame.height + 1) / 2);

    //告诉 OpenGL：按字节对齐读 CPU 数据。视频行常常不是 4 字节对齐，不设成 1 可能花屏。
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    auto upload = [this](GLuint tex, int w, int h, int stride,
                         const uint8_t* data) {
        glBindTexture(GL_TEXTURE_2D, tex); //选中要写的那张纹理
        //每行在内存里实际跨度（可能 > 图像宽）
        glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
        //把 w×h 的像素写进纹理；格式 GL_RED = 单通道
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE,
                        data);
    };

    upload(tex_y_, frame.width, frame.height, frame.stride_y, frame.data_y);
    upload(tex_u_, cw, ch, frame.stride_u, frame.data_u);
    upload(tex_v_, cw, ch, frame.stride_v, frame.data_v);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    video_w_ = frame.width;
    video_h_ = frame.height;
    has_frame_ = true;
}

void YuvGlWidget::paintGL() {
    xrtc::XRTCVideoFrame frame;
    bool do_clear = false;
    {
        QMutexLocker lock(&mutex_);
        if (clear_requested_) {
            do_clear = true;
            clear_requested_ = false;
            pending_ = {};
            dirty_ = false;
        } else if (dirty_) {
            frame = std::move(pending_);
            pending_ = {};
            dirty_ = false;
        }
    }
    if (do_clear) {
        has_frame_ = false;
        video_w_ = 0;
        video_h_ = 0;
    } else if (frame.valid()) {
        uploadFrame(frame);
    }

    glViewport(0, 0, width() * devicePixelRatio(),
               height() * devicePixelRatio());
    glClear(GL_COLOR_BUFFER_BIT);

    if (!has_frame_ || !program_ || video_w_ <= 0 || video_h_ <= 0) {
        return;
    }

    const int fb_w = width() * devicePixelRatio();
    const int fb_h = height() * devicePixelRatio();
    const float widget_aspect =
        fb_h > 0 ? static_cast<float>(fb_w) / static_cast<float>(fb_h) : 1.f;
    const float video_aspect =
        static_cast<float>(video_w_) / static_cast<float>(video_h_);

    int vp_w = fb_w;
    int vp_h = fb_h;
    int vp_x = 0;
    int vp_y = 0;
    if (widget_aspect > video_aspect) {
        vp_h = fb_h;
        vp_w = static_cast<int>(fb_h * video_aspect);
        vp_x = (fb_w - vp_w) / 2;
    } else {
        vp_w = fb_w;
        vp_h = static_cast<int>(fb_w / video_aspect);
        vp_y = (fb_h - vp_h) / 2;
    }
    glViewport(vp_x, vp_y, std::max(1, vp_w), std::max(1, vp_h));

    program_->bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y_);
    program_->setUniformValue("tex_y", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_u_);
    program_->setUniformValue("tex_u", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_v_);
    program_->setUniformValue("tex_v", 2);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    program_->release();
}

void YuvGlWidget::resizeGL(int, int) {}

void YuvGlWidget::destroyGl() {
    if (tex_y_) {
        glDeleteTextures(1, &tex_y_);
        tex_y_ = 0;
    }
    if (tex_u_) {
        glDeleteTextures(1, &tex_u_);
        tex_u_ = 0;
    }
    if (tex_v_) {
        glDeleteTextures(1, &tex_v_);
        tex_v_ = 0;
    }
    if (vbo_) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    delete program_;
    program_ = nullptr;
    tex_w_ = tex_h_ = 0;
}
