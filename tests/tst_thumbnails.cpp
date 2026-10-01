#include "TestFixture.h"
#include "ThumbnailProvider.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRunnable>
#include <QSemaphore>
#include <QStandardPaths>
#include <QQuickImageResponse>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QThreadPool>
#include <QUrl>

#include <atomic>
#include <memory>

namespace OmantaThumbnailTest {
extern std::atomic<bool> *g_pauseBeforePhotoStore;
extern std::atomic<bool> *g_reachedBeforePhotoStore;
}

// Thumbnails, tested against the freedesktop spec rather than against my
// assumptions about it. Two of these exist because the first working build got
// them wrong: every video fell back to a generic icon, and a perfectly ordinary
// high-resolution PNG refused to render.
class TestThumbnails : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void bucketsRoundUpToSpecSizes();
    void cachePathFollowsTheSpec();

    void registryFindsVideoThumbnailers();
    void rendersASmallImage();
    void rendersAHighResolutionImage();
    void honoursExifOrientation();

    void storesAndReusesACachedThumbnail();
    void invalidatesTheCacheWhenTheFileChanges();
    void invalidatesTheCacheWithinTheSameSecond();
    void sourceRoundTripsAwkwardNames();
    void providerAnswersOnlyTheVersionAsked();
    void remembersFailuresButNotForever();

    void detectsTypeByContentNotExtension();
    void canThumbnailRespectsTypeAndSize();
    void photoCachePersistsAcrossMemoryClears();
    void photoCacheInvalidatesWhenSourceChanges();
    void photoMemoryCacheIsBoundedAndClearable();
    void photoProviderReleasesMemoryOnDestruction();
    void providerSurvivesResponseDestructionWhileRunning();
    void photoProviderSurvivesResponseDestructionWhileRunning();
    void photoSourceUrlIgnoresPriority();
    void slowStorageDetectsRemovableAndRemote();
    void scaleModeSmoothsAResidualAndIsFastForAFullFrame();
    void queuedPhotoPriorityFollowsTheViewport();
    void cancelledDecodeStillWritesTheDiskCache();
    void photoIoBoundTracksLiveViews();

private:
    static QString writeImage(const TempTree &tree, const QString &name, int w, int h)
    {
        QImage image(w, h, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QPainter painter(&image);
        painter.fillRect(0, 0, w / 2, h, Qt::magenta);
        painter.end();
        const QString path = tree.filePath(name);
        // Format given explicitly: QImage::save() infers it from the suffix,
        // and this helper is deliberately used to write files without one.
        image.save(path, "png");
        return path;
    }
};

void TestThumbnails::initTestCase()
{
    // Never scribble on the real thumbnail cache while testing.
    QStandardPaths::setTestModeEnabled(true);
}

void TestThumbnails::bucketsRoundUpToSpecSizes()
{
    QCOMPARE(ThumbnailCache::bucketFor(16), 128);
    QCOMPARE(ThumbnailCache::bucketFor(128), 128);
    QCOMPARE(ThumbnailCache::bucketFor(129), 256);
    QCOMPARE(ThumbnailCache::bucketFor(512), 512);
    QCOMPARE(ThumbnailCache::bucketFor(4096), 1024);

    // The directory names are part of the shared-cache contract.
    QCOMPARE(ThumbnailCache::bucketName(128), QStringLiteral("normal"));
    QCOMPARE(ThumbnailCache::bucketName(256), QStringLiteral("large"));
    QCOMPARE(ThumbnailCache::bucketName(512), QStringLiteral("x-large"));
    QCOMPARE(ThumbnailCache::bucketName(1024), QStringLiteral("xx-large"));
}

void TestThumbnails::cachePathFollowsTheSpec()
{
    // The whole point of the spec is a shared cache: the filename must be the
    // MD5 of the file's URI, or thumbnails other applications generated will
    // never be found and ours will never be reused.
    const QString path = QStringLiteral("/home/someone/Pictures/a photo.jpg");
    const QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
    const QString expected =
        QString::fromLatin1(QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex())
        + QStringLiteral(".png");

    const QString cachePath = ThumbnailCache::cachePathFor(path, 256);
    QVERIFY2(cachePath.endsWith(expected), qPrintable(cachePath));
    QVERIFY2(cachePath.contains(QStringLiteral("/thumbnails/large/")), qPrintable(cachePath));
}

