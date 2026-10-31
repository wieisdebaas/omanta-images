#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

// EXIF Orientation (1–8). Qt's JPEG and TIFF decoders apply this themselves.
// The WebP and PNG decoders do not, and Qt Quick's Image never does, so a
// phone photo opened as a file URL is shown on its side or upside down.
namespace ImageOrientation {

// 0 when the header has no usable tag. 1 means "already upright".
int orientationOf(const QByteArray &header);

// The leading bytes of `path`, which is where the tag lives.
QByteArray headerFromFile(const QString &path);

// Identity for 0 and 1, and for any value outside 1–8.
QImage apply(const QImage &image, int orientation);

}
