/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2021 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "drm_pipeline.h"

#include <errno.h>

#include "logging.h"
#include "drm_gpu.h"
#include "drm_object_connector.h"
#include "drm_object_crtc.h"
#include "drm_object_plane.h"
#include "drm_buffer.h"
#include "cursor.h"
#include "session.h"
#include "drm_output.h"
#include "drm_backend.h"

#if HAVE_GBM
#include <gbm.h>

#include "egl_gbm_backend.h"
#include "drm_buffer_gbm.h"
#endif

#include <drm_fourcc.h>

namespace KWin
{

DrmPipeline::DrmPipeline(DrmGpu *gpu, DrmConnector *conn, DrmCrtc *crtc, DrmPlane *primaryPlane)
    : m_output(nullptr)
    , m_gpu(gpu)
    , m_connector(conn)
    , m_crtc(crtc)
    , m_primaryPlane(primaryPlane)
{
    m_allObjects << m_connector << m_crtc;
    if (m_primaryPlane) {
        m_allObjects << m_primaryPlane;
    }
}

DrmPipeline::~DrmPipeline()
{
}

void DrmPipeline::setup()
{
    if (!m_gpu->atomicModeSetting()) {
        return;
    }
    if (m_connector->getProp(DrmConnector::PropertyIndex::CrtcId)->current() == m_crtc->id()) {
        m_connector->findCurrentMode(m_crtc->queryCurrentMode());
    }
    m_connector->setPending(DrmConnector::PropertyIndex::CrtcId, m_crtc->id());
    m_crtc->setPending(DrmCrtc::PropertyIndex::Active, 1);
    auto mode = m_connector->currentMode();
    m_crtc->setPendingBlob(DrmCrtc::PropertyIndex::ModeId, &mode.mode, sizeof(drmModeModeInfo));
    m_primaryPlane->setPending(DrmPlane::PropertyIndex::CrtcId, m_crtc->id());
    m_primaryPlane->set(QPoint(0, 0), sourceSize(), QPoint(0, 0), mode.size);
    m_primaryPlane->setTransformation(DrmPlane::Transformation::Rotate0);
}

bool DrmPipeline::test(const QVector<DrmPipeline*> &pipelines)
{
    if (m_gpu->atomicModeSetting()) {
        return checkTestBuffer() && commitPipelines(pipelines, CommitMode::Test);
    } else {
        return true;
    }
}

bool DrmPipeline::test()
{
    return test(m_gpu->pipelines());
}

bool DrmPipeline::present(const QSharedPointer<DrmBuffer> &buffer)
{
    m_primaryBuffer = buffer;
    if (m_gpu->useEglStreams() && m_gpu->eglBackend() != nullptr && m_gpu == m_gpu->platform()->primaryGpu()) {
        // EglStreamBackend queues normal page flips through EGL,
        // modesets etc are performed through DRM-KMS
        bool needsCommit = std::any_of(m_allObjects.constBegin(), m_allObjects.constEnd(), [](auto obj){return obj->needsCommit();});
        if (!needsCommit) {
            return true;
        }
    }
    if (m_gpu->atomicModeSetting()) {
        if (!atomicCommit()) {
            // update properties and try again
            updateProperties();
            if (!atomicCommit()) {
                qCWarning(KWIN_DRM) << "Atomic present failed!" << strerror(errno);
                printDebugInfo();
                return false;
            }
        }
    } else {
        if (!presentLegacy()) {
            qCWarning(KWIN_DRM) << "Present failed!" << strerror(errno);
            return false;
        }
    }
    return true;
}

bool DrmPipeline::atomicCommit()
{
    return commitPipelines({this}, CommitMode::CommitWithPageflipEvent);
}

bool DrmPipeline::commitPipelines(const QVector<DrmPipeline*> &pipelines, CommitMode mode)
{
    Q_ASSERT(!pipelines.isEmpty());

    if (pipelines[0]->m_gpu->atomicModeSetting()) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (!req) {
            qCDebug(KWIN_DRM) << "Failed to allocate drmModeAtomicReq!" << strerror(errno);
            return false;
        }
        uint32_t flags = 0;
        const auto &failed = [pipelines, req](){
            drmModeAtomicFree(req);
            for (const auto &pipeline : pipelines) {
                pipeline->printDebugInfo();
                if (pipeline->m_oldTestBuffer) {
                    pipeline->m_primaryBuffer = pipeline->m_oldTestBuffer;
                    pipeline->m_oldTestBuffer = nullptr;
                }
                for (const auto &obj : qAsConst(pipeline->m_allObjects)) {
                    obj->rollbackPending();
                }
            }
            return false;
        };
        for (const auto &pipeline : pipelines) {
            if (!pipeline->checkTestBuffer()) {
                qCWarning(KWIN_DRM) << "Checking test buffer failed for" << mode;
                return failed();
            }
            if (!pipeline->populateAtomicValues(req, flags)) {
                qCWarning(KWIN_DRM) << "Populating atomic values failed for" << mode;
                return failed();
            }
        }
        if (mode != CommitMode::CommitWithPageflipEvent) {
            flags &= ~DRM_MODE_PAGE_FLIP_EVENT;
        }
        if (drmModeAtomicCommit(pipelines[0]->m_gpu->fd(), req, (flags & (~DRM_MODE_PAGE_FLIP_EVENT)) | DRM_MODE_ATOMIC_TEST_ONLY, pipelines[0]->m_output) != 0) {
            qCWarning(KWIN_DRM) << "Atomic test for" << mode << "failed!" << strerror(errno);
            return failed();
        }
        if (mode != CommitMode::Test && drmModeAtomicCommit(pipelines[0]->m_gpu->fd(), req, flags, pipelines[0]->m_output) != 0) {
            qCWarning(KWIN_DRM) << "Atomic commit failed! This should never happen!" << strerror(errno);
            return failed();
        }
        for (const auto &pipeline : pipelines) {
            pipeline->m_oldTestBuffer = nullptr;
            for (const auto &obj : qAsConst(pipeline->m_allObjects)) {
                obj->commitPending();
            }
            if (mode != CommitMode::Test) {
                pipeline->m_primaryPlane->setNext(pipeline->m_primaryBuffer);
                for (const auto &obj : qAsConst(pipeline->m_allObjects)) {
                    obj->commit();
                }
            }
        }
        drmModeAtomicFree(req);
        return true;
    } else {
        for (const auto &pipeline : pipelines) {
            if (pipeline->m_legacyNeedsModeset && !pipeline->modeset(0)) {
                return false;
            }
        }
        return true;
    }
}

