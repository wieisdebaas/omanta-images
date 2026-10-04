#include "FacePhotoFilter.h"

#include "DirectoryModel.h"

#include <QDir>

namespace {

QString cleanPath(const QString &path)
{
    if (path.isEmpty())
        return {};
    return QDir::cleanPath(path);
}

} // namespace

FacePhotoFilter::FacePhotoFilter(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    connect(this, &QAbstractItemModel::rowsInserted, this, &FacePhotoFilter::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &FacePhotoFilter::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &FacePhotoFilter::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &FacePhotoFilter::countChanged);
}

void FacePhotoFilter::setFiltering(bool filtering)
{
    if (m_filtering == filtering)
        return;
    beginFilterChange();
    m_filtering = filtering;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    Q_EMIT filteringChanged();
    Q_EMIT countChanged();
}

void FacePhotoFilter::setPaths(const QStringList &paths)
{
    if (m_paths == paths)
        return;
    beginFilterChange();
    m_paths = paths;
    m_pathSet.clear();
    for (const QString &path : paths)
        m_pathSet.insert(cleanPath(path));
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    Q_EMIT pathsChanged();
    Q_EMIT countChanged();
}

bool FacePhotoFilter::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (!m_filtering)
        return true;
    if (!sourceModel())
        return false;

    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
    const QString file = cleanPath(idx.data(DirectoryModel::FilePathRole).toString());
    if (m_pathSet.contains(file))
        return true;
    const QString target = cleanPath(idx.data(DirectoryModel::TargetPathRole).toString());
    return !target.isEmpty() && m_pathSet.contains(target);
}

int FacePhotoFilter::role(const QString &name) const
{
    if (m_roles.isEmpty()) {
        const QHash<int, QByteArray> names = roleNames();
        for (auto it = names.cbegin(); it != names.cend(); ++it)
            m_roles.insert(QString::fromUtf8(it.value()), it.key());
    }
    return m_roles.value(name, -1);
}

int FacePhotoFilter::proxyRowForName(const QString &name) const
{
    for (int row = 0; row < rowCount(); ++row) {
        if (index(row, 0).data(DirectoryModel::NameRole).toString() == name)
            return row;
    }
    return -1;
}

QVariant FacePhotoFilter::valueAt(int proxyRow, const QString &roleName) const
{
    const QModelIndex idx = index(proxyRow, 0);
    if (!idx.isValid())
        return {};
    const int id = role(roleName);
    return id < 0 ? QVariant{} : idx.data(id);
}

int FacePhotoFilter::findByPrefix(const QString &prefix, int startRow) const
{
    const int total = rowCount();
    if (total == 0 || prefix.isEmpty())
        return -1;

    for (int offset = 0; offset < total; ++offset) {
        const int row = (startRow + offset) % total;
        const QString name = index(row, 0).data(DirectoryModel::DisplayNameRole).toString();
        if (name.startsWith(prefix, Qt::CaseInsensitive))
            return row;
    }
    return -1;
}
