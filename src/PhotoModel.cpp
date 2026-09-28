#include "PhotoModel.h"

#include "DirectoryModel.h"
#include "Location.h"

namespace {

constexpr int kBatchSize = 256;
constexpr int kMaxParallelDirs = 4;
constexpr int kMaxPhotos = 10000;
constexpr int kMaxVisitedDirs = 2000;
constexpr int kMaxDepth = 20;
constexpr int kFlushBatch = 64;

} // namespace

PhotoModel::PhotoModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

PhotoModel::~PhotoModel()
{
    stop();
}

void PhotoModel::clearQueue()
{
    for (PendingDir &dir : m_queue)
        g_object_unref(dir.file);
    m_queue.clear();
}

void PhotoModel::stop()
{
    ++m_generation;
    if (m_cancellable) {
        g_cancellable_cancel(m_cancellable);
        g_object_unref(m_cancellable);
        m_cancellable = nullptr;
    }
    clearQueue();
    m_inFlight = 0;
    m_pending.clear();
    setScanning(false);
}

int PhotoModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_results.size());
}

QVariant PhotoModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_results.size())
        return {};

    const Result &result = m_results.at(index.row());
    switch (role) {
    case DirectoryModel::NameRole: return result.relPath;
    case DirectoryModel::DisplayNameRole: return result.relPath;
    case DirectoryModel::FilePathRole: return Location::descend(m_path, result.relPath);
    case DirectoryModel::IsDirRole: return result.entry.isDir;
    case DirectoryModel::IsHiddenRole: return result.entry.isHidden;
    case DirectoryModel::IsBackupRole: return result.entry.isBackup;
    case DirectoryModel::IsSymlinkRole: return result.entry.isSymlink;
    case DirectoryModel::SizeRole: return result.entry.size;
    case DirectoryModel::ModifiedRole: return result.entry.modified;
    case DirectoryModel::CreatedRole: return result.entry.created;
    case DirectoryModel::AccessedRole: return result.entry.accessed;
    case DirectoryModel::OwnerRole: return result.entry.owner;
    case DirectoryModel::GroupRole: return result.entry.group;
    case DirectoryModel::PermissionsRole: return result.entry.permissionString();
    case DirectoryModel::TargetPathRole: return result.entry.targetPath;
    case DirectoryModel::ContentTypeRole: return result.entry.contentType;
    case DirectoryModel::TypeDescriptionRole: return result.entry.typeDescription;
    case DirectoryModel::IconSourceRole:
        return QStringLiteral("image://fileicon/") + result.entry.iconNames.join(QLatin1Char(','));
    case DirectoryModel::OrigPathRole: return result.entry.origPath;
    case DirectoryModel::ItemCountRole: return result.entry.itemCount;
    case DirectoryModel::ItemCountAllRole: return result.entry.itemCountAll;
    case DirectoryModel::DepthRole: return 0;
    case DirectoryModel::ExpandedRole: return false;
    default: return {};
    }
}

QHash<int, QByteArray> PhotoModel::roleNames() const
{
    return DirectoryModel::fileRoles();
}

void PhotoModel::setPath(const QString &path)
{
    const QString clean = Location::clean(path);
    if (clean == m_path)
        return;
    m_path = clean;
    Q_EMIT pathChanged();
    if (m_active)
        restart();
}

void PhotoModel::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    Q_EMIT activeChanged();
    if (m_active)
        restart();
    else
        stop();
}

void PhotoModel::setShowHidden(bool showHidden)
{
    if (m_showHidden == showHidden)
        return;
    m_showHidden = showHidden;
    Q_EMIT showHiddenChanged();
    if (m_active)
        restart();
}

void PhotoModel::reload()
{
    if (m_active)
        restart();
}

void PhotoModel::setScanning(bool scanning)
{
    if (m_scanning == scanning)
        return;
    m_scanning = scanning;
    Q_EMIT scanningChanged();
}

void PhotoModel::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message)
        return;
    m_errorMessage = message;
    Q_EMIT errorMessageChanged();
}

void PhotoModel::restart()
{
    stop();

    beginResetModel();
    m_results.clear();
    endResetModel();
    Q_EMIT countChanged();

    m_visitedDirs = 0;
    if (m_capped) {
        m_capped = false;
        Q_EMIT cappedChanged();
    }
    setErrorMessage({});

    if (!m_active || m_path.isEmpty())
        return;

    // Special places are not GIO directories — leave the model empty.
    if (m_path == QLatin1String("starred:///") || m_path == QLatin1String("network:///"))
        return;

    setScanning(true);
    m_cancellable = g_cancellable_new();
    m_queue.append({ Location::make(m_path), QString(), 0 });
    pump();
}

