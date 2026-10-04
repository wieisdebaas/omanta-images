#include "FaceIndexer.h"

#include "FaceEngine.h"
#include "FaceStore.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QStandardPaths>

namespace {

constexpr int kMaxPhotos = 10000;
constexpr int kMaxDirs = 2000;
constexpr int kMaxDepth = 20;

bool isCacheDir(const QString &path)
{
    const QString abs = QFileInfo(path).absoluteFilePath();
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    const QStringList roots = {
        cache + QStringLiteral("/omanta/photo-thumbnails"),
        cache + QStringLiteral("/omanta/face-crops"),
        cache + QStringLiteral("/thumbnails"),
    };
    for (const QString &root : roots) {
        const QString folder = QFileInfo(root).absoluteFilePath();
        if (abs == folder || abs.startsWith(folder + QLatin1Char('/')))
            return true;
    }
    return false;
}

QStringList collectImages(const QString &root, bool showHidden,
                          const std::atomic<quint64> *generation, quint64 token, bool *capped)
{
    QStringList files;
    struct Pending {
        QString path;
        int depth = 0;
    };
    QList<Pending> queue;
    queue.append({ root, 0 });

    QDir::Filters filters = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot;
    if (showHidden)
        filters |= QDir::Hidden;

    const QStringList suffixes = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("jpe"),
        QStringLiteral("png"), QStringLiteral("webp"), QStringLiteral("bmp"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("gif"),
        QStringLiteral("heic"), QStringLiteral("heif"), QStringLiteral("avif"),
    };

    int visited = 0;
    while (!queue.isEmpty()) {
        if (generation->load() != token)
            return {};
        const Pending dir = queue.takeFirst();
        if (isCacheDir(dir.path))
            continue;
        if (++visited > kMaxDirs) {
            *capped = true;
            break;
        }

        const QFileInfoList entries = QDir(dir.path).entryInfoList(filters, QDir::Name);
        for (const QFileInfo &info : entries) {
            if (generation->load() != token)
                return {};
            if (!showHidden && info.fileName().endsWith(QLatin1Char('~')))
                continue;
            if (info.isDir()) {
                if (!info.isSymLink() && dir.depth < kMaxDepth && !isCacheDir(info.absoluteFilePath()))
                    queue.append({ info.absoluteFilePath(), dir.depth + 1 });
                continue;
            }
            if (!info.isFile() || !suffixes.contains(info.suffix(), Qt::CaseInsensitive))
                continue;
            if (files.size() >= kMaxPhotos) {
                *capped = true;
                return files;
            }
            files.append(info.absoluteFilePath());
        }
    }
    return files;
}

class ScanWorker : public QObject
{
public:
    FaceEngine::Result detect(const QString &path, QString *error)
    {
        if (!m_engine.isLoaded()) {
            QString loadError;
            if (!m_engine.load(&loadError)) {
                FaceEngine::Result missing;
                missing.modelsMissing = true;
                if (error)
                    *error = loadError;
                return missing;
            }
        }
        return m_engine.detect(path, error);
    }

private:
    FaceEngine m_engine;
};

} // namespace

class FaceIndexer::Worker : public ScanWorker
{
};

FaceIndexer::FaceIndexer(QObject *parent)
    : QObject(parent)
{
    m_worker = new Worker;
    m_worker->moveToThread(&m_thread);
    m_thread.start();
}

FaceIndexer::~FaceIndexer()
{
    stop();
    if (m_worker) {
        // The worker lives on its own thread. Queue the delete there, then
        // let that thread exit, so we never delete a QObject from the wrong one.
        QMetaObject::invokeMethod(m_worker, &QObject::deleteLater, Qt::QueuedConnection);
        m_worker = nullptr;
    }
    m_thread.quit();
    m_thread.wait();
}

void FaceIndexer::start(FaceStore *store, const QString &root, bool showHidden)
{
    stop();
    m_store = store;
    m_root = root;
    m_done = 0;
    m_total = 0;
    m_capped = false;
    m_error.clear();
    m_indexing = true;
    const quint64 token = m_generation.load();
    Q_EMIT progress(0, 0);

    QMetaObject::invokeMethod(m_worker, [this, root, showHidden, token] {
        bool capped = false;
        const QStringList files = collectImages(root, showHidden, &m_generation, token, &capped);
        QMetaObject::invokeMethod(this, [this, token, files, capped] {
            collectFinished(token, files, capped);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void FaceIndexer::stop()
{
    m_generation.fetch_add(1);
    m_queue.clear();
    if (!m_indexing)
        return;
    m_indexing = false;
}

void FaceIndexer::setHeld(bool held)
{
    if (m_held == held)
        return;
    m_held = held;
    if (!m_held)
        pump();
}

void FaceIndexer::collectFinished(quint64 token, const QStringList &files, bool capped)
{
    if (token != m_generation.load())
        return;
    if (m_store && !m_root.isEmpty())
        m_store->forgetAbsent(m_root, files);
    m_queue = files;
    m_total = files.size();
    m_capped = capped;
    m_done = 0;
    Q_EMIT progress(m_done, m_total);
    pump();
}

void FaceIndexer::pump()
{
    if (!m_store || m_generation.load() == 0 || m_held)
        return;

    const quint64 token = m_generation.load();
    while (!m_queue.isEmpty()) {
        if (token != m_generation.load())
            return;
        const QString path = m_queue.takeFirst();
        const QFileInfo info(path);
        const qint64 size = info.size();
        const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
        if (m_store->isCurrent(path, size, mtime)) {
            ++m_done;
            Q_EMIT progress(m_done, m_total);
            continue;
        }

        QMetaObject::invokeMethod(m_worker, [this, path, token] {
            QString error;
            const FaceEngine::Result result = m_worker->detect(path, &error);
            const QFileInfo info(path);
            const qint64 size = info.size();
            const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
            QMetaObject::invokeMethod(this, [this, token, path, size, mtime, result, error] {
                detected(token, path, size, mtime, result.width, result.height, result.faces,
                         result.decoded, result.modelsMissing, error);
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
        return;
    }

    m_indexing = false;
    Q_EMIT progress(m_done, m_total);
    Q_EMIT finished();
}

void FaceIndexer::detected(quint64 token, const QString &path, qint64 size, qint64 mtimeMs,
                           int width, int height, const QVector<FaceSample> &faces,
                           bool decoded, bool modelsMissing, const QString &error)
{
    if (token != m_generation.load() || !m_store)
        return;

    if (modelsMissing) {
        m_error = error.isEmpty() ? QStringLiteral("Face models are not installed") : error;
        m_queue.clear();
        m_indexing = false;
        Q_EMIT finished();
        return;
    }

    if (decoded)
        m_store->replacePhoto(path, size, mtimeMs, width, height, faces);

    ++m_done;
    Q_EMIT progress(m_done, m_total);
    if (decoded && !faces.isEmpty())
        Q_EMIT photoIndexed();
    pump();
}
