#include "ThumbnailProvider.h"

#include "Location.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QMimeDatabase>
#include <QMutex>
#include <QPointer>
#include <QProcess>
#include <QStorageInfo>
#include <QQuickImageResponse>
#include <QRunnable>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThread>
#include <QThreadPool>
#include <QUrl>

#include <gio/gio.h>

#include <atomic>
#include <limits>
#include <memory>
#include <mutex>

#include <sys/stat.h>
#include <sys/sysmacros.h>

// Test seam: a cancelled response must still persist a decode that already
// finished. The suite pauses the worker between render and store.
namespace OmantaThumbnailTest {
std::atomic<bool> *g_pauseBeforePhotoStore = nullptr;
std::atomic<bool> *g_reachedBeforePhotoStore = nullptr;
}

namespace {

QMutex g_registryMutex;
bool g_registryLoaded = false;
QHash<QString, QStringList> g_thumbnailers; // mime type → argv template
QMutex g_photoCacheMutex;
QCache<QString, QImage> g_photoMemoryCache;
qint64 g_photoMemoryLimit = PhotoThumbnailCache::DefaultMemoryLimit;
QMutex g_photoPriorityMutex;
QHash<QString, int> g_photoPriority;
PhotoThumbnailProvider *g_photoProvider = nullptr;
QMutex g_ioClientsMutex;
QHash<QObject *, bool> g_ioClients;

bool anyPhotoIoClientSlow()
{
    for (auto it = g_ioClients.cbegin(); it != g_ioClients.cend(); ++it) {
        if (it.value())
            return true;
    }
    return false;
}

void applyPhotoIoBound()
{
    if (!g_photoProvider)
        return;
    QMutexLocker lock(&g_ioClientsMutex);
    const bool bound = anyPhotoIoClientSlow();
    lock.unlock();
    g_photoProvider->setIoBound(bound);
}

QString thumbnailRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
           + QStringLiteral("/thumbnails");
}

// The spec keys the cache on the MD5 of the file's URI — not its path — so the
// hash must match byte for byte what other applications produce.
QString hashFor(const QString &filePath)
{
    const QByteArray uri = QUrl::fromLocalFile(filePath).toEncoded();
    return QString::fromLatin1(QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex());
}

QString uriFor(const QString &filePath)
{
    return QString::fromLatin1(QUrl::fromLocalFile(filePath).toEncoded());
}

QString photoKey(const QString &filePath, int bucket, const ThumbnailCache::Version &version)
{
    // Paths from the directory/photo models are already absolute. canonicalFilePath()
    // is a realpath syscall — catastrophic on SMB/gvfs/external disks when paid
    // per thumbnail.
    const QByteArray identity = filePath.toUtf8() + '\0'
        + QByteArray::number(version.size) + '\0'
        + QByteArray::number(version.modifiedMSecs) + '\0'
        + QByteArray::number(bucket);
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

QByteArray photoFormat()
{
    static const QByteArray format = [] {
        const QList<QByteArray> supported = QImageWriter::supportedImageFormats();
        if (!supported.contains("webp"))
            return QByteArrayLiteral("png");

        QImage sample(512, 512, QImage::Format_RGB32);
        for (int y = 0; y < sample.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(sample.scanLine(y));
            for (int x = 0; x < sample.width(); ++x)
                line[x] = qRgb((x * 13 + y * 3) & 255,
                               (x * 5 + y * 11) & 255,
                               (x * 7 + y * 17) & 255);
        }

        const auto score = [&sample](const QByteArray &candidate) {
            QByteArray encoded;
            QBuffer buffer(&encoded);
            buffer.open(QIODevice::WriteOnly);
            QImageWriter writer(&buffer, candidate);
            if (candidate == QByteArrayLiteral("webp"))
                writer.setQuality(82);
            QElapsedTimer timer;
            timer.start();
            if (!writer.write(sample))
                return std::numeric_limits<qint64>::max();
            const qint64 encodeNsecs = timer.nsecsElapsed();

            timer.restart();
            for (int iteration = 0; iteration < 3; ++iteration) {
                if (QImage::fromData(encoded, candidate.constData()).isNull())
                    return std::numeric_limits<qint64>::max();
            }
            const qint64 decodeNsecs = timer.nsecsElapsed() / 3;
            // Reads and decodes dominate repeat visits; generation is paid
            // once. One byte per nanosecond approximates fast local storage.
            return decodeNsecs + encodeNsecs / 8 + encoded.size();
        };

        return score(QByteArrayLiteral("webp")) < score(QByteArrayLiteral("png"))
            ? QByteArrayLiteral("webp") : QByteArrayLiteral("png");
    }();
    return format;
}

} // namespace

int ThumbnailCache::bucketFor(int requestedSize)
{
    if (requestedSize <= 128)
        return 128;
    if (requestedSize <= 256)
        return 256;
    if (requestedSize <= 512)
        return 512;
    return 1024;
}

QString ThumbnailCache::bucketName(int bucket)
{
    switch (bucket) {
    case 128: return QStringLiteral("normal");
    case 256: return QStringLiteral("large");
    case 512: return QStringLiteral("x-large");
    default: return QStringLiteral("xx-large");
    }
}

QString ThumbnailCache::cachePathFor(const QString &filePath, int bucket)
{
    return QStringLiteral("%1/%2/%3.png")
        .arg(thumbnailRoot(), bucketName(bucket), hashFor(filePath));
}

QString ThumbnailCache::failMarkerFor(const QString &filePath)
{
    // Kept under our own name so a failure here never suppresses another
    // application's attempt, and vice versa.
    return QStringLiteral("%1/fail/omanta/%2.png").arg(thumbnailRoot(), hashFor(filePath));
}

