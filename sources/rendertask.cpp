/*

Copyright 2013 Adam Reichold

This file is part of qpdfview.

qpdfview is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

qpdfview is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with qpdfview.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "rendertask.h"

#include <qmath.h>
#include <QThreadPool>

#include "model.h"

namespace
{

using namespace qpdfview;

enum
{
    NotCanceled = 0,
    CanceledNormally = 1,
    CanceledForcibly = 2
};

void setCancellation(QAtomicInt& wasCanceled, bool force)
{
#if QT_VERSION > QT_VERSION_CHECK(5,0,0)

    wasCanceled.storeRelease(force ? CanceledForcibly : CanceledNormally);

#else

    wasCanceled.fetchAndStoreRelease(force ? CanceledForcibly : CanceledNormally);

#endif // QT_VERSION
}

void resetCancellation(QAtomicInt& wasCanceled)
{
#if QT_VERSION > QT_VERSION_CHECK(5,0,0)

    wasCanceled.storeRelease(NotCanceled);

#else

    wasCanceled.fetchAndStoreRelease(NotCanceled);

#endif // QT_VERSION
}

bool testCancellation(QAtomicInt& wasCanceled, bool prefetch)
{
#if QT_VERSION >= QT_VERSION_CHECK(5,0,0)

    return prefetch ?
                wasCanceled.load() == CanceledForcibly :
                wasCanceled.load() != NotCanceled;

#else

    return prefetch ?
                wasCanceled.testAndSetRelaxed(CanceledForcibly, CanceledForcibly) :
                !wasCanceled.testAndSetRelaxed(NotCanceled, NotCanceled);

#endif // QT_VERSION
}

int loadWasCanceled(const QAtomicInt& wasCanceled)
{
#if QT_VERSION >= QT_VERSION_CHECK(5,0,0)

    return wasCanceled.load();

#else

    return wasCanceled;

#endif // QT_VERSION
}

qreal scaledResolutionX(const RenderParam& renderParam)
{
    return renderParam.resolution.devicePixelRatio *
            renderParam.resolution.resolutionX * renderParam.scaleFactor;
}

qreal scaledResolutionY(const RenderParam& renderParam)
{
    return renderParam.resolution.devicePixelRatio *
            renderParam.resolution.resolutionY * renderParam.scaleFactor;
}

qreal roundDown(qreal value, qreal precision, qreal tolerance)
{
    return qFloor((1.0 - tolerance) * value * precision) / precision;
}

qreal roundUp(qreal value, qreal precision, qreal tolerance)
{
    return qCeil((1.0 + tolerance) * value * precision) / precision;
}

bool columnHasPaperColor(int x, const QColor& paperColor, const QImage& image)
{
    const int height = image.height();

    for(int y = 0; y < height; ++y)
    {
        if(paperColor != image.pixel(x, y))
        {
            return false;
        }
    }

    return true;
}

bool rowHasPaperColor(int y, const QColor& paperColor, const QImage& image)
{
    const int width = image.width();

    for(int x = 0; x < width; ++x)
    {
        if(paperColor != image.pixel(x, y))
        {
            return false;
        }
    }

    return true;
}

const qreal cropBoxPrecision = 100.0;
const qreal cropBoxTolerance = 0.05;

QRectF trimMargins(const QColor& paperColor, const QImage& image)
{
    if(image.isNull())
    {
        return QRectF(0.0, 0.0, 1.0, 1.0);
    }

    const int width = image.width();
    const int height = image.height();

    int left;
    for(left = 0; left < width; ++left)
    {
        if(!columnHasPaperColor(left, paperColor, image))
        {
            break;
        }
    }

    int right;
    for(right = width - 1; right >= left; --right)
    {
        if(!columnHasPaperColor(right, paperColor, image))
        {
            break;
        }
    }

    int top;
    for(top = 0; top < height; ++top)
    {
        if(!rowHasPaperColor(top, paperColor, image))
        {
            break;
        }
    }

    int bottom;
    for(bottom = height - 1; bottom >= top; --bottom)
    {
        if(!rowHasPaperColor(bottom, paperColor, image))
        {
            break;
        }
    }

    return QRectF(roundDown(static_cast< qreal >(left) / width, cropBoxPrecision, cropBoxTolerance),
                  roundDown(static_cast< qreal >(top) / height, cropBoxPrecision, cropBoxTolerance),
                  roundUp(static_cast< qreal >(right - left + 1) / width, cropBoxPrecision, cropBoxTolerance),
                  roundUp(static_cast< qreal >(bottom - top + 1) / height, cropBoxPrecision, cropBoxTolerance));
}

} // anonymous

namespace qpdfview
{

RenderTask::RenderTask(Model::Page* page, QObject* parent) : QObject(parent), QRunnable(),
    m_isRunning(false),
    m_wasCanceled(NotCanceled),
    m_page(page),
    m_renderParam(),
    m_rect(),
    m_prefetch(false)
{
    setAutoDelete(false);
}

void RenderTask::wait()
{
    QMutexLocker mutexLocker(&m_mutex);

    while(m_isRunning)
    {
        m_waitCondition.wait(&m_mutex);
    }
}

bool RenderTask::isRunning() const
{
    QMutexLocker mutexLocker(&m_mutex);

    return m_isRunning;
}

bool RenderTask::wasCanceled() const
{
    return loadWasCanceled(m_wasCanceled) != NotCanceled;
}

bool RenderTask::wasCanceledNormally() const
{
    return loadWasCanceled(m_wasCanceled) == CanceledNormally;
}

bool RenderTask::wasCanceledForcibly() const
{
    return loadWasCanceled(m_wasCanceled) == CanceledForcibly;
}

void RenderTask::run()
{
    if(testCancellation(m_wasCanceled, m_prefetch))
    {
        finish();
        return;
    }


    QImage image = m_page->render(scaledResolutionX(m_renderParam), scaledResolutionY(m_renderParam),
                                  m_renderParam.rotation, m_rect);

#if QT_VERSION >= QT_VERSION_CHECK(5,1,0)

    image.setDevicePixelRatio(m_renderParam.resolution.devicePixelRatio);

#endif // QT_VERSION


    if(testCancellation(m_wasCanceled, m_prefetch))
    {
        finish();
        return;
    }


    if(m_renderParam.invertColors)
    {
        image.invertPixels();
    }

    emit imageReady(m_renderParam,
                    m_rect, m_prefetch,
                    image);


    if(/* TODO: m_trimMargins */true)
    {
        if(testCancellation(m_wasCanceled, m_prefetch))
        {
            finish();
            return;
        }


        const QRectF cropBox = trimMargins(/* TODO: m_paperColor */Qt::white, image);

        emit cropBoxReady(m_renderParam,
                          m_rect,
                          cropBox);
    }


    finish();
}

void RenderTask::start(const RenderParam& renderParam,
                       const QRect& rect, bool prefetch)
{
    m_renderParam = renderParam;

    m_rect = rect;
    m_prefetch = prefetch;

    m_mutex.lock();
    m_isRunning = true;
    m_mutex.unlock();

    resetCancellation(m_wasCanceled);

    QThreadPool::globalInstance()->start(this);
}

void RenderTask::cancel(bool force)
{
    setCancellation(m_wasCanceled, force);
}

void RenderTask::finish()
{
    emit finished();

    m_mutex.lock();
    m_isRunning = false;
    m_mutex.unlock();

    m_waitCondition.wakeAll();
}

} // qpdfview
