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
{
    addOutput(conn, crtc, primaryPlane);
}

DrmPipeline::~DrmPipeline()
{
}

void DrmPipeline::addOutput(DrmConnector *conn, DrmCrtc *crtc, DrmPlane *primaryPlane)
{
    Q_ASSERT_X(m_allObjects.isEmpty() || (m_gpu->atomicModeSetting() && !m_gpu->useEglStreams()), "DrmPipeline::addOutput", "Tiled displays require gbm and atomic modesetting");
    m_connectors << conn;
    m_crtcs << crtc;
    m_allObjects << conn << crtc;
    if (primaryPlane) {
        m_primaryPlanes << primaryPlane;
        m_allObjects << primaryPlane;
    }
}

void DrmPipeline::setup()
{
    if (!m_gpu->atomicModeSetting()) {
        return;
    }
    for (int i = 0; i < m_connectors.count(); i++) {
        auto mode = m_connectors[i]->currentMode();
        m_connectors[i]->setPending(DrmConnector::PropertyIndex::CrtcId, m_crtcs[i]->id());
        m_crtcs[i]->setPending(DrmCrtc::PropertyIndex::Active, 1);
        m_crtcs[i]->setPendingBlob(DrmCrtc::PropertyIndex::ModeId, &mode.mode, sizeof(drmModeModeInfo));
        m_primaryPlanes[i]->setPending(DrmPlane::PropertyIndex::CrtcId, m_crtcs[i]->id());
        m_primaryPlanes[i]->set(QPoint(0, 0), sourceSize(), QPoint(0, 0), mode.size);
        m_primaryPlanes[i]->setTransformation(DrmPlane::Transformation::Rotate0);
    }
    checkTestBuffer();
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
                for (const auto &plane : qAsConst(pipeline->m_primaryPlanes)) {
                    plane->setNext(pipeline->m_primaryBuffer);
                }
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

    for (int i = 0; i < m_connectors.count(); i++) {
        auto modeSize = m_connectors[i]->currentMode().size;
        m_primaryPlanes[i]->set(m_connectors[i]->tilePos(), rotated(modeSize), QPoint(0, 0), modeSize);
        m_primaryPlanes[i]->setBuffer(m_active ? m_primaryBuffer.get() : nullptr);
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
    if ((!currentBuffer() || currentBuffer()->needsModeChange(m_primaryBuffer.get())) && !modeset(modeIndex())) {
        return false;
    }
    m_lastFlags = DRM_MODE_PAGE_FLIP_EVENT;
    m_crtcs.first()->setNext(m_primaryBuffer);
    if (drmModePageFlip(m_gpu->fd(), m_crtcs.first()->id(), m_primaryBuffer ? m_primaryBuffer->bufferId() : 0, DRM_MODE_PAGE_FLIP_EVENT, m_output) != 0) {
        qCWarning(KWIN_DRM) << "Page flip failed:" << strerror(errno) << m_primaryBuffer;
        return false;
    }
    return true;
}

bool DrmPipeline::modeset(int wantedMode)
{
    if (m_gpu->atomicModeSetting()) {
        auto setValues = [this, wantedMode](){
            for (int i = 0; i < m_connectors.count(); i++) {
                auto &conn = m_connectors[i];
                conn->setModeIndex(wantedMode);
                auto mode = conn->currentMode();
                m_crtcs[i]->setPendingBlob(DrmCrtc::PropertyIndex::ModeId, &mode.mode, sizeof(drmModeModeInfo));
                if (conn->hasOverscan()) {
                    conn->setOverscan(conn->overscan(), mode.size);
                }
            }
        };
        setValues();
        bool works = test();
        // hardware rotation could fail in some modes, try again with soft rotation if possible
        if (!works
            && transformation() != DrmPlane::Transformations(DrmPlane::Transformation::Rotate0)
            && setPendingTransformation(DrmPlane::Transformation::Rotate0)) {
            // values are reset on the failing test, set them again
            setValues();
            works = test();
        }
        if (!works) {
            qCWarning(KWIN_DRM) << "Modeset failed!" << strerror(errno);
            return false;
        }
    } else {
        const auto &crtc = m_crtcs.first();
        const auto &conn = m_connectors.first();
        int oldModeIndex = modeIndex();
        conn->setModeIndex(wantedMode);
        auto mode = conn->currentMode().mode;
        uint32_t connId = conn->id();
        if (!checkTestBuffer() || drmModeSetCrtc(m_gpu->fd(), crtc->id(), m_primaryBuffer->bufferId(), 0, 0, &connId, 1, &mode) != 0) {
            qCWarning(KWIN_DRM) << "Modeset failed!" << strerror(errno);
            conn->setModeIndex(oldModeIndex);
            m_primaryBuffer = m_oldTestBuffer;
            return false;
        }
        m_oldTestBuffer = nullptr;
        m_legacyNeedsModeset = false;
        // make sure the buffer gets kept alive, or the modeset gets reverted by the kernel
        if (crtc->current()) {
            crtc->setNext(m_primaryBuffer);
        } else {
            crtc->setCurrent(m_primaryBuffer);
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
    for (const auto &crtc : qAsConst(m_crtcs)) {
        int ret = drmModeSetCursor2(m_gpu->fd(), crtc->id(), buffer ? buffer->handle() : 0, s.width(), s.height(), hotspot.x(), hotspot.y());
        if (ret == -ENOTSUP) {
            // for NVIDIA case that does not support drmModeSetCursor2
            ret = drmModeSetCursor(m_gpu->fd(), crtc->id(), buffer ? buffer->handle() : 0, s.width(), s.height());
        }
        if (ret != 0) {
            qCWarning(KWIN_DRM) << "Could not set cursor:" << strerror(errno);
            return false;
        }
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
    for (const auto &crtc : qAsConst(m_crtcs)) {
        if (drmModeMoveCursor(m_gpu->fd(), crtc->id(), pos.x(), pos.y()) != 0) {
            m_cursor.pos = pos;
            return false;
        }
    }
    m_cursor.dirtyPos = false;
    return true;
}

bool DrmPipeline::setActive(bool active)
{
    // disable the cursor before the primary plane to circumvent a crash in amdgpu
    if (m_active && !active) {
        for (const auto &crtc : qAsConst(m_crtcs)) {
            if (drmModeSetCursor(m_gpu->fd(), crtc->id(), 0, 0, 0) != 0) {
                qCWarning(KWIN_DRM) << "Could not set cursor:" << strerror(errno);
            }
        }
    }
    bool success = false;
    bool oldActive = m_active;
    m_active = active;
    if (m_gpu->atomicModeSetting()) {
        for (int i = 0; i < m_connectors.count(); i++) {
            auto mode = m_connectors[i]->currentMode().mode;
            m_connectors[i]->setPending(DrmConnector::PropertyIndex::CrtcId, active ? m_crtcs[i]->id() : 0);
            m_crtcs[i]->setPending(DrmCrtc::PropertyIndex::Active, active);
            m_crtcs[i]->setPendingBlob(DrmCrtc::PropertyIndex::ModeId, active ? &mode : nullptr, sizeof(drmModeModeInfo));
            m_primaryPlanes[i]->setPending(DrmPlane::PropertyIndex::CrtcId, active ? m_crtcs[i]->id() : 0);
        }
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
        auto dpmsProp = m_connectors.first()->getProp(DrmConnector::PropertyIndex::Dpms);
        if (!dpmsProp) {
            qCWarning(KWIN_DRM) << "Setting active failed: dpms property missing!";
        } else {
            success = drmModeConnectorSetProperty(m_gpu->fd(), m_connectors.first()->id(), dpmsProp->propId(), active ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF) == 0;
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
    if (m_gpu->atomicModeSetting() && m_crtcs.first()->getProp(DrmCrtc::PropertyIndex::Gamma_LUT)) {
        struct drm_color_lut *gamma = new drm_color_lut[ramp.size()];
        for (uint32_t i = 0; i < ramp.size(); i++) {
            gamma[i].red = ramp.red()[i];
            gamma[i].green = ramp.green()[i];
            gamma[i].blue = ramp.blue()[i];
        }
        bool result = true;
        for (const auto &crtc : qAsConst(m_crtcs)) {
            result &= crtc->setPendingBlob(DrmCrtc::PropertyIndex::Gamma_LUT, gamma, ramp.size() * sizeof(drm_color_lut));
            if (!result) {
                break;
            }
        }
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
        for (const auto &crtc : qAsConst(m_crtcs)) {
            if (drmModeCrtcSetGamma(m_gpu->fd(), crtc->id(), ramp.size(), red, green, blue) != 0) {
                qCWarning(KWIN_DRM) << "setting gamma failed!" << strerror(errno);
                return false;
            }
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
    bool result = true;
    for (const auto &plane : qAsConst(m_primaryPlanes)) {
        result &= plane->setTransformation(transformation);
        if (!result) {
            break;
        }
    }
    if (!result) {
        for (const auto &obj : qAsConst(m_primaryPlanes)) {
            obj->rollbackPending();
        }
        return false;
    }
    return true;
}

bool DrmPipeline::setSyncMode(RenderLoopPrivate::SyncMode syncMode)
{
    if (!vrrCapable()) {
        return syncMode == RenderLoopPrivate::SyncMode::Fixed;
    }
    bool vrr = syncMode == RenderLoopPrivate::SyncMode::Adaptive;
    if (m_gpu->atomicModeSetting()) {
        bool success = true;
        bool needsTest = false;
        for (const auto &crtc : qAsConst(m_crtcs)) {
            auto vrrProp = crtc->getProp(DrmCrtc::PropertyIndex::VrrEnabled);
            if (!vrrProp) {
                success = false;
                break;
            } else if (vrrProp->pending() != vrr) {
                needsTest = true;
                vrrProp->setPending(vrr);
            }
        }
        return success && (!needsTest || test());
    } else {
        auto vrrProp = m_crtcs.first()->getProp(DrmCrtc::PropertyIndex::VrrEnabled);
        return vrrProp && drmModeObjectSetProperty(m_gpu->fd(), m_crtcs.first()->id(), DRM_MODE_OBJECT_CRTC, vrrProp->propId(), vrr) == 0;
    }
}

bool DrmPipeline::setOverscan(uint32_t overscan)
{
    if (overscan > 100 || m_connectors.count() > 1 || (overscan != 0 && !m_connectors.first()->hasOverscan())) {
        return false;
    }
    m_connectors.first()->setOverscan(overscan, m_connectors.first()->currentMode().size);
    return test();
}

bool DrmPipeline::setRgbRange(AbstractWaylandOutput::RgbRange rgbRange)
{
    // FIXME
    const auto &prop = m_connectors.first()->getProp(DrmConnector::PropertyIndex::Broadcast_RGB);
    if (prop) {
        prop->setEnum(rgbRange);
        return test();
    } else {
        return false;
    }
}

QSize DrmPipeline::rotated(const QSize &size) const
{
    if (transformation() & (DrmPlane::Transformation::Rotate90 | DrmPlane::Transformation::Rotate270)) {
        return size.transposed();
    }
    return size;
}

QSize DrmPipeline::sourceSize() const
{
    auto size = m_connectors.first()->totalModeSize(modeIndex());
    if (transformation() & (DrmPlane::Transformation::Rotate90 | DrmPlane::Transformation::Rotate270)) {
        return size.transposed();
    }
    return size;
}

DrmPlane::Transformations DrmPipeline::transformation() const
{
    return m_primaryPlanes.count() ? m_primaryPlanes.first()->transformation() : DrmPlane::Transformation::Rotate0;
}

bool DrmPipeline::isActive() const
{
    return m_active;
}

bool DrmPipeline::isCursorVisible() const
{
    return m_cursor.buffer && QRect(m_cursor.pos, m_cursor.buffer->size()).intersects(QRect(QPoint(0, 0), m_connectors.first()->totalModeSize(modeIndex())));
}

QPoint DrmPipeline::cursorPos() const
{
    return m_cursor.pos;
}

QVector<DrmConnector*> DrmPipeline::connectors() const
{
    return m_connectors;
}

QVector<DrmCrtc*> DrmPipeline::crtcs() const
{
    return m_crtcs;
}

QVector<DrmPlane*> DrmPipeline::primaryPlanes() const
{
    return m_primaryPlanes;
}

DrmBuffer *DrmPipeline::currentBuffer() const
{
    return m_primaryPlanes.count() ? m_primaryPlanes.first()->current().get() : m_crtcs.first()->current().get();
}

void DrmPipeline::pageFlipped()
{
    for (const auto &obj : qAsConst(m_allObjects)) {
        if (auto crtc = dynamic_cast<DrmCrtc*>(obj)) {
            crtc->flipBuffer();
        } else if (auto plane = dynamic_cast<DrmPlane*>(obj)) {
            plane->flipBuffer();
        }
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
    if (m_primaryPlanes.count()) {
        for (int i = 0; i < m_connectors.count(); i++) {
            if (m_connectors[i]->getProp(DrmConnector::PropertyIndex::CrtcId)->current() != m_crtcs[i]->id()
                || m_primaryPlanes[i]->getProp(DrmPlane::PropertyIndex::CrtcId)->current() != m_crtcs[i]->id()) {
                return false;
            }
        }
        return true;
    } else {
        return false;
    }
}

bool DrmPipeline::isFormatSupported(uint32_t drmFormat) const
{
    if (m_gpu->atomicModeSetting()) {
        // FIXME directly save mapping of format -> modifiers, like in DrmPlane
        return m_primaryPlanes.first()->formats().contains(drmFormat);
    } else {
        return drmFormat == DRM_FORMAT_XRGB8888 || DRM_FORMAT_ARGB8888;
    }
}

QVector<uint64_t> DrmPipeline::supportedModifiers(uint32_t drmFormat) const
{
    if (m_gpu->atomicModeSetting()) {
        // FIXME directly save mapping of format -> modifiers, like in DrmPlane
        return m_primaryPlanes.first()->formats()[drmFormat];
    } else {
        return {};
    }
}

bool DrmPipeline::isComplete() const
{
    if (m_connectors.first()->isTiled()) {
        if (m_gpu->useEglStreams()) {
            // not supported with eglstreams
            return true;
        }
        int width = m_connectors.first()->tilingInfo().num_tiles_x;
        int height = m_connectors.first()->tilingInfo().num_tiles_y;
        for (int x = 0; x < width; x++) {
            for (int y = 0; y < height; y++) {
                // find connector that fills the current 1x1 tile
                bool tileFound = false;
                for (const auto &conn : qAsConst(m_connectors)) {
                    const auto &info = conn->tilingInfo();
                    if (x >= info.loc_x && x <= info.loc_x + info.tile_width
                        && y >= info.loc_y && y <= info.loc_y + info.tile_height) {
                        tileFound = true;
                        break;
                    }
                }
                if (!tileFound) {
                    return false;
                }
            }
        }
        return true;
    } else {
        return true;
    }
}

int DrmPipeline::modeIndex() const
{
    return m_connectors.first()->modeIndex();
}

QVector<DrmPipeline::Mode> DrmPipeline::modeList() const
{
    QVector<Mode> modeList;
    const auto modes = m_connectors.first()->modes();
    for (int i = 0; i < modes.count(); i++) {
        Mode m;
        m.size = m_connectors.first()->totalModeSize(i);
        m.refreshRate = modes[i].refreshRate;
        m.preferred = modes[i].mode.type & DRM_MODE_TYPE_PREFERRED;
        modeList << m;
    }
    return modeList;
}

DrmPipeline::Mode DrmPipeline::currentMode() const
{
    Mode m;
    m.size = m_connectors.first()->totalModeSize(modeIndex());
    m.refreshRate = m_connectors.first()->currentMode().refreshRate;
    m.preferred = m_connectors.first()->currentMode().mode.type & DRM_MODE_TYPE_PREFERRED;
    return m;
}

bool DrmPipeline::vrrCapable() const
{
    return std::all_of(m_connectors.constBegin(), m_connectors.constEnd(), [](const auto &conn){return conn->vrrCapable();});
}

bool DrmPipeline::hasOverscan() const
{
    return m_connectors.count() > 1 ? false : m_connectors.first()->hasOverscan();
}

int DrmPipeline::tilingGroup() const
{
    return m_connectors.first()->tilingInfo().group_id;
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
    for (int i = 0; i < m_connectors.count(); i++) {
        qCWarning(KWIN_DRM) << "connector" << m_connectors[i]->id();
        auto list = m_connectors[i]->properties();
        printProps(m_connectors[i]);
        qCWarning(KWIN_DRM) << "crtc" << m_crtcs[i]->id();
        printProps(m_crtcs[i]);
        if (m_primaryPlanes[i]) {
            qCWarning(KWIN_DRM) << "primary plane" << m_primaryPlanes[i]->id();
            printProps(m_primaryPlanes[i]);
        }
    }
}

}
