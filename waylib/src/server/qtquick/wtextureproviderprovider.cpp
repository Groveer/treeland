// Copyright (C) 2024-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "wtextureproviderprovider.h"
#include "woutputrenderwindow.h"
#include "private/wglobal_p.h"

#include <rhi/qrhi.h>

WAYLIB_SERVER_BEGIN_NAMESPACE
Q_LOGGING_CATEGORY(qLcTextureProvider, "waylib.server.texture.provider")

class Q_DECL_HIDDEN WTextureCapturerPrivate : public WObjectPrivate
{
public:
    W_DECLARE_PUBLIC(WTextureCapturer)
    explicit WTextureCapturerPrivate(WTextureCapturer *qq, WTextureProviderProvider *p)
        : WObjectPrivate(qq)
        , provider(p)
        , renderWindow(p->outputRenderWindow())
    {}

    QPromise<QImage> imgPromise;
    WTextureProviderProvider *const provider;
    WOutputRenderWindow *const renderWindow;
};

static void cleanupRbResult(void *rbResult)
{
    delete reinterpret_cast<QRhiReadbackResult*>(rbResult);
}

WTextureCapturer::WTextureCapturer(WTextureProviderProvider *provider, QObject *parent)
    : QObject(parent)
    , WObject(*new WTextureCapturerPrivate(this, provider))
{

}

QFuture<QImage> WTextureCapturer::grabToImage()
{
    W_D(WTextureCapturer);
    auto future = d->imgPromise.future();
    // Do NOT moveToThread here. Keeping this object on the main thread avoids
    // V4 GC ScarceResourceData corruption: if the object (or any QML-tracked
    // parent/child) holds a QVariant wrapping QImage/QPixmap, destroying it on
    // the render thread while the V4 GC sweeps scarceResources on the main
    // thread corrupts the QIntrusiveListNode chain (prev = 0xFFFFFFFF).
    //
    // Use DirectConnection so that doGrabToImage runs in the render thread's
    // signal emission context (renderEnd is emitted from the render thread),
    // which is required for RHI GPU readback operations.
    connect(d->renderWindow,
            &WOutputRenderWindow::renderEnd,
            this,
            &WTextureCapturer::doGrabToImage,
            Qt::DirectConnection);
    if (!d->renderWindow->inRendering()) {
        // Request a frame so that renderEnd will be emitted soon.
        d->renderWindow->update();
    }
    return future;
}

void WTextureCapturer::doGrabToImage()
{
    W_D(WTextureCapturer);
    if (d->imgPromise.isCanceled())
        return;
    d->imgPromise.start();
    WSGTextureProvider *textureProvider = d->provider->wTextureProvider();
    if (textureProvider && textureProvider->texture() && textureProvider->texture()->rhiTexture()) {
        // Perform rhi texture read back
        auto texture = textureProvider->texture()->rhiTexture();
        qCInfo(qLcTextureProvider) << "Perform rhi texture read back for texture" << texture;
        QRhiReadbackResult *rbResult = new QRhiReadbackResult;
        QRhiCommandBuffer *cb;
        d->renderWindow->rhi()->beginOffscreenFrame(&cb);
        auto ub = d->renderWindow->rhi()->nextResourceUpdateBatch();
        cb->beginComputePass(ub);
        QRhiReadbackDescription rbd(texture);
        ub->readBackTexture(rbd, rbResult);
        cb->endComputePass(ub);
        auto frameOpResult = d->renderWindow->rhi()->endOffscreenFrame();
        if (frameOpResult == QRhi::FrameOpSuccess) {
            d->imgPromise.addResult(QImage(reinterpret_cast<const uchar *>(rbResult->data.constData()),
                                       rbResult->pixelSize.width(),
                                       rbResult->pixelSize.height(),
                                       QImage::Format_RGBA8888_Premultiplied,
                                       cleanupRbResult,
                                       rbResult)); // TODO: Automatically get data format.
        } else {
            d->imgPromise.setException(std::make_exception_ptr(std::runtime_error("Offscreen frame operation failed.")));
        }
    } else {
        d->imgPromise.setException(std::make_exception_ptr(std::runtime_error("Texture provider is not valid.")));
    }
    d->imgPromise.finish();

    // Disconnect after one-shot readback. No moveToThread needed — this object
    // never left the main thread. The DirectConnection slot ran in the render
    // thread's signal context, but the object's thread affinity is unchanged.
    disconnect(d->renderWindow,
               &WOutputRenderWindow::renderEnd,
               this,
               &WTextureCapturer::doGrabToImage);
}

WAYLIB_SERVER_END_NAMESPACE