bool DrmPipeline::populateAtomicValues(drmModeAtomicReq *req, uint32_t &flags)
{
    bool usesEglStreams = m_gpu->useEglStreams() && m_gpu->eglBackend() != nullptr && m_gpu == m_gpu->platform()->primaryGpu();
    if (!usesEglStreams && m_active) {
        flags |= DRM_MODE_PAGE_FLIP_EVENT;
    }
    bool needsModeset = std::any_of(m_allObjects.constBegin(), m_allObjects.constEnd(), [](auto obj){return obj->needsModeset();});
    if (needsModeset) {
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    } else {
        flags |= DRM_MODE_ATOMIC_NONBLOCK;
    }
    m_lastFlags = flags;

    if (m_active) {
        m_primaryPlane->set(QPoint(0, 0), m_primaryBuffer->size(), QPoint(0, 0), m_primaryBuffer->size());
        m_primaryPlane->setBuffer(m_primaryBuffer.data());
    } else {
        m_primaryPlane->setBuffer(nullptr);
    }

    for (const auto &obj : qAsConst(m_allObjects)) {
        if (!obj->atomicPopulate(req)) {
            return false;
        }
    }
    return true;
}

bool DrmPipeline::presentLegacy()
{
    if ((!currentBuffer() || currentBuffer()->needsModeChange(m_primaryBuffer.get())) && !modeset(m_connector->currentModeIndex())) {
        return false;
    }
    m_lastFlags = DRM_MODE_PAGE_FLIP_EVENT;
    m_crtc->setNext(m_primaryBuffer);
    if (drmModePageFlip(m_gpu->fd(), m_crtc->id(), m_primaryBuffer ? m_primaryBuffer->bufferId() : 0, DRM_MODE_PAGE_FLIP_EVENT, m_output) != 0) {
        qCWarning(KWIN_DRM) << "Page flip failed:" << strerror(errno) << m_primaryBuffer;
        return false;
    }
    return true;
}

