#include "FacePhotosModel.h"

#include "DirectoryModel.h"

#include <QFileInfo>
#include <QMimeDatabase>

#include <algorithm>

FacePhotosModel::FacePhotosModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int FacePhotosModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QHash<int, QByteArray> FacePhotosModel::roleNames() const
{
    return DirectoryModel::fileRoles();
}

void FacePhotosModel::setPaths(const QStringList &paths)
{
    if (m_paths == paths)
        return;
    m_paths = paths;
    rebuild();
    Q_EMIT pathsChanged();
}

void FacePhotosModel::rebuild()
{
    QList<Row> next;
    QMimeDatabase mime;
    for (const QString &path : m_paths) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile())
            continue;
        Row row;
        row.path = info.absoluteFilePath();
        row.displayName = info.fileName();
        row.size = info.size();
        row.modified = info.lastModified();
        row.contentType = mime.mimeTypeForFile(info).name();
        next.append(row);
    }
    std::sort(next.begin(), next.end(), [](const Row &a, const Row &b) {
        if (a.modified != b.modified)
            return a.modified > b.modified;
        return a.path < b.path;
    });

    beginResetModel();
    m_rows = next;
    endResetModel();
    Q_EMIT countChanged();
}

QVariant FacePhotosModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case DirectoryModel::NameRole: return row.path;
    case DirectoryModel::DisplayNameRole: return row.displayName;
    case DirectoryModel::FilePathRole: return row.path;
    case DirectoryModel::IsDirRole: return false;
    case DirectoryModel::IsHiddenRole: return false;
    case DirectoryModel::IsBackupRole: return false;
    case DirectoryModel::IsSymlinkRole: return false;
    case DirectoryModel::SizeRole: return row.size;
    case DirectoryModel::ModifiedRole: return row.modified;
    case DirectoryModel::ContentTypeRole: return row.contentType;
    case DirectoryModel::TargetPathRole: return QString();
    case DirectoryModel::DepthRole: return 0;
    case DirectoryModel::ExpandedRole: return false;
    case DirectoryModel::ItemCountRole: return -1;
    case DirectoryModel::ItemCountAllRole: return -1;
    default: return {};
    }
}