void TestThumbnails::registryFindsVideoThumbnailers()
{
    // The first implementation parsed .thumbnailer files with QSettings, whose
    // INI reader treats ';' as a comment introducer — the exact character
    // separating the MimeType list. The registry came back empty and every
    // video silently fell back to a generic icon.
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpegthumbnailer")).isEmpty())
        QSKIP("ffmpegthumbnailer is not installed");

    QVERIFY2(ThumbnailCache::canHandle(QStringLiteral("video/quicktime")),
             "video/quicktime must resolve to a thumbnailer");
    QVERIFY(ThumbnailCache::canHandle(QStringLiteral("video/mp4")));

    const QStringList command = ThumbnailCache::commandFor(QStringLiteral("video/quicktime"));
    QVERIFY(!command.isEmpty());
    QVERIFY(command.first().contains(QStringLiteral("ffmpegthumbnailer")));
    // The placeholders must survive parsing or substitution has nothing to do.
    QVERIFY(command.contains(QStringLiteral("%i")));
    QVERIFY(command.contains(QStringLiteral("%o")));
}

void TestThumbnails::rendersASmallImage()
{
    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("small.png"), 400, 300);

    const QImage thumb = ThumbnailCache::render(path, QStringLiteral("image/png"), 256);
    QVERIFY(!thumb.isNull());
    QVERIFY(thumb.width() <= 256 && thumb.height() <= 256);
    // Aspect ratio must survive, or previews look squashed.
    QVERIFY(qAbs(qreal(thumb.width()) / thumb.height() - 4.0 / 3.0) < 0.05);
}

void TestThumbnails::rendersAHighResolutionImage()
{
    // Qt refuses image allocations over 256MB by default, which rejected an
    // ordinary 3160×2272 scan before it could be scaled down. Thumbnailing is
    // exactly the case where decoding a large file is the point.
    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("scan.png"), 5000, 4000);

    const QImage thumb = ThumbnailCache::render(path, QStringLiteral("image/png"), 256);
    QVERIFY2(!thumb.isNull(), "a high-resolution image must still produce a thumbnail");
    QVERIFY(thumb.width() <= 256 && thumb.height() <= 256);
}

void TestThumbnails::honoursExifOrientation()
{
    // No EXIF here, but the reader must at least not mangle a plain image —
    // the setting that enables rotation is easy to lose in a refactor.
    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("wide.png"), 600, 200);

    const QImage thumb = ThumbnailCache::renderImageFile(path, 128);
    QVERIFY(!thumb.isNull());
    QVERIFY2(thumb.width() > thumb.height(), "a landscape image must stay landscape");
}

void TestThumbnails::storesAndReusesACachedThumbnail()
{
    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("cached.png"), 300, 300);

    const QImage generated = ThumbnailCache::render(path, QStringLiteral("image/png"), 256);
    QVERIFY(!generated.isNull());
    ThumbnailCache::store(path, 256, generated);

    QVERIFY(QFile::exists(ThumbnailCache::cachePathFor(path, 256)));

    const QImage reloaded = ThumbnailCache::loadValid(path, 256);
    QVERIFY2(!reloaded.isNull(), "a freshly stored thumbnail must load back");
    QCOMPARE(reloaded.size(), generated.size());
}

void TestThumbnails::invalidatesTheCacheWhenTheFileChanges()
{
    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("edited.png"), 300, 300);
    ThumbnailCache::store(path, 256, ThumbnailCache::render(path, QStringLiteral("image/png"), 256));
    QVERIFY(!ThumbnailCache::loadValid(path, 256).isNull());

    // Edit the file: the stale preview must stop being served.
    QTest::qWait(1100); // mtime has one-second resolution
    writeImage(tree, QStringLiteral("edited.png"), 300, 300);
    tree.setModified(QStringLiteral("edited.png"), QDateTime::currentDateTime().addSecs(5));

    QVERIFY2(ThumbnailCache::loadValid(path, 256).isNull(),
             "an edited file must not keep showing its old thumbnail");
}

void TestThumbnails::invalidatesTheCacheWithinTheSameSecond()
{
    // Thumb::MTime is whole seconds: a picture re-saved inside the second it
    // was written, then pinned back to that second, still has to invalidate.
    TempTree tree;
    const QDateTime when = QDateTime::currentDateTime().addSecs(-30);
    const QString path = writeImage(tree, QStringLiteral("resaved.png"), 300, 100);
    tree.setModified(QStringLiteral("resaved.png"), when);
    ThumbnailCache::store(path, 256, ThumbnailCache::render(path, QStringLiteral("image/png"), 256));
    QVERIFY(!ThumbnailCache::loadValid(path, 256).isNull());

    writeImage(tree, QStringLiteral("resaved.png"), 100, 300);
    tree.setModified(QStringLiteral("resaved.png"), when);
    QVERIFY2(ThumbnailCache::loadValid(path, 256).isNull(),
             "a same-second re-save must not keep showing its old thumbnail");
}

