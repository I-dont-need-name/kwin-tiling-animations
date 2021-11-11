/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "renderbackend.h"

namespace KWin
{

RenderBackend::RenderBackend(QObject *parent)
    : QObject(parent)
{
}

OverlayWindow *RenderBackend::overlayWindow() const
{
    return nullptr;
}

QList<QByteArray> RenderBackend::openGLPlatformInterfaceExtensions() const
{
    return QList<QByteArray>{};
}

} // namespace KWin