bool DrmPipeline::modeset(int modeIndex)
{
    int oldModeIndex = m_connector->currentModeIndex();
    m_connector->setModeIndex(modeIndex);
    auto mode = m_connector->currentMode().mode;
    if (m_gpu->atomicModeSetting()) {
        m_crtc->setPendingBlob(DrmCrtc::PropertyIndex::ModeId, &mode, sizeof(drmModeModeInfo));
        if (m_connector->hasOverscan()) {
            m_connector->setOverscan(m_connector->overscan(), m_connector->currentMode().size);
        }
        bool works = test();
        // hardware rotation could fail in some modes, try again with soft rotation if possible
        if (!works
            && transformation() != DrmPlane::Transformations(DrmPlane::Transformation::Rotate0)
            && setPendingTransformation(DrmPlane::Transformation::Rotate0)) {
            // values are reset on the failing test, set them again
            m_crtc->setPendingBlob(DrmCrtc::PropertyIndex::ModeId, &mode, sizeof(drmModeModeInfo));
            if (m_connector->hasOverscan()) {
                m_connector->setOverscan(m_connector->overscan(), m_connector->currentMode().size);
            }
            works = test();
        }
        if (!works) {
            qCWarning(KWIN_DRM) << "Modeset failed!" << strerror(errno);
            m_connector->setModeIndex(oldModeIndex);
            return false;
        }
    } else {
        uint32_t connId = m_connector->id();
        if (!checkTestBuffer() || drmModeSetCrtc(m_gpu->fd(), m_crtc->id(), m_primaryBuffer->bufferId(), 0, 0, &connId, 1, &mode) != 0) {
            qCWarning(KWIN_DRM) << "Modeset failed!" << strerror(errno);
            m_connector->setModeIndex(oldModeIndex);
            m_primaryBuffer = m_oldTestBuffer;
            return false;
        }
        m_oldTestBuffer = nullptr;
        m_legacyNeedsModeset = false;
        // make sure the buffer gets kept alive, or the modeset gets reverted by the kernel
        if (m_crtc->current()) {
            m_crtc->setNext(m_primaryBuffer);
        } else {
            m_crtc->setCurrent(m_primaryBuffer);
        }
    }
    return true;
}

bool DrmPipeline::checkTestBuffer()
{
    if (m_primaryBuffer && m_primaryBuffer->size() == sourceSize()) {
        return true;
    }
    if (!m_active) {
        return true;
    }
#if HAVE_GBM
    auto backend = m_gpu->eglBackend();
    if (backend && m_output) {
        auto buffer = backend->renderTestFrame(m_output);
        if (buffer && buffer->bufferId()) {
            m_oldTestBuffer = m_primaryBuffer;
            m_primaryBuffer = buffer;
            return true;
        } else {
            return false;
        }
    }
    // we either don't have a DrmOutput or we're using QPainter
    QSharedPointer<DrmBuffer> buffer;
    if (backend && m_gpu->gbmDevice()) {
        gbm_bo *bo = gbm_bo_create(m_gpu->gbmDevice(), sourceSize().width(), sourceSize().height(), GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!bo) {
            return false;
        }
        buffer = QSharedPointer<DrmGbmBuffer>::create(m_gpu, bo, nullptr);
    } else {
        buffer = QSharedPointer<DrmDumbBuffer>::create(m_gpu, sourceSize());
    }
#else
    auto buffer = QSharedPointer<DrmDumbBuffer>::create(m_gpu, sourceSize());
#endif
    if (buffer && buffer->bufferId()) {
        m_oldTestBuffer = m_primaryBuffer;
        m_primaryBuffer = buffer;
        return true;
    }
    return false;
}