void TestThumbnails::sourceRoundTripsAwkwardNames()
{
    Thumbnails thumbnails;
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(1700000000123);
    for (const QString &path : {QStringLiteral("/tmp/plain.png"),
                                QStringLiteral("/tmp/shot #1 100% done?.png"),
                                QStringLiteral("/tmp/%2F%25 literal.png"),
                                QStringLiteral("/tmp/ünïcødé 写真.jpg")}) {
        const QUrl url(thumbnails.source(path, when, 42));
        QCOMPARE(url.scheme(), QStringLiteral("image"));
        QCOMPARE(url.host(), QStringLiteral("thumbnail"));
        QVERIFY2(url.query().isEmpty() && url.fragment().isEmpty(), qPrintable(url.toString()));
        // What QQuickPixmap hands the provider as the id.
        const QString id = url.toString(QUrl::RemoveScheme | QUrl::RemoveAuthority).mid(1);
        QCOMPARE(Thumbnails::pathFromId(id), path);
    }

    // Any change to the file changes the URL, and nothing else does.
    const QString base = thumbnails.source(QStringLiteral("/tmp/a.png"), when, 42);
    QCOMPARE(thumbnails.source(QStringLiteral("/tmp/a.png"), when, 42), base);
    QVERIFY(thumbnails.source(QStringLiteral("/tmp/a.png"), when.addMSecs(1), 42) != base);
    QVERIFY(thumbnails.source(QStringLiteral("/tmp/a.png"), when, 43) != base);
}

void TestThumbnails::providerAnswersOnlyTheVersionAsked()
{
    // Qt caches whatever comes back under the URL. A URL naming an older
    // version must not be answered with a picture of the file as it is now.
    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("versioned.png"), 300, 100);
    const QFileInfo info(path);
    Thumbnails thumbnails;
    ThumbnailProvider provider;
    const auto ask = [&](const QString &url) {
        const QString id = QUrl(url).toString(QUrl::RemoveScheme | QUrl::RemoveAuthority).mid(1);
        std::unique_ptr<QQuickImageResponse> response(
            provider.requestImageResponse(id, QSize(128, 128)));
        QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
        if (!finished.wait(10000))
            return QString("timeout");
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        return response->errorString().isEmpty() ? QString("image") : response->errorString();
    };

    QCOMPARE(ask(thumbnails.source(path, info.lastModified(), info.size())), QString("image"));
    QCOMPARE(ask(thumbnails.source(path, info.lastModified().addMSecs(-1), info.size())),
             QString("file changed"));
    QCOMPARE(ask(thumbnails.source(path, info.lastModified(), info.size() + 1)),
             QString("file changed"));
    // No mtime known (a remote row): whatever is there is the answer.
    QCOMPARE(ask(thumbnails.source(path, QDateTime(), 0)), QString("image"));
}

void TestThumbnails::remembersFailuresButNotForever()
{
    TempTree tree;
    const QString path = tree.writeFile(QStringLiteral("broken.png"), 32); // not a real PNG

    QVERIFY(!ThumbnailCache::hasFailed(path));
    ThumbnailCache::markFailed(path);
    QVERIFY2(ThumbnailCache::hasFailed(path),
             "a failure must be remembered, or every scroll retries it");

    // A file that has changed deserves another attempt.
    tree.setModified(QStringLiteral("broken.png"), QDateTime::currentDateTime().addSecs(120));
    QVERIFY2(!ThumbnailCache::hasFailed(path),
             "a changed file must be retried rather than written off forever");
}

void TestThumbnails::detectsTypeByContentNotExtension()
{
    // A real PNG saved with no extension. Guessing from the filename returns
    // application/octet-stream, nothing handles that, and the file silently
    // never gets a preview — which is exactly what happened.
    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("no-extension-here"), 200, 200);

    QCOMPARE(ThumbnailCache::contentTypeOf(path), QStringLiteral("image/png"));

    const QImage thumb = ThumbnailCache::render(path, ThumbnailCache::contentTypeOf(path), 128);
    QVERIFY2(!thumb.isNull(), "an extensionless image must still thumbnail");
}

