/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "qpainterbackend.h"

#include <QObject>
#include <QVector>
#include <QMap>

namespace KWin
{

class VirtualBackend;

class VirtualQPainterBackend : public QPainterBackend
{
    Q_OBJECT
public:
    VirtualQPainterBackend(VirtualBackend *backend);
    ~VirtualQPainterBackend() override;

    QImage *bufferForScreen(AbstractOutput *output) override;
    QRegion beginFrame(AbstractOutput *output) override;
    void endFrame(AbstractOutput *output, const QRegion &damage) override;

private:
    void createOutputs();

    QMap<AbstractOutput *, QImage> m_backBuffers;
    VirtualBackend *m_backend;
    int m_frameCounter = 0;
};

}