bool DrmPipeline::setCursor(const QSharedPointer<DrmDumbBuffer> &buffer, const QPoint &hotspot)
{
    if (!m_cursor.dirtyBo && m_cursor.buffer == buffer && m_cursor.hotspot == hotspot) {
        return true;
    }
    const QSize &s = buffer ? buffer->size() : QSize(64, 64);
    int ret = drmModeSetCursor2(m_gpu->fd(), m_crtc->id(), buffer ? buffer->handle() : 0, s.width(), s.height(), hotspot.x(), hotspot.y());
    if (ret == -ENOTSUP) {
        // for NVIDIA case that does not support drmModeSetCursor2
        ret = drmModeSetCursor(m_gpu->fd(), m_crtc->id(), buffer ? buffer->handle() : 0, s.width(), s.height());
    }
    if (ret != 0) {
        qCWarning(KWIN_DRM) << "Could not set cursor:" << strerror(errno);
        return false;
    }
    m_cursor.buffer = buffer;
    m_cursor.dirtyBo = false;
    m_cursor.hotspot = hotspot;
    return true;
}

bool DrmPipeline::moveCursor(QPoint pos)
{
    if (!m_cursor.dirtyPos && m_cursor.pos == pos) {
        return true;
    }
    if (drmModeMoveCursor(m_gpu->fd(), m_crtc->id(), pos.x(), pos.y()) != 0) {
        m_cursor.pos = pos;
        return false;
    }
    m_cursor.dirtyPos = false;
    return true;
}

bool DrmPipeline::setActive(bool active)
{
    // disable the cursor before the primary plane to circumvent a crash in amdgpu
    if (m_active && !active) {
        if (drmModeSetCursor(m_gpu->fd(), m_crtc->id(), 0, 0, 0) != 0) {
            qCWarning(KWIN_DRM) << "Could not set cursor:" << strerror(errno);
        }
    }
    bool success = false;
    auto mode = m_connector->currentMode().mode;
    bool oldActive = m_active;
    m_active = active;
    if (m_gpu->atomicModeSetting()) {
        m_connector->setPending(DrmConnector::PropertyIndex::CrtcId, active ? m_crtc->id() : 0);
        m_crtc->setPending(DrmCrtc::PropertyIndex::Active, active);
        m_crtc->setPendingBlob(DrmCrtc::PropertyIndex::ModeId, active ? &mode : nullptr, sizeof(drmModeModeInfo));
        m_primaryPlane->setPending(DrmPlane::PropertyIndex::CrtcId, active ? m_crtc->id() : 0);
        if (active) {
            success = test();
            if (!success) {
                updateProperties();
                success = test();
            }
        } else {
            // immediately commit if disabling as there will be no present
            success = atomicCommit();
        }
    } else {
        auto dpmsProp = m_connector->getProp(DrmConnector::PropertyIndex::Dpms);
        if (!dpmsProp) {
            qCWarning(KWIN_DRM) << "Setting active failed: dpms property missing!";
        } else {
            success = drmModeConnectorSetProperty(m_gpu->fd(), m_connector->id(), dpmsProp->propId(), active ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF) == 0;
        }
    }
    if (!success) {
        m_active = oldActive;
        qCWarning(KWIN_DRM) << "Setting active to" << active << "failed" << strerror(errno);
    }
    if (m_active) {
        // enable cursor (again)
        setCursor(m_cursor.buffer, m_cursor.hotspot);
    }
    return success;
}