void TestThumbnails::canThumbnailRespectsTypeAndSize()
{
    Thumbnails thumbnails;

    QVERIFY(thumbnails.canThumbnail(QStringLiteral("image/jpeg"), 1000));
    QVERIFY(!thumbnails.canThumbnail(QStringLiteral("text/plain"), 1000));
    QVERIFY(!thumbnails.canThumbnail(QString(), 1000));

    // The size cap guards the in-process image decode only.
    QVERIFY(!thumbnails.canThumbnail(QStringLiteral("image/jpeg"),
                                     thumbnails.maximumFileSize() + 1));

    // A video over the cap still thumbnails — the external thumbnailer reads
    // frames, not the whole file (Nautilus's rule; a 4GB recording previews).
    // External thumbnailers are optional; a minimal installation may have none.
    QCOMPARE(thumbnails.canThumbnail(QStringLiteral("video/mp4"),
                                      thumbnails.maximumFileSize() + 1),
             ThumbnailCache::canHandle(QStringLiteral("video/mp4")));

    thumbnails.setEnabled(false);
    QVERIFY(!thumbnails.canThumbnail(QStringLiteral("image/jpeg"), 1000));
}

void TestThumbnails::photoCachePersistsAcrossMemoryClears()
{
    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("photo-cache.png"), 640, 480);
    const auto version = ThumbnailCache::Version::of(path);
    const QImage thumbnail = ThumbnailCache::renderImageFile(path, 256);

    PhotoThumbnailCache::store(path, 256, version, thumbnail);
    QVERIFY(QFile::exists(PhotoThumbnailCache::cachePathFor(path, 256, version)));
    PhotoThumbnailCache::clearMemory();

    const QImage reloaded = PhotoThumbnailCache::load(path, 256, version);
    QVERIFY2(!reloaded.isNull(), "the disk cache must survive clearing decoded RAM images");
}

void TestThumbnails::photoCacheInvalidatesWhenSourceChanges()
{
    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("photo-edited.png"), 640, 480);
    const auto oldVersion = ThumbnailCache::Version::of(path);
    PhotoThumbnailCache::store(path, 256, oldVersion,
                               ThumbnailCache::renderImageFile(path, 256));

    tree.setModified(QStringLiteral("photo-edited.png"),
                     QDateTime::currentDateTime().addSecs(10));
    QVERIFY(PhotoThumbnailCache::load(path, 256, oldVersion).isNull());
    QVERIFY(PhotoThumbnailCache::cachePathFor(path, 256, oldVersion)
            != PhotoThumbnailCache::cachePathFor(path, 256,
                                                  ThumbnailCache::Version::of(path)));
}

void TestThumbnails::photoMemoryCacheIsBoundedAndClearable()
{
    TempTree tree;
    const qint64 previousLimit = PhotoThumbnailCache::memoryLimit();
    PhotoThumbnailCache::clearMemory();
    PhotoThumbnailCache::setMemoryLimit(80 * 1024);

    for (int index = 0; index < 3; ++index) {
        const QString path = writeImage(tree, QStringLiteral("bounded-%1.png").arg(index),
                                        128, 128);
        PhotoThumbnailCache::store(path, 256, ThumbnailCache::Version::of(path),
                                   ThumbnailCache::renderImageFile(path, 256));
    }

    QVERIFY(PhotoThumbnailCache::memoryCost() <= PhotoThumbnailCache::memoryLimit());
    QVERIFY(PhotoThumbnailCache::memoryCount() < 3);
    PhotoThumbnailCache::clearMemory();
    QCOMPARE(PhotoThumbnailCache::memoryCost(), 0);
    QCOMPARE(PhotoThumbnailCache::memoryCount(), 0);
    PhotoThumbnailCache::setMemoryLimit(previousLimit);
}

void TestThumbnails::photoProviderReleasesMemoryOnDestruction()
{
    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("lifecycle.png"), 128, 128);
    PhotoThumbnailCache::store(path, 256, ThumbnailCache::Version::of(path),
                               ThumbnailCache::renderImageFile(path, 256));
    QVERIFY(PhotoThumbnailCache::memoryCount() > 0);

    {
        PhotoThumbnailProvider provider;
    }

    QCOMPARE(PhotoThumbnailCache::memoryCount(), 0);
}

