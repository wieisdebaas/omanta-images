#pragma once

#include <QDateTime>
#include <QCache>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QQuickAsyncImageProvider>
#include <QStringList>
#include <QThreadPool>
#include <QtQmlIntegration>

// Thumbnails, implemented against the freedesktop thumbnail spec rather than a
// GNOME library.
//
// The payoff for following the spec is a *shared* cache: thumbnails other
// applications already generated show up instantly here, and the ones generated
// here show up in them. On this system that means ffmpegthumbnailer, evince and
// glycin do the work for video, PDF and HEIF/JXL/SVG, and omanta only has to
// find and cache the results.
//
// Generation runs on a thread pool. The icon provider deliberately does not —
// icons are cheap and immediate — but a thumbnail can mean spawning a process
// to decode a video, and doing that on the GUI thread would stutter the window
// on every scroll.
class ThumbnailCache
{
public:
    // The spec's size buckets. Requests are rounded up to one of these so the
    // cache is shared rather than fragmented per-widget-size.
    static int bucketFor(int requestedSize);
    static QString bucketName(int bucket);

    // Where the spec says this file's thumbnail lives, whoever made it.
    static QString cachePathFor(const QString &filePath, int bucket);
    static QString failMarkerFor(const QString &filePath);

    // Which version of a file a thumbnail was made from.
    struct Version
    {
        qint64 modifiedMSecs = -1;
        qint64 size = -1;
        static Version of(const QString &filePath);
        bool operator==(const Version &) const = default;
    };

    // A cached thumbnail is only valid while the source hasn't changed since.
    static QImage loadValid(const QString &filePath, int bucket);
    // Stamped with the version that was rendered, which must be read BEFORE
    // rendering: stamping afterwards would label a picture of the old
    // contents as the new ones if the file is saved over mid-render.
    static void store(const QString &filePath, int bucket, QImage image, const Version &rendered);
    static void store(const QString &filePath, int bucket, QImage image)
    { store(filePath, bucket, std::move(image), Version::of(filePath)); }
    static void markFailed(const QString &filePath, const Version &attempted);
    static void markFailed(const QString &filePath) { markFailed(filePath, Version::of(filePath)); }
    static bool hasFailed(const QString &filePath);

    // Registered .thumbnailer handlers, keyed by MIME type. Parsed once.
    static bool canHandle(const QString &mimeType);
    static QStringList commandFor(const QString &mimeType);

    // Exposed so the tests can drive rendering directly rather than through a
    // QML image provider.
    // Content type by sniffing, not by extension — a valid image saved
    // without one must still get a preview.
    static QString contentTypeOf(const QString &filePath);

    static QImage render(const QString &filePath, const QString &mimeType, int size);
    static QImage renderImageFile(const QString &filePath, int size);
    static QImage renderViaThumbnailer(const QString &filePath, const QString &mimeType, int size);
    // Smooth when the decoder already came close; fast when it returned the full frame.
    static Qt::TransformationMode scaleModeFor(const QSize &decoded, int target);

private:
    static void ensureRegistryLoaded();
};

class ThumbnailProvider : public QQuickAsyncImageProvider
{
public:
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;
    // Dedicated pool (not the process-wide global) so icon thumbs cannot
    // saturate every core while photo decode is also running.
    static QThreadPool *threadPool();
};

class PhotoThumbnailCache
{
public:
    static constexpr qint64 DefaultMemoryLimit = 256LL * 1024 * 1024;

    static int bucketFor(int requestedSize);
    static QString cacheRoot();
    static QString cachePathFor(const QString &filePath, int bucket,
                                const ThumbnailCache::Version &version);
    static QImage load(const QString &filePath, int bucket,
                       const ThumbnailCache::Version &version);
    // RAM first (cheap, scroll-back), then disk. Callers that already
    // delivered the pixels to the UI can write disk alone.
    static void store(const QString &filePath, int bucket,
                      const ThumbnailCache::Version &version, const QImage &image);
    static void storeMemory(const QString &filePath, int bucket,
                            const ThumbnailCache::Version &version, const QImage &image);
    static void storeDisk(const QString &filePath, int bucket,
                          const ThumbnailCache::Version &version, const QImage &image);

