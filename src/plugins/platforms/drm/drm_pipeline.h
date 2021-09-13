/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2021 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QPoint>
#include <QSize>
#include <QVector>
#include <QSharedPointer>

#include <xf86drmMode.h>

#include "drm_object_plane.h"
#include "renderloop_p.h"
#include "abstract_wayland_output.h"

namespace KWin
{

class DrmGpu;
class DrmConnector;
class DrmCrtc;
class DrmBuffer;
class DrmDumbBuffer;
class GammaRamp;

class DrmPipeline
{
public:
    DrmPipeline(DrmGpu *gpu, DrmConnector *conn, DrmCrtc *crtc, DrmPlane *primaryPlane);
    ~DrmPipeline();

    /**
     * Sets the necessary initial drm properties for the pipeline to work
     */
    void setup();
    void addOutput(DrmConnector *conn, DrmCrtc *crtc, DrmPlane *primaryPlane);

    /**
     * checks if the connector(s) and plane(s) are set to the CRTC(s)
     * always returns false in legacy mode
     */
    bool isConnected() const;

    /**
     * checks if all tiles of the display are included in this Pipeline
     */
    bool isComplete() const;

    /**
     * tests the pending commit first and commits it if the test passes
     * if the test fails, there is a guarantee for no lasting changes
     */
    bool present(const QSharedPointer<DrmBuffer> &buffer);

    /**
     * tests the pending commit
     * always returns true in legacy mode!
     */
    bool test(const QVector<DrmPipeline*> &pipelines);

    bool modeset(int modeIndex);
    bool setCursor(const QSharedPointer<DrmDumbBuffer> &buffer, const QPoint &hotspot = QPoint());
    bool setActive(bool enable);
    bool setGammaRamp(const GammaRamp &ramp);
    bool setTransformation(const DrmPlane::Transformations &transform);
    bool moveCursor(QPoint pos);
    bool setSyncMode(RenderLoopPrivate::SyncMode syncMode);
    bool setOverscan(uint32_t overscan);
    bool setRgbRange(AbstractWaylandOutput::RgbRange rgbRange);

    DrmPlane::Transformations transformation() const;
    bool isActive() const;
    bool isCursorVisible() const;
    QPoint cursorPos() const;

    QVector<DrmConnector*> connectors() const;
    QVector<DrmCrtc*> crtcs() const;
    QVector<DrmPlane*> primaryPlanes() const;

    DrmBuffer *currentBuffer() const;

    void pageFlipped();
    void printDebugInfo() const;
    QSize sourceSize() const;
    void updateProperties();

    bool isFormatSupported(uint32_t drmFormat) const;
    QVector<uint64_t> supportedModifiers(uint32_t drmFormat) const;

    void setOutput(DrmOutput *output);
    DrmOutput *output() const;

    struct Mode {
        QSize size;
        uint32_t refreshRate;
        bool preferred;
    };
    QVector<Mode> modeList() const;
    Mode currentMode() const;
    int modeIndex() const;
    int tilingGroup() const;

    bool vrrCapable() const;
    bool hasOverscan() const;

    enum class CommitMode {
        Test,
        Commit,
        CommitWithPageflipEvent
    };
    Q_ENUM(CommitMode);
    static bool commitPipelines(const QVector<DrmPipeline*> &pipelines, CommitMode mode);

private:
    bool populateAtomicValues(drmModeAtomicReq *req, uint32_t &flags);
    bool test();
    bool atomicCommit();
    bool presentLegacy();
    bool checkTestBuffer();
    QSize rotated(const QSize &size) const;

    bool setPendingTransformation(const DrmPlane::Transformations &transformation);

    DrmOutput *m_output = nullptr;
    DrmGpu *m_gpu = nullptr;
    QVector<DrmConnector*> m_connectors;
    QVector<DrmCrtc*> m_crtcs;

    QVector<DrmPlane*> m_primaryPlanes;
    QSharedPointer<DrmBuffer> m_primaryBuffer;
    QSharedPointer<DrmBuffer> m_oldTestBuffer;

    bool m_active = true;
    bool m_legacyNeedsModeset = true;
    struct {
        QPoint pos;
        QPoint hotspot;
        QSharedPointer<DrmDumbBuffer> buffer;
        bool dirtyBo = true;
        bool dirtyPos = true;
    } m_cursor;
    QVector<DrmObject*> m_allObjects;

    int m_lastFlags = 0;
};

}