void TestThumbnails::providerSurvivesResponseDestructionWhileRunning()
{
    // Qt cancels and deletes a QQuickImageResponse when a cell scrolls away.
    // The worker must not touch that object afterwards — the SIGSEGV in
    // QObjectPrivate::cleanOrphanedConnectionsImpl was exactly that race.
    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("race.png"), 2500, 2500);
    const QFileInfo info(path);
    Thumbnails thumbnails;
    ThumbnailProvider provider;
    const QString id =
        QUrl(thumbnails.source(path, info.lastModified(), info.size()))
            .toString(QUrl::RemoveScheme | QUrl::RemoveAuthority)
            .mid(1);

    for (int i = 0; i < 80; ++i) {
        QQuickImageResponse *response = provider.requestImageResponse(id, QSize(256, 256));
        response->cancel();
        delete response;
    }

    QVERIFY(ThumbnailProvider::threadPool()->waitForDone(60000));
}

void TestThumbnails::photoProviderSurvivesResponseDestructionWhileRunning()
{
    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("photo-race.png"), 2500, 2500);
    const QFileInfo info(path);
    Thumbnails thumbnails;
    const QString id =
        QUrl(thumbnails.photoSource(path, info.lastModified(), info.size(), 256, 50))
            .toString(QUrl::RemoveScheme | QUrl::RemoveAuthority)
            .mid(1);

    {
        PhotoThumbnailProvider provider;
        for (int i = 0; i < 80; ++i) {
            QQuickImageResponse *response =
                provider.requestImageResponse(id, QSize(256, 256));
            response->cancel();
            delete response;
        }
    }
}

void TestThumbnails::photoSourceUrlIgnoresPriority()
{
    Thumbnails thumbnails;
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(1700000000123);
    const QString path = QStringLiteral("/tmp/album/shot.jpg");
    const QString low = thumbnails.photoSource(path, when, 42, 256, 10);
    const QString high = thumbnails.photoSource(path, when, 42, 256, 100);
    QCOMPARE(low, high);
    QVERIFY2(!low.contains(QStringLiteral("/10/")) && !low.contains(QStringLiteral("/100/")),
             qPrintable(low));
    QCOMPARE(Thumbnails::photoPriorityFor(path), 100);

    const QString id = QUrl(low).toString(QUrl::RemoveScheme | QUrl::RemoveAuthority).mid(1);
    QVERIFY(id.startsWith(QStringLiteral("256/")));
    PhotoThumbnailProvider provider;
    std::unique_ptr<QQuickImageResponse> response(
        provider.requestImageResponse(id, QSize(256, 256)));
    // Missing file → finished with an error, but must parse the new URL shape.
    QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
    QVERIFY(finished.wait(5000));
    QCOMPARE(response->errorString(), QStringLiteral("could not render"));
}

void TestThumbnails::slowStorageDetectsRemovableAndRemote()
{
    Thumbnails thumbnails;
    QVERIFY(thumbnails.isSlowStorage(QStringLiteral("smb://server/share")));
    QVERIFY(thumbnails.isSlowStorage(QStringLiteral("/run/media/robin/EXTERNE HD/photos")));
    QVERIFY(thumbnails.isSlowStorage(QStringLiteral("/run/user/1000/gvfs/smb-share:server=x")));
    QVERIFY(!thumbnails.isSlowStorage(QStringLiteral("/home/robin/Pictures")));
    // Absent /mnt is not a removable disk. A mounted SSD is classified from sysfs.
    QVERIFY(!thumbnails.isSlowStorage(QStringLiteral("/mnt/data/photos")));
    QVERIFY(!thumbnails.isSlowStorage(QDir::tempPath()));
}

void TestThumbnails::scaleModeSmoothsAResidualAndIsFastForAFullFrame()
{
    QCOMPARE(ThumbnailCache::scaleModeFor(QSize(300, 200), 256), Qt::SmoothTransformation);
    QCOMPARE(ThumbnailCache::scaleModeFor(QSize(512, 512), 256), Qt::SmoothTransformation);
    QCOMPARE(ThumbnailCache::scaleModeFor(QSize(6000, 4000), 256), Qt::FastTransformation);
}