QImage ThumbnailCache::loadValid(const QString &filePath, int bucket)
{
    const QString cached = cachePathFor(filePath, bucket);
    if (!QFileInfo::exists(cached))
        return {};

    QImage image(cached);
    if (image.isNull())
        return {};

    // Thumb::MTime is what makes the cache correct rather than merely fast: an
    // edited file must not keep showing its old preview.
    const QFileInfo info(filePath);
    const QString recorded = image.text(QStringLiteral("Thumb::MTime"));
    if (recorded.isEmpty() || recorded.toLongLong() != info.lastModified().toSecsSinceEpoch())
        return {};

    // Thumb::MTime has one-second resolution, so a picture re-saved within
    // the second it was written would pass that check. Size (optional in the
    // spec) and our own millisecond stamp catch those, where present.
    const QString size = image.text(QStringLiteral("Thumb::Size"));
    if (!size.isEmpty() && size.toLongLong() != info.size())
        return {};
    const QString msecs = image.text(QStringLiteral("X-Omanta::MTime-MSec"));
    if (!msecs.isEmpty() && msecs.toLongLong() != info.lastModified().toMSecsSinceEpoch())
        return {};

    return image;
}

namespace {

// QFileInfo only sees native paths. smb:// and the other GIO URIs need the
// same mtime formula as FileEntry, or the photo grid treats a fresh decode
// as "file changed" and throws it away.
ThumbnailCache::Version versionViaGio(const QString &uri)
{
    GFile *file = Location::make(uri);
    GError *error = nullptr;
    GFileInfo *info = g_file_query_info(file,
                                        G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                                        G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC ","
                                        G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                        G_FILE_QUERY_INFO_NONE, nullptr, &error);
    g_object_unref(file);
    if (!info) {
        g_clear_error(&error);
        return {};
    }

    ThumbnailCache::Version version;
    if (GDateTime *modified = g_file_info_get_modification_date_time(info)) {
        version.modifiedMSecs = g_date_time_to_unix(modified) * 1000
            + g_date_time_get_microsecond(modified) / 1000;
        g_date_time_unref(modified);
    }
    version.size = qint64(g_file_info_get_size(info));
    g_object_unref(info);
    return version.modifiedMSecs < 0 ? ThumbnailCache::Version{} : version;
}

// Whole file: QImageReader wants a seekable device so setScaledSize can
// still ask the JPEG decoder for a reduced frame. Capped so a mistaken
// multi-gigabyte "image" cannot fill memory.
QByteArray readUriBytes(const QString &uri)
{
    GFile *file = Location::make(uri);
    GError *error = nullptr;
    GFileInputStream *input = g_file_read(file, nullptr, &error);
    g_object_unref(file);
    if (!input) {
        g_clear_error(&error);
        return {};
    }

    QByteArray bytes;
    char buffer[64 * 1024];
    constexpr qint64 kMaxBytes = 512LL * 1024 * 1024;
    while (bytes.size() < kMaxBytes) {
        const gssize n = g_input_stream_read(G_INPUT_STREAM(input), buffer,
                                              sizeof buffer, nullptr, &error);
        if (n < 0) {
            g_clear_error(&error);
            bytes.clear();
            break;
        }
        if (n == 0)
            break;
        bytes.append(buffer, int(n));
    }
    g_object_unref(input);
    if (bytes.size() >= kMaxBytes)
        return {};
    return bytes;
}

} // namespace

ThumbnailCache::Version ThumbnailCache::Version::of(const QString &filePath)
{
    const QFileInfo info(filePath);
    if (info.exists())
        return { info.lastModified().toMSecsSinceEpoch(), info.size() };
    // A URI QFileInfo cannot see (smb://, sftp://, file:// before clean()).
    if (Location::isUri(filePath))
        return versionViaGio(filePath);
    return {};
}

void ThumbnailCache::store(const QString &filePath, int bucket, QImage image,
                           const Version &rendered)
{
    if (image.isNull() || rendered.size < 0)
        return;

    const QString cached = cachePathFor(filePath, bucket);
    QDir().mkpath(QFileInfo(cached).absolutePath());

    image.setText(QStringLiteral("Thumb::URI"), uriFor(filePath));
    image.setText(QStringLiteral("Thumb::MTime"),
                  QString::number(QDateTime::fromMSecsSinceEpoch(rendered.modifiedMSecs)
                                      .toSecsSinceEpoch()));
    image.setText(QStringLiteral("Thumb::Size"), QString::number(rendered.size));
    image.setText(QStringLiteral("X-Omanta::MTime-MSec"), QString::number(rendered.modifiedMSecs));
    image.setText(QStringLiteral("Software"), QStringLiteral("omanta"));

    image.save(cached, "png");
}

void ThumbnailCache::markFailed(const QString &filePath, const Version &attempted)
{
    if (attempted.size < 0)
        return;
    const QString marker = failMarkerFor(filePath);
    QDir().mkpath(QFileInfo(marker).absolutePath());

    // A 1×1 image carrying the mtime: enough to stop retrying every scroll,
    // and it stops applying once the file itself changes.
    QImage marker1x1(1, 1, QImage::Format_ARGB32);
    marker1x1.fill(Qt::transparent);
    marker1x1.setText(QStringLiteral("Thumb::URI"), uriFor(filePath));
    marker1x1.setText(QStringLiteral("Thumb::MTime"),
                      QString::number(QDateTime::fromMSecsSinceEpoch(attempted.modifiedMSecs)
                                          .toSecsSinceEpoch()));
    marker1x1.setText(QStringLiteral("Thumb::Size"), QString::number(attempted.size));
    marker1x1.save(marker, "png");
}