void PhotoModel::pump()
{
    while (m_inFlight < kMaxParallelDirs && !m_queue.isEmpty()) {
        if (m_visitedDirs >= kMaxVisitedDirs || m_results.size() + m_pending.size() >= kMaxPhotos) {
            if (!m_capped) {
                m_capped = true;
                Q_EMIT cappedChanged();
            }
            clearQueue();
            break;
        }

        PendingDir dir = m_queue.takeFirst();
        if (dir.depth > kMaxDepth) {
            g_object_unref(dir.file);
            continue;
        }

        ++m_visitedDirs;
        ++m_inFlight;

        auto *ctx = new EnumerateCtx{ this, m_generation, dir.relPrefix, dir.depth };
        g_file_enumerate_children_async(dir.file, FileEntry::queryAttributes(),
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        G_PRIORITY_DEFAULT, m_cancellable,
                                        &PhotoModel::onEnumerateReady, ctx);
        g_object_unref(dir.file);
    }

    if (m_inFlight == 0) {
        flushPending();
        setScanning(false);
    }
}

void PhotoModel::finishDir()
{
    --m_inFlight;
    if (m_pending.size() >= kFlushBatch)
        flushPending();
    pump();
}

void PhotoModel::flushPending()
{
    if (m_pending.isEmpty())
        return;

    QList<Result> batch = std::move(m_pending);
    m_pending = {};
    if (m_results.size() >= kMaxPhotos)
        return;
    if (m_results.size() + batch.size() > kMaxPhotos) {
        batch = batch.mid(0, kMaxPhotos - m_results.size());
        if (!m_capped) {
            m_capped = true;
            Q_EMIT cappedChanged();
        }
    }

    beginInsertRows({}, int(m_results.size()), int(m_results.size() + batch.size()) - 1);
    m_results += batch;
    endInsertRows();
    Q_EMIT countChanged();
}

void PhotoModel::onEnumerateReady(GObject *source, GAsyncResult *res, gpointer data)
{
    auto *ctx = static_cast<EnumerateCtx *>(data);
    GError *error = nullptr;
    GFileEnumerator *enumerator = g_file_enumerate_children_finish(G_FILE(source), res, &error);

    PhotoModel *self = ctx->self.data();
    if (!self || ctx->generation != self->m_generation) {
        if (enumerator)
            g_object_unref(enumerator);
        g_clear_error(&error);
        delete ctx;
        return;
    }

    if (!enumerator) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED) && ctx->relPrefix.isEmpty()) {
            Q_EMIT self->needsMount(self->m_path);
        } else if (ctx->relPrefix.isEmpty()
                   && error
                   && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            self->setErrorMessage(QString::fromUtf8(error->message));
        }
        g_clear_error(&error);
        self->finishDir();
        delete ctx;
        return;
    }

    g_file_enumerator_next_files_async(enumerator, kBatchSize, G_PRIORITY_DEFAULT,
                                       self->m_cancellable,
                                       &PhotoModel::onNextFilesReady, ctx);
}

void PhotoModel::onNextFilesReady(GObject *source, GAsyncResult *res, gpointer data)
{
    auto *ctx = static_cast<EnumerateCtx *>(data);
    auto *enumerator = G_FILE_ENUMERATOR(source);

    GError *error = nullptr;
    GList *infos = g_file_enumerator_next_files_finish(enumerator, res, &error);

    PhotoModel *self = ctx->self.data();
    if (!self || ctx->generation != self->m_generation) {
        g_list_free_full(infos, g_object_unref);
        g_clear_error(&error);
        g_object_unref(enumerator);
        delete ctx;
        return;
    }

    if (!infos) {
        g_clear_error(&error);
        g_object_unref(enumerator);
        self->finishDir();
        delete ctx;
        return;
    }

    self->consume(infos, ctx->relPrefix, ctx->depth);
    g_list_free_full(infos, g_object_unref);

    g_file_enumerator_next_files_async(enumerator, kBatchSize, G_PRIORITY_DEFAULT,
                                       self->m_cancellable,
                                       &PhotoModel::onNextFilesReady, ctx);
}

void PhotoModel::consume(GList *infos, const QString &relPrefix, int depth)
{
    for (GList *l = infos; l; l = l->next) {
        FileEntry entry = FileEntry::fromInfo(static_cast<GFileInfo *>(l->data));
        if (!m_showHidden && (entry.isHidden || entry.isBackup))
            continue;

        const QString relPath = relPrefix + entry.name;

        if (entry.isDir && !entry.isSymlink && depth < kMaxDepth
            && m_visitedDirs < kMaxVisitedDirs
            && m_results.size() + m_pending.size() < kMaxPhotos) {
            GFile *child = Location::make(Location::descend(m_path, relPath));
            m_queue.append({ child, relPath + QLatin1Char('/'), depth + 1 });
        }

        if (!entry.isDir && entry.contentType.startsWith(QLatin1String("image/")))
            m_pending.append({ std::move(entry), relPath });
    }

    if (m_pending.size() >= kFlushBatch)
        flushPending();

    pump();
}
