#include "FacePreviewProvider.h"

#include <QImageReader>
#include <QUrl>

FacePreviewProvider::FacePreviewProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage FacePreviewProvider::requestImage(const QString &id, QSize *size, const QSize &)
{
    const int slash = id.indexOf(QLatin1Char('/'));
    const QString path = QUrl::fromPercentEncoding(id.mid(slash + 1).toUtf8());
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (size)
        *size = image.size();
    return image;
}