void TestThumbnails::queuedPhotoPriorityFollowsTheViewport()
{
    PhotoThumbnailProvider provider;
    provider.threadPool()->setMaxThreadCount(1);

    class PoolBlocker : public QRunnable {
    public:
        QSemaphore entered;
        QSemaphore release;
        void run() override
        {
            entered.release();
            release.acquire();
        }
    };
    auto *blocker = new PoolBlocker;
    provider.threadPool()->start(blocker, 1000);
    QVERIFY(blocker->entered.tryAcquire(1, 5000));

    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("queued.png"), 64, 64);
    const QFileInfo info(path);
    Thumbnails thumbnails;
    const QString source = thumbnails.photoSource(path, info.lastModified(), info.size(), 256, 10);
    const QString id = QUrl(source).toString(QUrl::RemoveScheme | QUrl::RemoveAuthority).mid(1);
    std::unique_ptr<QQuickImageResponse> response(
        provider.requestImageResponse(id, QSize(256, 256)));
    QCOMPARE(provider.queuedPriority(path), 10);

    // Scrolling the cell into view, then away, then back, retargets the
    // queued decode. It must not cancel and restart from the file.
    thumbnails.notePhotoPriority(path, 100);
    QCOMPARE(provider.queuedPriority(path), 100);
    thumbnails.notePhotoPriority(path, 10);
    QCOMPARE(provider.queuedPriority(path), 10);
    thumbnails.notePhotoPriority(path, 100);
    QCOMPARE(provider.queuedPriority(path), 100);

    blocker->release.release();
    QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
    QVERIFY(finished.wait(5000));
    QCOMPARE(response->errorString(), QString());
    QCOMPARE(provider.queuedPriority(path), -1);
}

void TestThumbnails::cancelledDecodeStillWritesTheDiskCache()
{
    TempTree tree;
    const QString path = writeImage(tree, QStringLiteral("cancel-persist.png"), 64, 64);
    const auto version = ThumbnailCache::Version::of(path);
    const QString cachePath = PhotoThumbnailCache::cachePathFor(path, 256, version);
    QFile::remove(cachePath);
    PhotoThumbnailCache::clearMemory();

    struct Pause {
        std::atomic<bool> pause{false};
        std::atomic<bool> reached{false};
        Pause()
        {
            OmantaThumbnailTest::g_pauseBeforePhotoStore = &pause;
            OmantaThumbnailTest::g_reachedBeforePhotoStore = &reached;
        }
        ~Pause()
        {
            pause.store(true, std::memory_order_release);
            OmantaThumbnailTest::g_pauseBeforePhotoStore = nullptr;
            OmantaThumbnailTest::g_reachedBeforePhotoStore = nullptr;
        }
    } pause;

    const QFileInfo info(path);
    Thumbnails thumbnails;
    PhotoThumbnailProvider provider;
    const QString source = thumbnails.photoSource(path, info.lastModified(), info.size(), 256, 50);
    const QString id = QUrl(source).toString(QUrl::RemoveScheme | QUrl::RemoveAuthority).mid(1);
    QQuickImageResponse *response = provider.requestImageResponse(id, QSize(256, 256));
    QTRY_VERIFY(pause.reached.load(std::memory_order_acquire));
    response->cancel();
    delete response;
    pause.pause.store(true, std::memory_order_release);

    QVERIFY(provider.threadPool()->waitForDone(5000));
    QVERIFY2(QFile::exists(cachePath), qPrintable(cachePath));
}

void TestThumbnails::photoIoBoundTracksLiveViews()
{
    PhotoThumbnailProvider provider;
    Thumbnails thumbnails;
    auto slow = std::make_unique<QObject>();
    auto fast = std::make_unique<QObject>();
    const int fastThreads = qMax(2, qMin(4, QThread::idealThreadCount()));

    QVERIFY(!provider.ioBound());
    thumbnails.bindPhotoIo(slow.get(), true);
    QVERIFY(provider.ioBound());
    QCOMPARE(provider.threadPool()->maxThreadCount(), 2);

    // A fast album does not lift the limit while a spinning disk is still open.
    thumbnails.bindPhotoIo(fast.get(), false);
    QVERIFY(provider.ioBound());

    slow.reset();
    QVERIFY(!provider.ioBound());
    QCOMPARE(provider.threadPool()->maxThreadCount(), fastThreads);

    thumbnails.bindPhotoIo(fast.get(), true);
    QVERIFY(provider.ioBound());
    fast.reset();
    QVERIFY(!provider.ioBound());
    QCOMPARE(provider.threadPool()->maxThreadCount(), fastThreads);
}

QTEST_MAIN(TestThumbnails)
#include "tst_thumbnails.moc"
