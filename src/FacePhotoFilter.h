#pragma once

#include <QSet>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QtQmlIntegration>

// Photo grid filtered to one person's pictures. With filtering off it is a
// pass-through, so Ctrl+3 can always sit on this proxy and keep the same
// row numbers the tab's selection already uses.
class FacePhotoFilter : public QSortFilterProxyModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool filtering READ filtering WRITE setFiltering NOTIFY filteringChanged)
    Q_PROPERTY(QStringList paths READ paths WRITE setPaths NOTIFY pathsChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit FacePhotoFilter(QObject *parent = nullptr);

    bool filtering() const { return m_filtering; }
    void setFiltering(bool filtering);

    QStringList paths() const { return m_paths; }
    void setPaths(const QStringList &paths);

    int count() const { return rowCount(); }

    Q_INVOKABLE int proxyRowForName(const QString &name) const;
    Q_INVOKABLE QVariant valueAt(int proxyRow, const QString &roleName) const;
    Q_INVOKABLE int findByPrefix(const QString &prefix, int startRow) const;

Q_SIGNALS:
    void filteringChanged();
    void pathsChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    int role(const QString &name) const;

    bool m_filtering = false;
    QStringList m_paths;
    QSet<QString> m_pathSet;
    mutable QHash<QString, int> m_roles;
};