bool ThumbnailCache::hasFailed(const QString &filePath)
{
    const QString marker = failMarkerFor(filePath);
    if (!QFileInfo::exists(marker))
        return false;

    QImage image(marker);
    const QFileInfo info(filePath);
    const QString recorded = image.text(QStringLiteral("Thumb::MTime"));
    const QString size = image.text(QStringLiteral("Thumb::Size"));
    // A file that has changed since it failed deserves another go.
    return !recorded.isEmpty() && recorded.toLongLong() == info.lastModified().toSecsSinceEpoch()
        && (size.isEmpty() || size.toLongLong() == info.size());
}

void ThumbnailCache::ensureRegistryLoaded()
{
    QMutexLocker lock(&g_registryMutex);
    if (g_registryLoaded)
        return;
    g_registryLoaded = true;

    QStringList directories;
    for (const QString &base : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        directories << base + QStringLiteral("/thumbnailers");

    for (const QString &directory : std::as_const(directories)) {
        const QDir dir(directory);
        if (!dir.exists())
            continue;

        for (const QFileInfo &entry : dir.entryInfoList({ QStringLiteral("*.thumbnailer") },
                                                        QDir::Files)) {
            QFile file(entry.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;

            // Parsed by hand rather than with QSettings. A .thumbnailer is a
            // desktop-entry file, and QSettings' INI reader mangles them: it
            // treats ';' as a comment introducer, and ';' is exactly the
            // character separating the MimeType list. The result was an empty
            // registry and every video falling back to a generic icon.
            QString exec, tryExec, mimeLine;
            bool inEntry = false;

            while (!file.atEnd()) {
                const QString line = QString::fromUtf8(file.readLine()).trimmed();
                if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                    continue;

                if (line.startsWith(QLatin1Char('['))) {
                    inEntry = line.compare(QLatin1String("[Thumbnailer Entry]"),
                                           Qt::CaseInsensitive) == 0;
                    continue;
                }
                if (!inEntry)
                    continue;

                const int equals = line.indexOf(QLatin1Char('='));
                if (equals <= 0)
                    continue;

                const QString key = line.left(equals).trimmed();
                const QString value = line.mid(equals + 1).trimmed();

                if (key == QLatin1String("Exec"))
                    exec = value;
                else if (key == QLatin1String("TryExec"))
                    tryExec = value;
                else if (key == QLatin1String("MimeType"))
                    mimeLine = value;
            }

            if (exec.isEmpty() || mimeLine.isEmpty())
                continue;
            if (!tryExec.isEmpty() && QStandardPaths::findExecutable(tryExec).isEmpty())
                continue;

            const QStringList argv = QProcess::splitCommand(exec);
            if (argv.isEmpty() || QStandardPaths::findExecutable(argv.first()).isEmpty())
                continue;

            for (const QString &mime : mimeLine.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
                if (!g_thumbnailers.contains(mime.trimmed()))
                    g_thumbnailers.insert(mime.trimmed(), argv);
            }
        }
    }
}

bool ThumbnailCache::canHandle(const QString &mimeType)
{
    ensureRegistryLoaded();
    QMutexLocker lock(&g_registryMutex);
    return g_thumbnailers.contains(mimeType);
}

QStringList ThumbnailCache::commandFor(const QString &mimeType)
{
    ensureRegistryLoaded();
    QMutexLocker lock(&g_registryMutex);
    return g_thumbnailers.value(mimeType);
}

// ---------------------------------------------------------------------------

QString ThumbnailCache::contentTypeOf(const QString &filePath)
{
    // Extension first: most album photos are image/jpeg etc. Gio sniffing is a
    // full query_info round-trip and dominated external-disk thumbnailing.
    QMimeDatabase db;
    const QMimeType byExtension = db.mimeTypeForFile(filePath, QMimeDatabase::MatchExtension);
    if (byExtension.name().startsWith(QLatin1String("image/")))
        return byExtension.name();

    // Extensionless or unknown: sniff contents (same as the directory model).
    QString mimeType;
    GFile *file = g_file_new_for_path(filePath.toUtf8().constData());
    if (GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                            G_FILE_QUERY_INFO_NONE, nullptr, nullptr)) {
        if (const char *type = g_file_info_get_content_type(info))
            mimeType = QString::fromUtf8(type);
        g_object_unref(info);
    }
    g_object_unref(file);
    if (!mimeType.isEmpty())
        return mimeType;
    return byExtension.name();
}

QImage ThumbnailCache::render(const QString &filePath, const QString &mimeType, int size)
{
    QImage image;
    if (mimeType.startsWith(QLatin1String("image/")))
        image = renderImageFile(filePath, size);
    if (image.isNull())
        image = renderViaThumbnailer(filePath, mimeType, size);
    return image;
}

QImage ThumbnailCache::renderImageFile(const QString &filePath, int size)
{
    // Native paths go straight to the decoder. GIO URIs (a NAS opened as
    // smb://) are not files QImageReader can open, so the bytes come across
    // first and the same scaled decode runs on the buffer.
    QByteArray remoteBytes;
    QBuffer remoteDevice;
    QImageReader reader;
    if (!QFileInfo::exists(filePath) && Location::isUri(filePath)) {
        remoteBytes = readUriBytes(filePath);
        if (remoteBytes.isEmpty())
            return {};
        remoteDevice.setBuffer(&remoteBytes);
        if (!remoteDevice.open(QIODevice::ReadOnly))
            return {};
        reader.setDevice(&remoteDevice);
    } else {
        reader.setFileName(filePath);
    }
    reader.setAutoTransform(true); // honour EXIF orientation

    // Qt caps decoded image allocations at 256MB by default, which rejects
    // ordinary high-resolution scans before they can be scaled down. Generating
    // a thumbnail is precisely the case where decoding a large file is the
    // intended behaviour.
    reader.setAllocationLimit(1024);

    const QSize original = reader.size();
    if (original.isValid()) {
        // Ask the decoder for a reduced size where it can oblige. On a 60MP
        // scan that is the difference between instant and a visible stall.
        QSize target = original;
        target.scale(size, size, Qt::KeepAspectRatio);
        reader.setScaledSize(target);
    }

    QImage image = reader.read();
    if (image.isNull())
        return {};

    if (image.width() > size || image.height() > size) {
        image = image.scaled(size, size, Qt::KeepAspectRatio,
                              ThumbnailCache::scaleModeFor(image.size(), size));
    }
    return image;
}

Qt::TransformationMode ThumbnailCache::scaleModeFor(const QSize &decoded, int target)
{
    // setScaledSize already asked the decoder for roughly `target`. What remains
    // within 2× is a small cleanup and worth smoothing. A decoder that ignored
    // the hint returns the full frame, and smooth filtering that is the cost.
    if (target > 0 && decoded.width() <= target * 2 && decoded.height() <= target * 2)
        return Qt::SmoothTransformation;
    return Qt::FastTransformation;
}

QImage ThumbnailCache::renderViaThumbnailer(const QString &filePath, const QString &mimeType, int size)
{
    QStringList argv = ThumbnailCache::commandFor(mimeType);
    if (argv.isEmpty())
        return {};

    QTemporaryFile output(QDir::tempPath() + QStringLiteral("/omanta-thumb-XXXXXX.png"));
    if (!output.open())
        return {};
    output.close();

    const QString program = argv.takeFirst();
    for (QString &argument : argv) {
        argument.replace(QStringLiteral("%i"), filePath);
        argument.replace(QStringLiteral("%u"), QString::fromLatin1(QUrl::fromLocalFile(filePath).toEncoded()));
        argument.replace(QStringLiteral("%o"), output.fileName());
        argument.replace(QStringLiteral("%s"), QString::number(size));
    }

    QProcess process;
    process.start(program, argv);
    // A wedged decoder must not hold a pool thread forever.
    if (!process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};

    return QImage(output.fileName());
}

namespace {

// The response is owned by Qt Quick and can be cancel()'d / deleted while a
// pool thread is still decoding. Keep the QRunnable separate and only deliver
// through a QPointer on the GUI thread, so a cancelled response is never
// touched after destruction (the cleanOrphanedConnectionsImpl SIGSEGV).
class ThumbnailResponse : public QQuickImageResponse
{
public:
    ThumbnailResponse(QString filePath, ThumbnailCache::Version expected, int size)
        : m_filePath(std::move(filePath))
        , m_expected(expected)
        , m_bucket(ThumbnailCache::bucketFor(size))
        , m_cancelled(std::make_shared<std::atomic_bool>(false))
    {
    }

    ~ThumbnailResponse() override { m_cancelled->store(true, std::memory_order_release); }

    QQuickTextureFactory *textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    void cancel() override { m_cancelled->store(true, std::memory_order_release); }

    QString errorString() const override { return m_error; }

    void deliver(QImage image, const QString &error)
    {
        if (m_cancelled->load(std::memory_order_acquire))
            return;
        m_image = std::move(image);
        m_error = error;
        Q_EMIT finished();
    }

    const QString &filePath() const { return m_filePath; }
    const ThumbnailCache::Version &expected() const { return m_expected; }
    int bucket() const { return m_bucket; }
    std::shared_ptr<std::atomic_bool> cancelFlag() const { return m_cancelled; }

private:
    QString m_filePath;
    ThumbnailCache::Version m_expected;
    int m_bucket;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QImage m_image;
    QString m_error;
};

void deliverThumbnail(const QPointer<ThumbnailResponse> &response, QImage image,
                      const QString &error)
{
    // Post via qApp so a deleteLater'd response never receives the event.
    QMetaObject::invokeMethod(qApp, [response, image = std::move(image), error]() mutable {
        if (!response)
            return;
        response->deliver(std::move(image), error);
    }, Qt::QueuedConnection);
}

class ThumbnailJob : public QRunnable
{
public:
    explicit ThumbnailJob(ThumbnailResponse *response)
        : m_response(response)
        , m_cancelled(response->cancelFlag())
        , m_filePath(response->filePath())
        , m_expected(response->expected())
        , m_bucket(response->bucket())
    {
    }

    void run() override
    {
        if (m_cancelled->load(std::memory_order_acquire))
            return;

        const QFileInfo info(m_filePath);
        // smb:// is not a QFileInfo. The GIO read below is what can see it.
        const bool remote = Location::isUri(m_filePath) && !info.exists();
        if (!remote && (!info.exists() || !info.isFile())) {
            deliverThumbnail(m_response, {}, QStringLiteral("no such file"));
            return;
        }

        // Qt caches the answer under the URL, and the URL names one version of
        // the file. Answering it with a picture of any other version would
        // cache that picture as this version's, to be served back if the file
        // is ever at this version again. The view asks afresh once its model
        // catches up with the file.
        const auto actual = ThumbnailCache::Version::of(m_filePath);
        if (m_expected.modifiedMSecs > 0 && !(actual == m_expected)) {
            deliverThumbnail(m_response, {}, QStringLiteral("file changed"));
            return;
        }

        if (m_cancelled->load(std::memory_order_acquire))
            return;

        if (QImage cached = ThumbnailCache::loadValid(m_filePath, m_bucket); !cached.isNull()) {
            deliverThumbnail(m_response, std::move(cached), {});
            return;
        }

        if (ThumbnailCache::hasFailed(m_filePath)) {
            deliverThumbnail(m_response, {}, QStringLiteral("previously failed"));
            return;
        }

        // A file being saved over while it renders (an editor writing, a
        // copy landing) yields either the old picture or a half-written
        // decode failure. Neither may be recorded against the new version,
        // so render again until the file holds still.
        QImage generated;
        ThumbnailCache::Version version;
        const QString mimeType = ThumbnailCache::contentTypeOf(m_filePath);
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (m_cancelled->load(std::memory_order_acquire))
                return;
            version = ThumbnailCache::Version::of(m_filePath);
            generated = ThumbnailCache::render(m_filePath, mimeType, m_bucket);
            if (m_cancelled->load(std::memory_order_acquire))
                return;
            if (ThumbnailCache::Version::of(m_filePath) == version) {
                if (generated.isNull())
                    ThumbnailCache::markFailed(m_filePath, version);
                else
                    ThumbnailCache::store(m_filePath, m_bucket, generated, version);
                break;
            }
        }

        if (m_cancelled->load(std::memory_order_acquire))
            return;

        if (m_expected.modifiedMSecs > 0 && !(version == m_expected)) {
            deliverThumbnail(m_response, {}, QStringLiteral("file changed"));
            return;
        }

        if (generated.isNull()) {
            // The view falls back to the file-type icon when a response errors, so
            // failing is a normal outcome here, not an exceptional one.
            deliverThumbnail(m_response, {}, QStringLiteral("could not render"));
            return;
        }

        deliverThumbnail(m_response, std::move(generated), {});
    }

private:
    QPointer<ThumbnailResponse> m_response;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QString m_filePath;
    ThumbnailCache::Version m_expected;
    int m_bucket;
};

} // namespace

