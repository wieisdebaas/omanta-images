#include "ImageOrientation.h"

#include <QFile>
#include <QImageIOHandler>
#include <QTransform>

namespace ImageOrientation {
namespace {

constexpr int kHeaderBytes = 256 * 1024;

int u16(const uchar *data, bool little)
{
    return little ? data[0] | (data[1] << 8) : (data[0] << 8) | data[1];
}

quint32 u32(const uchar *data, bool little)
{
    return little
        ? quint32(data[0]) | (quint32(data[1]) << 8) | (quint32(data[2]) << 16) | (quint32(data[3]) << 24)
        : (quint32(data[0]) << 24) | (quint32(data[1]) << 16) | (quint32(data[2]) << 8) | quint32(data[3]);
}

// First IFD only. Orientation is tag 0x0112 and is stored inline.
int orientationInTiff(const uchar *tiff, int length)
{
    if (length < 8)
        return 0;
    const bool little = tiff[0] == 'I' && tiff[1] == 'I';
    const bool big = tiff[0] == 'M' && tiff[1] == 'M';
    if (!little && !big)
        return 0;
    if (u16(tiff + 2, little) != 42)
        return 0;
    const int ifd = int(u32(tiff + 4, little));
    if (ifd < 0 || ifd + 2 > length)
        return 0;
    const int count = u16(tiff + ifd, little);
    if (count < 1 || count > 64 || ifd + 2 + count * 12 > length)
        return 0;
    for (int i = 0; i < count; ++i) {
        const uchar *entry = tiff + ifd + 2 + i * 12;
        if (u16(entry, little) != 0x0112)
            continue;
        if (u16(entry + 2, little) != 3) // SHORT
            return 0;
        const int orientation = u16(entry + 8, little);
        return orientation >= 1 && orientation <= 8 ? orientation : 0;
    }
    return 0;
}

int orientationInExif(const QByteArray &payload)
{
    if (payload.size() >= 6 && payload.startsWith(QByteArray("Exif\0\0", 6)))
        return orientationInTiff(reinterpret_cast<const uchar *>(payload.constData() + 6),
                                 payload.size() - 6);
    return orientationInTiff(reinterpret_cast<const uchar *>(payload.constData()), payload.size());
}

int orientationInJpeg(const QByteArray &data)
{
    const auto *bytes = reinterpret_cast<const uchar *>(data.constData());
    const int size = data.size();
    if (size < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8)
        return 0;
    int i = 2;
    while (i + 4 < size) {
        if (bytes[i] != 0xFF)
            return 0;
        while (i < size && bytes[i] == 0xFF)
            ++i;
        if (i >= size)
            return 0;
        const int marker = bytes[i++];
        if (marker == 0xD9 || marker == 0xDA)
            return 0;
        if (marker >= 0xD0 && marker <= 0xD7)
            continue;
        if (i + 2 > size)
            return 0;
        const int length = (bytes[i] << 8) | bytes[i + 1];
        if (length < 2 || i + length > size)
            return 0;
        if (marker == 0xE1) {
            const int found = orientationInExif(data.mid(i + 2, length - 2));
            if (found)
                return found;
        }
        i += length;
    }
    return 0;
}

int orientationInWebp(const QByteArray &data)
{
    if (data.size() < 12 || !data.startsWith("RIFF") || data.mid(8, 4) != "WEBP")
        return 0;
    int i = 12;
    while (i + 8 <= data.size()) {
        const QByteArray tag = data.mid(i, 4);
        const quint32 chunk = u32(reinterpret_cast<const uchar *>(data.constData() + i + 4), true);
        i += 8;
        if (chunk > quint32(data.size() - i))
            return 0;
        if (tag == "EXIF")
            return orientationInExif(data.mid(i, int(chunk)));
        i += int(chunk) + int(chunk & 1);
    }
    return 0;
}

int orientationInPng(const QByteArray &data)
{
    static const char signature[] = "\x89PNG\r\n\x1a\n";
    if (data.size() < 8 || !data.startsWith(QByteArray(signature, 8)))
        return 0;
    int i = 8;
    while (i + 8 <= data.size()) {
        const quint32 length = u32(reinterpret_cast<const uchar *>(data.constData() + i), false);
        const QByteArray type = data.mid(i + 4, 4);
        if (length > quint32(data.size() - i - 12))
            return 0;
        if (type == "eXIf")
            return orientationInExif(data.mid(i + 8, int(length)));
        if (type == "IDAT" || type == "IEND")
            return 0;
        i += 12 + int(length);
    }
    return 0;
}

// The same composition Qt uses once a decoder reports ImageTransformation,
// so a WebP we rotate ourselves matches a JPEG the plugin rotated.
QImageIOHandler::Transformations qtTransform(int orientation)
{
    using T = QImageIOHandler::Transformation;
    switch (orientation) {
    case 2: return T::TransformationMirror;
    case 3: return T::TransformationRotate180;
    case 4: return T::TransformationFlip;
    // Tags 5 and 7 are the transposed mirrors. Matching Qt's JPEG output
    // (not the numeric value of QImageIOHandler::transformation(), which
    // names the two the other way round) keeps a WebP beside a JPEG of the
    // same shot.
    case 5: return T::TransformationMirrorAndRotate90;
    case 6: return T::TransformationRotate90;
    case 7: return T::TransformationFlipAndRotate90;
    case 8: return T::TransformationRotate270;
    default: return T::TransformationNone;
    }
}

} // namespace

int orientationOf(const QByteArray &header)
{
    if (header.size() < 8)
        return 0;
    if (const int jpeg = orientationInJpeg(header))
        return jpeg;
    if (const int webp = orientationInWebp(header))
        return webp;
    if (const int png = orientationInPng(header))
        return png;
    const int exif = header.indexOf(QByteArray("Exif\0\0", 6));
    if (exif >= 0)
        return orientationInTiff(reinterpret_cast<const uchar *>(header.constData() + exif + 6),
                                 header.size() - exif - 6);
    return 0;
}

QByteArray headerFromFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.read(kHeaderBytes);
}

QImage apply(const QImage &image, int orientation)
{
    const auto transform = qtTransform(orientation);
    if (image.isNull() || transform == QImageIOHandler::TransformationNone)
        return image;

    // Mirror, then flip, then a quarter turn. Qt applies a reported tag in
    // this order; doing anything else would disagree with the JPEG decoder.
    QTransform xf;
    if (transform & QImageIOHandler::TransformationMirror)
        xf.scale(-1, 1);
    if (transform & QImageIOHandler::TransformationFlip)
        xf.scale(1, -1);
    if (transform & QImageIOHandler::TransformationRotate90)
        xf.rotate(90);
    return image.transformed(xf, Qt::FastTransformation);
}

} // namespace ImageOrientation