bool DrmPipeline::setGammaRamp(const GammaRamp &ramp)
{
    // There are old Intel iGPUs that don't have full support for setting
    // the gamma ramp with AMS -> fall back to legacy without the property
    if (m_gpu->atomicModeSetting() && m_crtc->getProp(DrmCrtc::PropertyIndex::Gamma_LUT)) {
        struct drm_color_lut *gamma = new drm_color_lut[ramp.size()];
        for (uint32_t i = 0; i < ramp.size(); i++) {
            gamma[i].red = ramp.red()[i];
            gamma[i].green = ramp.green()[i];
            gamma[i].blue = ramp.blue()[i];
        }
        bool result = m_crtc->setPendingBlob(DrmCrtc::PropertyIndex::Gamma_LUT, gamma, ramp.size() * sizeof(drm_color_lut));
        delete[] gamma;
        if (!result) {
            qCWarning(KWIN_DRM) << "Could not create gamma LUT property blob" << strerror(errno);
            return false;
        }
        if (!test()) {
            qCWarning(KWIN_DRM) << "Setting gamma failed!" << strerror(errno);
            return false;
        }
    } else {
        uint16_t *red = const_cast<uint16_t*>(ramp.red());
        uint16_t *green = const_cast<uint16_t*>(ramp.green());
        uint16_t *blue = const_cast<uint16_t*>(ramp.blue());
        if (drmModeCrtcSetGamma(m_gpu->fd(), m_crtc->id(), ramp.size(), red, green, blue) != 0) {
            qCWarning(KWIN_DRM) << "setting gamma failed!" << strerror(errno);
            return false;
        }
    }
    return true;
}

bool DrmPipeline::setTransformation(const DrmPlane::Transformations &transformation)
{
    return setPendingTransformation(transformation) && test();
}

bool DrmPipeline::setPendingTransformation(const DrmPlane::Transformations &transformation)
{
    if (this->transformation() == transformation) {
        return true;
    }
    if (!m_gpu->atomicModeSetting()) {
        return false;
    }
    if (!m_primaryPlane->setTransformation(transformation)) {
        return false;
    }
    return true;
}

bool DrmPipeline::setSyncMode(RenderLoopPrivate::SyncMode syncMode)
{
    auto vrrProp = m_crtc->getProp(DrmCrtc::PropertyIndex::VrrEnabled);
    if (!vrrProp || !m_connector->vrrCapable()) {
        return syncMode == RenderLoopPrivate::SyncMode::Fixed;
    }
    bool vrr = syncMode == RenderLoopPrivate::SyncMode::Adaptive;
    if (vrrProp->pending() == vrr) {
        return true;
    }
    if (m_gpu->atomicModeSetting()) {
        vrrProp->setPending(vrr);
        return test();
    } else {
        return drmModeObjectSetProperty(m_gpu->fd(), m_crtc->id(), DRM_MODE_OBJECT_CRTC, vrrProp->propId(), vrr) == 0;
    }
}

bool DrmPipeline::setOverscan(uint32_t overscan)
{
    if (overscan > 100 || (overscan != 0 && !m_connector->hasOverscan())) {
        return false;
    }
    m_connector->setOverscan(overscan, m_connector->currentMode().size);
    return test();
}

bool DrmPipeline::setRgbRange(AbstractWaylandOutput::RgbRange rgbRange)
{
    const auto &prop = m_connector->getProp(DrmConnector::PropertyIndex::Broadcast_RGB);
    if (prop) {
        prop->setEnum(rgbRange);
        return test();
    } else {
        return false;
    }
}

QSize DrmPipeline::sourceSize() const
{
    auto mode = m_connector->currentMode();
    if (transformation() & (DrmPlane::Transformation::Rotate90 | DrmPlane::Transformation::Rotate270)) {
        return mode.size.transposed();
    }
    return mode.size;
}

DrmPlane::Transformations DrmPipeline::transformation() const
{
    return m_primaryPlane ? m_primaryPlane->transformation() : DrmPlane::Transformation::Rotate0;
}

bool DrmPipeline::isActive() const
{
    return m_active;
}

bool DrmPipeline::isCursorVisible() const
{
    return m_cursor.buffer && QRect(m_cursor.pos, m_cursor.buffer->size()).intersects(QRect(QPoint(0, 0), m_connector->currentMode().size));
}