QThreadPool *ThumbnailProvider::threadPool()
{
    static QThreadPool pool;
    static std::once_flag once;
    std::call_once(once, [] {
        // Cap well below idealThreadCount: 16 parallel JPEG decodes thrash a
        // spinning disk and fight the photo pool for IO.
        pool.setMaxThreadCount(qMax(2, qMin(6, QThread::idealThreadCount())));
        pool.setExpiryTimeout(30000);
    });
    return &pool;
}

QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id,
                                                             const QSize &requestedSize)
{
    const int size = requestedSize.width() > 0 ? requestedSize.width() : 128;
    ThumbnailCache::Version expected;
    const QString path = Thumbnails::pathFromId(id, &expected);
    auto *response = new ThumbnailResponse(path, expected, size);
    threadPool()->start(new ThumbnailJob(response));
    return response;
}

// ---------------------------------------------------------------------------

int PhotoThumbnailCache::bucketFor(int requestedSize)
{
    if (requestedSize <= 256)
        return 256;
    if (requestedSize <= 512)
        return 512;
    return 1024;
}

QString PhotoThumbnailCache::cacheRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
           + QStringLiteral("/omanta/photo-thumbnails");
}

QString PhotoThumbnailCache::cachePathFor(const QString &filePath, int bucket,
                                          const ThumbnailCache::Version &version)
{
    const QString extension = QString::fromLatin1(photoFormat());
    return QStringLiteral("%1/%2/%3.%4")
        .arg(cacheRoot(), QString::number(bucket), photoKey(filePath, bucket, version), extension);
}

