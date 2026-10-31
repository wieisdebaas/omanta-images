#include "FacePreviewProvider.h"

#include "ImageOrientation.h"

#include <QImageIOHandler>
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
    reader.setAllocationLimit(1024);
    QImage image = reader.read();
    // JPEG/TIFF already rotated. WebP and PNG leave the tag for us; applying
    // it when the plugin already did would turn the picture upside down again.
    if (!image.isNull() && reader.transformation() == QImageIOHandler::TransformationNone) {
        image = ImageOrientation::apply(image,
            ImageOrientation::orientationOf(ImageOrientation::headerFromFile(path)));
    }
    if (size)
        *size = image.size();
    return image;
}
