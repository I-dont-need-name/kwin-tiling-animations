/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2019 NVIDIA Inc.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "drm_abstract_egl_backend.h"
#include "basiceglsurfacetexture_wayland.h"
#include <KWaylandServer/surface_interface.h>
#include <KWaylandServer/eglstream_controller_interface.h>
#include <wayland-server-core.h>

namespace KWin
{

class DrmAbstractOutput;
class DrmDumbBuffer;
class DumbSwapchain;
class ShadowBuffer;
class DrmCrtc;
class DrmPlane;

/**
 * @brief OpenGL Backend using Egl with an EGLDevice.
 */
class DrmEglStreamBackend : public DrmAbstractEglBackend
{
    Q_OBJECT
public:
    DrmEglStreamBackend(DrmBackend *b, DrmGpu *gpu);
    ~DrmEglStreamBackend() override;
    SurfaceTexture *createSurfaceTextureInternal(SurfacePixmapInternal *pixmap) override;
    SurfaceTexture *createSurfaceTextureWayland(SurfacePixmapWayland *pixmap) override;
    QRegion beginFrame(AbstractOutput *output) override;
    void endFrame(AbstractOutput *output, const QRegion &damage, const QRegion &damagedRegion) override;
    void init() override;

    bool hasOutput(AbstractOutput *output) const override;
    bool addOutput(DrmAbstractOutput *output) override;
    void removeOutput(DrmAbstractOutput *output) override;

    QSharedPointer<DrmBuffer> renderTestFrame(DrmAbstractOutput *output) override;
    uint32_t drmFormat() const override;

protected:
    void cleanupSurfaces() override;

private:
    bool initializeEgl();
    bool initBufferConfigs();
    bool initRenderingContext();
    struct StreamTexture
    {
        EGLStreamKHR stream;
        GLuint texture;
    };
    StreamTexture *lookupStreamTexture(KWaylandServer::SurfaceInterface *surface);
    void destroyStreamTexture(KWaylandServer::SurfaceInterface *surface);
    void attachStreamConsumer(KWaylandServer::SurfaceInterface *surface,
                              void *eglStream,
                              wl_array *attribs);
    struct Output
    {
        DrmOutput *output = nullptr;
        QSharedPointer<DrmDumbBuffer> buffer;
        EGLSurface eglSurface = EGL_NO_SURFACE;
        EGLStreamKHR eglStream = EGL_NO_STREAM_KHR;
        QSharedPointer<ShadowBuffer> shadowBuffer;

        DrmPlane *targetPlane = nullptr;

        // for operation as secondary GPU
        QSharedPointer<DumbSwapchain> dumbSwapchain;
    };
    bool resetOutput(Output &output);
    bool createEglSurface(Output &o);
    bool makeContextCurrent(const Output &output);
    void cleanupOutput(Output &output);
    bool needsReset(const Output &o) const;

    QMap<AbstractOutput *, Output> m_outputs;
    KWaylandServer::EglStreamControllerInterface *m_eglStreamControllerInterface;
    QHash<KWaylandServer::SurfaceInterface *, StreamTexture> m_streamTextures;

    friend class EglStreamSurfaceTextureWayland;
};

class EglStreamSurfaceTextureWayland : public BasicEGLSurfaceTextureWayland
{
public:
    EglStreamSurfaceTextureWayland(DrmEglStreamBackend *backend, SurfacePixmapWayland *pixmap);
    ~EglStreamSurfaceTextureWayland() override;

    bool create() override;
    void update(const QRegion &region) override;

private:
    bool acquireStreamFrame(EGLStreamKHR stream);
    void createFbo();
    void copyExternalTexture(GLuint tex);
    bool attachBuffer(KWaylandServer::ClientBuffer *buffer);
    bool checkBuffer(KWaylandServer::SurfaceInterface *surface,
                     KWaylandServer::ClientBuffer *buffer);

    DrmEglStreamBackend *m_backend;
    GLuint m_fbo, m_rbo, m_textureId;
    GLenum m_format;
};

} // namespace