QImage PhotoThumbnailCache::load(const QString &filePath, int bucket,
                                 const ThumbnailCache::Version &version)
{
    // Version comes from the image URL. Trust it for the RAM key; only re-stat
    // when reading disk so a stale on-disk file is not served after an edit.
    const QString key = photoKey(filePath, bucket, version);
    {
        QMutexLocker lock(&g_photoCacheMutex);
        if (QImage *cached = g_photoMemoryCache.object(key))
            return *cached;
    }

    if (!(ThumbnailCache::Version::of(filePath) == version))
        return {};

    QImage image(cachePathFor(filePath, bucket, version));
    if (image.isNull())
        return {};

    QMutexLocker lock(&g_photoCacheMutex);
    const int cost = int(qMin<qint64>(image.sizeInBytes(), std::numeric_limits<int>::max()));
    g_photoMemoryCache.insert(key, new QImage(image), cost);
    return image;
}

void PhotoThumbnailCache::storeMemory(const QString &filePath, int bucket,
                                      const ThumbnailCache::Version &version,
                                      const QImage &image)
{
    if (image.isNull())
        return;
    const QString key = photoKey(filePath, bucket, version);
    QMutexLocker lock(&g_photoCacheMutex);
    const int cost = int(qMin<qint64>(image.sizeInBytes(), std::numeric_limits<int>::max()));
    g_photoMemoryCache.insert(key, new QImage(image), cost);
}

void PhotoThumbnailCache::storeDisk(const QString &filePath, int bucket,
                                    const ThumbnailCache::Version &version,
                                    const QImage &image)
{
    if (image.isNull() || !(ThumbnailCache::Version::of(filePath) == version))
        return;

    const QString path = cachePathFor(filePath, bucket, version);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile output(path);
    if (output.open(QIODevice::WriteOnly)) {
        QImageWriter writer(&output, photoFormat());
        if (photoFormat() == QByteArrayLiteral("webp"))
            writer.setQuality(82);
        if (writer.write(image))
            output.commit();
        else
            output.cancelWriting();
    }
}

void PhotoThumbnailCache::store(const QString &filePath, int bucket,
                                const ThumbnailCache::Version &version, const QImage &image)
{
    storeMemory(filePath, bucket, version, image);
    storeDisk(filePath, bucket, version, image);
}

