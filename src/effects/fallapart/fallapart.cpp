/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2007 Lubos Lunak <l.lunak@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "fallapart.h"
// KConfigSkeleton
#include "fallapartconfig.h"

#include <cmath>

Q_LOGGING_CATEGORY(KWIN_FALLAPART, "kwin_effect_fallapart", QtWarningMsg)

namespace KWin
{

bool FallApartEffect::supported()
{
    return DeformEffect::supported() && effects->animationsSupported();
}

FallApartEffect::FallApartEffect()
{
    initConfig<FallApartConfig>();
    reconfigure(ReconfigureAll);
    connect(effects, &EffectsHandler::windowClosed, this, &FallApartEffect::slotWindowClosed);
    connect(effects, &EffectsHandler::windowDeleted, this, &FallApartEffect::slotWindowDeleted);
    connect(effects, &EffectsHandler::windowDataChanged, this, &FallApartEffect::slotWindowDataChanged);
}

void FallApartEffect::reconfigure(ReconfigureFlags)
{
    FallApartConfig::self()->read();
    blockSize = FallApartConfig::blockSize();
}

void FallApartEffect::prePaintScreen(ScreenPrePaintData& data, std::chrono::milliseconds presentTime)
{
    if (!windows.isEmpty())
        data.mask |= PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS;
    effects->prePaintScreen(data, presentTime);
}

void FallApartEffect::prePaintWindow(EffectWindow* w, WindowPrePaintData& data, std::chrono::milliseconds presentTime)
{
    auto animationIt = windows.find(w);
    if (animationIt != windows.end() && isRealWindow(w)) {
        if (animationIt->progress < 1) {
            int time = 0;
            if (animationIt->lastPresentTime.count()) {
                time = (presentTime - animationIt->lastPresentTime).count();
            }
            animationIt->lastPresentTime = presentTime;

            animationIt->progress += time / animationTime(1000.);
            data.setTransformed();
            w->enablePainting(EffectWindow::PAINT_DISABLED_BY_DELETE);
        } else {
            unredirect(w);
            windows.remove(w);
            w->unrefWindow();
        }
    }
    effects->prePaintWindow(w, data, presentTime);
}

void FallApartEffect::deform(EffectWindow *w, int mask, WindowPaintData &data, WindowQuadList &quads)
{
    Q_UNUSED(w)
    Q_UNUSED(mask)
    auto animationIt = windows.constFind(w);
    if (animationIt != windows.constEnd() && isRealWindow(w)) {
        const qreal t = animationIt->progress;
        // Request the window to be divided into cells
        quads = quads.makeGrid(blockSize);
        int cnt = 0;
        for (WindowQuad &quad : quads) {
            // make fragments move in various directions, based on where
            // they are (left pieces generally move to the left, etc.)
            QPointF p1(quad[ 0 ].x(), quad[ 0 ].y());
            double xdiff = 0;
            if (p1.x() < w->width() / 2)
                xdiff = -(w->width() / 2 - p1.x()) / w->width() * 100;
            if (p1.x() > w->width() / 2)
                xdiff = (p1.x() - w->width() / 2) / w->width() * 100;
            double ydiff = 0;
            if (p1.y() < w->height() / 2)
                ydiff = -(w->height() / 2 - p1.y()) / w->height() * 100;
            if (p1.y() > w->height() / 2)
                ydiff = (p1.y() - w->height() / 2) / w->height() * 100;
            double modif = t * t * 64;
            srandom(cnt);   // change direction randomly but consistently
            xdiff += (rand() % 21 - 10);
            ydiff += (rand() % 21 - 10);
            for (int j = 0;
                    j < 4;
                    ++j) {
                quad[ j ].move(quad[ j ].x() + xdiff * modif, quad[ j ].y() + ydiff * modif);
            }
            // also make the fragments rotate around their center
            QPointF center((quad[ 0 ].x() + quad[ 1 ].x() + quad[ 2 ].x() + quad[ 3 ].x()) / 4,
                           (quad[ 0 ].y() + quad[ 1 ].y() + quad[ 2 ].y() + quad[ 3 ].y()) / 4);
            double adiff = (rand() % 720 - 360) / 360. * 2 * M_PI;   // spin randomly
            for (int j = 0;
                    j < 4;
                    ++j) {
                double x = quad[ j ].x() - center.x();
                double y = quad[ j ].y() - center.y();
                double angle = atan2(y, x);
                angle += animationIt->progress * adiff;
                double dist = sqrt(x * x + y * y);
                x = dist * cos(angle);
                y = dist * sin(angle);
                quad[ j ].move(center.x() + x, center.y() + y);
            }
            ++cnt;
        }
        data.multiplyOpacity(interpolate(1.0, 0.0, t));
    }
}

void FallApartEffect::postPaintScreen()
{
    if (!windows.isEmpty())
        effects->addRepaintFull();
    effects->postPaintScreen();
}

bool FallApartEffect::isRealWindow(EffectWindow* w)
{
    // TODO: isSpecialWindow is rather generic, maybe tell windowtypes separately?
    /*
    qCDebug(KWIN_FALLAPART) << "--" << w->caption() << "--------------------------------";
    qCDebug(KWIN_FALLAPART) << "Tooltip:" << w->isTooltip();
    qCDebug(KWIN_FALLAPART) << "Toolbar:" << w->isToolbar();
    qCDebug(KWIN_FALLAPART) << "Desktop:" << w->isDesktop();
    qCDebug(KWIN_FALLAPART) << "Special:" << w->isSpecialWindow();
    qCDebug(KWIN_FALLAPART) << "TopMenu:" << w->isTopMenu();
    qCDebug(KWIN_FALLAPART) << "Notific:" << w->isNotification();
    qCDebug(KWIN_FALLAPART) << "Splash:" << w->isSplash();
    qCDebug(KWIN_FALLAPART) << "Normal:" << w->isNormalWindow();
    */
    if (w->isPopupWindow()) {
        return false;
    }
    if (w->isX11Client() && !w->isManaged()) {
        return false;
    }
    if (!w->isNormalWindow())
        return false;
    return true;
}

void FallApartEffect::slotWindowClosed(EffectWindow* c)
{
    if (!isRealWindow(c))
        return;
    if (!c->isVisible())
        return;
    const void* e = c->data(WindowClosedGrabRole).value<void*>();
    if (e && e != this)
        return;
    c->setData(WindowClosedGrabRole, QVariant::fromValue(static_cast<void*>(this)));
    windows[ c ].progress = 0;
    c->refWindow();
    redirect(c);
}

void FallApartEffect::slotWindowDeleted(EffectWindow* c)
{
    windows.remove(c);
}

void FallApartEffect::slotWindowDataChanged(EffectWindow* w, int role)
{
    if (role != WindowClosedGrabRole) {
        return;
    }

    if (w->data(role).value<void*>() == this) {
        return;
    }

    auto it = windows.find(w);
    if (it == windows.end()) {
        return;
    }

    unredirect(it.key());
    it.key()->unrefWindow();
    windows.erase(it);
}

bool FallApartEffect::isActive() const
{
    return !windows.isEmpty();
}

} // namespace
