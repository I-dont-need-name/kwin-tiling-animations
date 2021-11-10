/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "qpainterbackend.h"

#include <QObject>
#include <QImage>

namespace KWin
{
class FramebufferBackend;

class FramebufferQPainterBackend : public QPainterBackend
{
    Q_OBJECT
public:
    FramebufferQPainterBackend(FramebufferBackend *backend);
    ~FramebufferQPainterBackend() override;

    QImage *bufferForScreen(AbstractOutput *output) override;
    QRegion beginFrame(AbstractOutput *output) override;
    void endFrame(AbstractOutput *output, const QRegion &damage) override;

private:
    void reactivate();
    void deactivate();

    /**
     * @brief mapped memory buffer on fb device
     */
    QImage m_renderBuffer;
    /**
     * @brief buffer to draw into
     */
    QImage m_backBuffer;

    FramebufferBackend *m_backend;
};

}