void PhotoThumbnailCache::setMemoryLimit(qint64 bytes)
{
    QMutexLocker lock(&g_photoCacheMutex);
    g_photoMemoryLimit = qMax<qint64>(0, qMin<qint64>(bytes, std::numeric_limits<int>::max()));
    g_photoMemoryCache.setMaxCost(int(g_photoMemoryLimit));
}

qint64 PhotoThumbnailCache::memoryLimit()
{
    QMutexLocker lock(&g_photoCacheMutex);
    return g_photoMemoryLimit;
}

qint64 PhotoThumbnailCache::memoryCost()
{
    QMutexLocker lock(&g_photoCacheMutex);
    return g_photoMemoryCache.totalCost();
}

int PhotoThumbnailCache::memoryCount()
{
    QMutexLocker lock(&g_photoCacheMutex);
    return g_photoMemoryCache.size();
}

void PhotoThumbnailCache::clearMemory()
{
    QMutexLocker lock(&g_photoCacheMutex);
    g_photoMemoryCache.clear();
}

void PhotoThumbnailCache::clearAll()
{
    clearMemory();
    const QString root = cacheRoot();
    if (!root.isEmpty())
        QDir(root).removeRecursively();
}

namespace {

class PhotoThumbnailResponse : public QQuickImageResponse
{
public:
    PhotoThumbnailResponse(QString filePath, ThumbnailCache::Version version, int size)
        : m_filePath(std::move(filePath))
        , m_version(version)
        , m_bucket(PhotoThumbnailCache::bucketFor(size))
        , m_cancelled(std::make_shared<std::atomic_bool>(false))
    {
    }

    ~PhotoThumbnailResponse() override { m_cancelled->store(true, std::memory_order_release); }

    QQuickTextureFactory *textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    void cancel() override { m_cancelled->store(true, std::memory_order_release); }

    QString errorString() const override { return m_error; }

    void deliver(QImage image, const QString &error)
    {
        if (m_cancelled->load(std::memory_order_acquire))
            return;
        m_image = std::move(image);
        m_error = error;
        Q_EMIT finished();
    }

    const QString &filePath() const { return m_filePath; }
    const ThumbnailCache::Version &version() const { return m_version; }
    int bucket() const { return m_bucket; }
    std::shared_ptr<std::atomic_bool> cancelFlag() const { return m_cancelled; }

private:
    QString m_filePath;
    ThumbnailCache::Version m_version;
    int m_bucket;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QImage m_image;
    QString m_error;
};

void deliverPhotoThumbnail(const QPointer<PhotoThumbnailResponse> &response, QImage image,
                           const QString &error)
{
    QMetaObject::invokeMethod(qApp, [response, image = std::move(image), error]() mutable {
        if (!response)
            return;
        response->deliver(std::move(image), error);
    }, Qt::QueuedConnection);
}

class PhotoThumbnailJob : public QRunnable
{
public:
    PhotoThumbnailJob(PhotoThumbnailResponse *response, PhotoThumbnailProvider *provider)
        : m_response(response)
        , m_provider(provider)
        , m_cancelled(response->cancelFlag())
        , m_filePath(response->filePath())
        , m_version(response->version())
        , m_bucket(response->bucket())
    {
    }

    void run() override
    {
        m_provider->noteDecodeStarted(m_filePath, this);
        struct Finish {
            PhotoThumbnailProvider *provider;
            QString path;
            ~Finish() { provider->noteDecodeFinished(path); }
        } finish{m_provider, m_filePath};

        if (m_cancelled->load(std::memory_order_acquire))
            return;

        QImage cached = PhotoThumbnailCache::load(m_filePath, m_bucket, m_version);
        if (!cached.isNull()) {
            deliverPhotoThumbnail(m_response, std::move(cached), {});
            return;
        }

        if (m_cancelled->load(std::memory_order_acquire))
            return;

        QImage image = ThumbnailCache::renderImageFile(m_filePath, m_bucket);
        if (m_cancelled->load(std::memory_order_acquire))
            return;
        if (image.isNull()) {
            deliverPhotoThumbnail(m_response, {}, QStringLiteral("could not render"));
            return;
        }
        if (!(ThumbnailCache::Version::of(m_filePath) == m_version)) {
            deliverPhotoThumbnail(m_response, {}, QStringLiteral("file changed"));
            return;
        }

        if (auto *reached = OmantaThumbnailTest::g_reachedBeforePhotoStore)
            reached->store(true, std::memory_order_release);
        if (auto *pause = OmantaThumbnailTest::g_pauseBeforePhotoStore) {
            while (!pause->load(std::memory_order_acquire))
                QThread::msleep(1);
        }

        // RAM + a queued GUI delivery first, so scrolling sees pixels before
        // WebP hits the disk. The disk write still happens if the cell scrolled
        // away: the decode is already paid for.
        PhotoThumbnailCache::storeMemory(m_filePath, m_bucket, m_version, image);
        if (!m_cancelled->load(std::memory_order_acquire))
            deliverPhotoThumbnail(m_response, image, {});
        PhotoThumbnailCache::storeDisk(m_filePath, m_bucket, m_version, image);
    }

private:
    QPointer<PhotoThumbnailResponse> m_response;
    PhotoThumbnailProvider *m_provider;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QString m_filePath;
    ThumbnailCache::Version m_version;
    int m_bucket;
};

} // namespace

PhotoThumbnailProvider::PhotoThumbnailProvider()
{
    g_photoProvider = this;
    PhotoThumbnailCache::setMemoryLimit(PhotoThumbnailCache::DefaultMemoryLimit);
    m_pool.setExpiryTimeout(30000);
    {
        QMutexLocker lock(&g_ioClientsMutex);
        m_ioBound = anyPhotoIoClientSlow();
    }
    applyPoolLimits();
}

