#pragma once

#include <QQuickImageProvider>

// The photo popup's pixels, with the same orientation the face boxes were
// measured in. Qt Quick's Image does not apply that orientation itself, so
// a phone photo would otherwise show boxes on the unrotated frame.
class FacePreviewProvider : public QQuickImageProvider
{
public:
    FacePreviewProvider();

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