    static void setMemoryLimit(qint64 bytes);
    static qint64 memoryLimit();
    static qint64 memoryCost();
    static int memoryCount();
    static void clearMemory();
    // Wipe ~/.cache/omanta/photo-thumbnails and the in-memory LRU.
    static void clearAll();
};

class PhotoThumbnailProvider : public QQuickAsyncImageProvider
{
public:
    PhotoThumbnailProvider();
    ~PhotoThumbnailProvider() override;

    QQuickImageResponse *requestImageResponse(const QString &id,
                                              const QSize &requestedSize) override;

    // Bound IO on spinning disks / network filesystems: fewer concurrent reads.
    void setIoBound(bool bound);
    bool ioBound() const { return m_ioBound; }
    QThreadPool *threadPool() { return &m_pool; }

    // Move a not-yet-started decode to `priority` without cancelling it.
    // -1 when nothing for this path is still waiting in the pool.
    void reprioritize(const QString &filePath, int priority);
    int queuedPriority(const QString &filePath) const;
    // Queued or currently decoding — the priority map may keep these paths.
    bool isActive(const QString &filePath) const;

    void noteDecodeStarted(const QString &filePath, QRunnable *job);
    void noteDecodeFinished(const QString &filePath);

private:
    struct QueuedJob {
        QRunnable *job = nullptr;
        int priority = 0;
    };

    void applyPoolLimits();

    QThreadPool m_pool;
    mutable QMutex m_queueMutex;
    QHash<QString, QList<QueuedJob>> m_queued;
    QHash<QString, int> m_decoding;
    bool m_ioBound = false;
};

// What QML needs to decide whether to even ask for a thumbnail.
class Thumbnails : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(qint64 maximumFileSize READ maximumFileSize WRITE setMaximumFileSize
                   NOTIFY maximumFileSizeChanged)

public:
    explicit Thumbnails(QObject *parent = nullptr);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);

    qint64 maximumFileSize() const { return m_maximumFileSize; }
    void setMaximumFileSize(qint64 bytes);

    // True when this file is worth asking about at all: the type is supported
    // and the file is not so large that decoding it would cost more than the
    // preview is worth.
    Q_INVOKABLE bool canThumbnail(const QString &mimeType, qint64 fileSize) const;

    // The image:// URL for a file's thumbnail. It carries the file's
    // modification time and size, so an edited file gets a new URL: Qt's
    // pixmap cache is keyed on the URL and would otherwise keep serving the
    // old picture, even across a reload. The path is percent-encoded so
    // names containing '#', '?' or '%' are not read as URL syntax.
    Q_INVOKABLE QString source(const QString &filePath, const QDateTime &modified,
                               qint64 fileSize) const;
    // Priority is recorded out-of-band so changing viewport priority does not
    // change the URL (and cancel/restart an in-flight decode).
    Q_INVOKABLE QString photoSource(const QString &filePath, const QDateTime &modified,
                                    qint64 fileSize, int requestedSize,
                                    int priority = 0) const;
    Q_INVOKABLE void notePhotoPriority(const QString &filePath, int priority) const;
    Q_INVOKABLE QString originalSource(const QString &filePath) const;
    // Network URIs, gvfs, spinning disks, and not-yet-mounted /media paths.
    // A mounted SSD (including under /run/media) stays fast.
    Q_INVOKABLE bool isSlowStorage(const QString &location) const;
    // Register one photo grid. The shared pool is IO-bound while any live
    // client says so; destroying `view` drops its claim.
    Q_INVOKABLE void bindPhotoIo(QObject *view, bool slow) const;
    // Preferences: wipe the Ctrl+3 photo thumbnail disk + RAM cache.
    Q_INVOKABLE void clearPhotoCache() const;
    // The inverse, as the provider sees it: the file path an id names, and
    // optionally the version of it the id asks for (a zero mtime means
    // "whatever is there": remote rows may not report one).
    static QString pathFromId(const QString &id, ThumbnailCache::Version *version = nullptr);
    static int photoPriorityFor(const QString &filePath);

Q_SIGNALS:
    void enabledChanged();
    void maximumFileSizeChanged();

private:
    bool m_enabled = true;
    // Nautilus defaults to 50MB; the same number keeps behaviour familiar.
    qint64 m_maximumFileSize = 50LL * 1024 * 1024;
};