PhotoThumbnailProvider::~PhotoThumbnailProvider()
{
    if (g_photoProvider == this)
        g_photoProvider = nullptr;
    {
        QMutexLocker lock(&m_queueMutex);
        m_queued.clear();
    }
    m_pool.clear();
    m_pool.waitForDone();
    PhotoThumbnailCache::clearMemory();
}

void PhotoThumbnailProvider::setIoBound(bool bound)
{
    if (m_ioBound == bound)
        return;
    m_ioBound = bound;
    applyPoolLimits();
}

void PhotoThumbnailProvider::applyPoolLimits()
{
    const int ideal = QThread::idealThreadCount();
    m_pool.setMaxThreadCount(m_ioBound ? 2 : qMax(2, qMin(4, ideal)));
}

void PhotoThumbnailProvider::noteDecodeStarted(const QString &filePath, QRunnable *job)
{
    QMutexLocker lock(&m_queueMutex);
    auto queued = m_queued.find(filePath);
    if (queued != m_queued.end()) {
        QList<QueuedJob> &jobs = queued.value();
        for (int i = 0; i < jobs.size(); ++i) {
            if (jobs.at(i).job == job) {
                jobs.removeAt(i);
                break;
            }
        }
        if (jobs.isEmpty())
            m_queued.erase(queued);
    }
    m_decoding[filePath] += 1;
}

void PhotoThumbnailProvider::noteDecodeFinished(const QString &filePath)
{
    QMutexLocker lock(&m_queueMutex);
    auto decoding = m_decoding.find(filePath);
    if (decoding == m_decoding.end())
        return;
    if (decoding.value() <= 1)
        m_decoding.erase(decoding);
    else
        --decoding.value();
}

void PhotoThumbnailProvider::reprioritize(const QString &filePath, int priority)
{
    QMutexLocker lock(&m_queueMutex);
    auto queued = m_queued.find(filePath);
    if (queued == m_queued.end())
        return;
    for (QueuedJob &entry : queued.value()) {
        if (entry.priority == priority)
            continue;
        // Already running: tryTake fails and the in-flight decode is left alone.
        if (!m_pool.tryTake(entry.job))
            continue;
        entry.priority = priority;
        m_pool.start(entry.job, priority);
    }
}

int PhotoThumbnailProvider::queuedPriority(const QString &filePath) const
{
    QMutexLocker lock(&m_queueMutex);
    const auto queued = m_queued.constFind(filePath);
    if (queued == m_queued.cend() || queued.value().isEmpty())
        return -1;
    return queued.value().constFirst().priority;
}

bool PhotoThumbnailProvider::isActive(const QString &filePath) const
{
    QMutexLocker lock(&m_queueMutex);
    return m_queued.contains(filePath) || m_decoding.contains(filePath);
}

QQuickImageResponse *PhotoThumbnailProvider::requestImageResponse(const QString &id,
                                                                  const QSize &requestedSize)
{
    // id: "<bucket>/<mtime>-<size>/<path>" — priority is out-of-band.
    const QStringList parts = id.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    if (parts.size() < 3)
        return new PhotoThumbnailResponse({}, {}, 256);

    const int encodedSize = parts.at(0).toInt();
    const QStringList version = parts.at(1).split(QLatin1Char('-'));
    ThumbnailCache::Version expected;
    if (version.size() == 2)
        expected = { version.at(0).toLongLong(), version.at(1).toLongLong() };
    const QString path = QUrl::fromPercentEncoding(parts.mid(2).join(QLatin1Char('/')).toUtf8());
    const int size = requestedSize.width() > 0 ? requestedSize.width() : encodedSize;
    const int priority = Thumbnails::photoPriorityFor(path);
    auto *response = new PhotoThumbnailResponse(path, expected, size);
    auto *job = new PhotoThumbnailJob(response, this);
    {
        QMutexLocker lock(&m_queueMutex);
        m_queued[path].append({job, priority});
    }
    m_pool.start(job, priority);
    return response;
}

// ---------------------------------------------------------------------------

Thumbnails::Thumbnails(QObject *parent)
    : QObject(parent)
{
}

void Thumbnails::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    Q_EMIT enabledChanged();
}

void Thumbnails::setMaximumFileSize(qint64 bytes)
{
    if (m_maximumFileSize == bytes)
        return;
    m_maximumFileSize = bytes;
    Q_EMIT maximumFileSizeChanged();
}

bool Thumbnails::canThumbnail(const QString &mimeType, qint64 fileSize) const
{
    if (!m_enabled || mimeType.isEmpty())
        return false;

    // Images are decoded in-process; everything else needs a registered
    // thumbnailer, and asking about a type nothing handles just costs a
    // process launch that will fail.
    //
    // The size cap guards ONLY the in-process image decode — Nautilus's
    // rule. An external thumbnailer (video, PDF) reads a few frames, not
    // the whole file, so a 4GB screen recording still gets its preview.
    if (mimeType.startsWith(QLatin1String("image/")))
        return m_maximumFileSize <= 0 || fileSize <= m_maximumFileSize;

    return ThumbnailCache::canHandle(mimeType);
}

QString Thumbnails::source(const QString &filePath, const QDateTime &modified,
                           qint64 fileSize) const
{
    // "<version>/<encoded path>": the version is opaque to the provider,
    // which only needs the path back.
    const qint64 msecs = modified.isValid() ? modified.toMSecsSinceEpoch() : 0;
    return QStringLiteral("image://thumbnail/%1-%2/%3")
        .arg(msecs)
        .arg(fileSize)
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(filePath, "/")));
}