QPoint DrmPipeline::cursorPos() const
{
    return m_cursor.pos;
}

DrmConnector *DrmPipeline::connector() const
{
    return m_connector;
}

DrmCrtc *DrmPipeline::crtc() const
{
    return m_crtc;
}

DrmPlane *DrmPipeline::primaryPlane() const
{
    return m_primaryPlane;
}

DrmBuffer *DrmPipeline::currentBuffer() const
{
    return m_primaryPlane ? m_primaryPlane->current().get() : m_crtc->current().get();
}

void DrmPipeline::pageFlipped()
{
    m_crtc->flipBuffer();
    if (m_primaryPlane) {
        m_primaryPlane->flipBuffer();
    }
}

void DrmPipeline::setOutput(DrmOutput *output)
{
    m_output = output;
}

DrmOutput *DrmPipeline::output() const
{
    return m_output;
}

void DrmPipeline::updateProperties()
{
    for (const auto &obj : qAsConst(m_allObjects)) {
        obj->updateProperties();
    }
    // with legacy we don't know what happened to the cursor after VT switch
    // so make sure it gets set again
    m_cursor.dirtyBo = true;
    m_cursor.dirtyPos = true;
}

bool DrmPipeline::isConnected() const
{
    if (m_primaryPlane) {
        return m_connector->getProp(DrmConnector::PropertyIndex::CrtcId)->current() == m_crtc->id()
            && m_primaryPlane->getProp(DrmPlane::PropertyIndex::CrtcId)->current() == m_crtc->id();
    } else {
        return false;
    }
}

bool DrmPipeline::isFormatSupported(uint32_t drmFormat) const
{
    if (m_gpu->atomicModeSetting()) {
        return m_primaryPlane->formats().contains(drmFormat);
    } else {
        return drmFormat == DRM_FORMAT_XRGB8888 || DRM_FORMAT_ARGB8888;
    }
}

QVector<uint64_t> DrmPipeline::supportedModifiers(uint32_t drmFormat) const
{
    if (m_gpu->atomicModeSetting()) {
        return m_primaryPlane->formats()[drmFormat];
    } else {
        return {};
    }
}

static void printProps(DrmObject *object)
{
    auto list = object->properties();
    for (const auto &prop : list) {
        if (prop) {
            if (prop->isImmutable() || !prop->needsCommit()) {
                qCWarning(KWIN_DRM).nospace() << "\t" << prop->name() << ": " << prop->current();
            } else {
                qCWarning(KWIN_DRM).nospace() << "\t" << prop->name() << ": " << prop->current() << "->" << prop->pending();
            }
        }
    }
}

void DrmPipeline::printDebugInfo() const
{
    if (m_lastFlags == 0) {
        qCWarning(KWIN_DRM) << "Flags: none";
    } else {
        qCWarning(KWIN_DRM) << "Flags:";
        if (m_lastFlags & DRM_MODE_PAGE_FLIP_EVENT) {
            qCWarning(KWIN_DRM) << "\t DRM_MODE_PAGE_FLIP_EVENT";
        }
        if (m_lastFlags & DRM_MODE_ATOMIC_ALLOW_MODESET) {
            qCWarning(KWIN_DRM) << "\t DRM_MODE_ATOMIC_ALLOW_MODESET";
        }
        if (m_lastFlags & DRM_MODE_PAGE_FLIP_ASYNC) {
            qCWarning(KWIN_DRM) << "\t DRM_MODE_PAGE_FLIP_ASYNC";
        }
    }
    qCWarning(KWIN_DRM) << "Drm objects:";
    qCWarning(KWIN_DRM) << "connector" << m_connector->id();
    auto list = m_connector->properties();
    printProps(m_connector);
    qCWarning(KWIN_DRM) << "crtc" << m_crtc->id();
    printProps(m_crtc);
    if (m_primaryPlane) {
        qCWarning(KWIN_DRM) << "primary plane" << m_primaryPlane->id();
        printProps(m_primaryPlane);
    }
}

}