QString Thumbnails::photoSource(const QString &filePath, const QDateTime &modified,
                                qint64 fileSize, int requestedSize, int priority) const
{
    notePhotoPriority(filePath, priority);
    const qint64 msecs = modified.isValid() ? modified.toMSecsSinceEpoch() : 0;
    // Priority must NOT appear in the URL: viewport priority changes would
    // otherwise cancel and restart every in-flight decode on scroll.
    return QStringLiteral("image://photo/%1/%2-%3/%4")
        .arg(PhotoThumbnailCache::bucketFor(requestedSize))
        .arg(msecs)
        .arg(fileSize)
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(filePath, "/")));
}

void Thumbnails::notePhotoPriority(const QString &filePath, int priority) const
{
    if (filePath.isEmpty())
        return;
    if (g_photoProvider)
        g_photoProvider->reprioritize(filePath, priority);

    QMutexLocker lock(&g_photoPriorityMutex);
    g_photoPriority.insert(filePath, priority);
    // Viewport priority is recorded for every cell that scrolls by. Drop paths
    // that are neither queued nor decoding so a long session does not keep them.
    if (g_photoPriority.size() <= 2048 || !g_photoProvider)
        return;
    QStringList drop;
    for (auto it = g_photoPriority.cbegin(); it != g_photoPriority.cend(); ++it) {
        if (it.key() == filePath || g_photoProvider->isActive(it.key()))
            continue;
        drop.append(it.key());
    }
    for (const QString &key : drop)
        g_photoPriority.remove(key);
}

int Thumbnails::photoPriorityFor(const QString &filePath)
{
    QMutexLocker lock(&g_photoPriorityMutex);
    return g_photoPriority.value(filePath, 50);
}

QString Thumbnails::originalSource(const QString &filePath) const
{
    return QUrl::fromLocalFile(filePath).toString();
}

namespace {

bool isNetworkFileSystem(const QByteArray &fs)
{
    return fs == "cifs" || fs == "smb2" || fs == "smbfs" || fs == "nfs" || fs == "nfs4"
        || fs == "afs" || fs == "9p" || fs.startsWith("fuse.sshfs")
        || fs.startsWith("fuse.rclone") || fs.startsWith("fuse.gvfs")
        || fs.startsWith("fuse.davfs") || fs.startsWith("fuse.http");
}

// 1 rotational, 0 not, -1 if this sysfs node has no queue flag.
int rotationalFlag(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    return file.peek(1) == "1" ? 1 : 0;
}

bool isRotational(const QStorageInfo &storage)
{
    const QByteArray device = storage.device();
    if (!device.startsWith('/'))
        return false;

    struct stat info;
    if (::stat(device.constData(), &info) != 0 || !S_ISBLK(info.st_mode))
        return false;

    const QString node = QStringLiteral("/sys/dev/block/%1:%2")
                             .arg(gnu_dev_major(info.st_rdev))
                             .arg(gnu_dev_minor(info.st_rdev));
    const int own = rotationalFlag(node + QStringLiteral("/queue/rotational"));
    if (own >= 0)
        return own == 1;

    // Partitions publish the flag on the parent disk, not on themselves.
    const QString resolved = QFileInfo(node).canonicalFilePath();
    if (resolved.isEmpty())
        return false;
    return rotationalFlag(QDir::cleanPath(resolved + QStringLiteral("/../queue/rotational"))) == 1;
}

} // namespace

bool Thumbnails::isSlowStorage(const QString &location) const
{
    if (location.isEmpty())
        return false;
    if (!Location::isLocal(location))
        return true; // smb://, sftp://, …

    const QString path = Location::clean(location);
    if (path.contains(QLatin1String("/gvfs/")))
        return true;

    const QFileInfo info(path);
    if (info.exists()) {
        const QStorageInfo storage(info.isDir() ? info.absoluteFilePath()
                                                 : info.absolutePath());
        if (isNetworkFileSystem(storage.fileSystemType()) || isRotational(storage))
            return true;
        // Mounted SSD, including one plugged in under /run/media or /mnt.
        return false;
    }

    // Nothing to stat yet: removable mountpoints are the usual slow case.
    // /mnt is not — it is a normal place for an internal volume.
    return path.startsWith(QLatin1String("/run/media/"))
        || path.startsWith(QLatin1String("/media/"));
}

void Thumbnails::bindPhotoIo(QObject *view, bool slow) const
{
    if (!view)
        return;

    {
        QMutexLocker lock(&g_ioClientsMutex);
        const bool first = !g_ioClients.contains(view);
        g_ioClients.insert(view, slow);
        if (first) {
            QObject::connect(view, &QObject::destroyed, [](QObject *dead) {
                {
                    QMutexLocker lock(&g_ioClientsMutex);
                    g_ioClients.remove(dead);
                }
                applyPhotoIoBound();
            });
        }
    }
    applyPhotoIoBound();
}

void Thumbnails::clearPhotoCache() const
{
    PhotoThumbnailCache::clearAll();
}

QString Thumbnails::pathFromId(const QString &id, ThumbnailCache::Version *version)
{
    const int slash = id.indexOf(QLatin1Char('/'));
    if (slash < 0)
        return {};
    if (version) {
        const QStringList parts = id.left(slash).split(QLatin1Char('-'));
        if (parts.size() == 2)
            *version = { parts.at(0).toLongLong(), parts.at(1).toLongLong() };
    }
    // Qt hands the id over partly decoded, but never a literal '%' (it stays
    // %25), so decoding once more always recovers the original bytes.
    return QUrl::fromPercentEncoding(id.mid(slash + 1).toUtf8());
}
