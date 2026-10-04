#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QStringList>
#include <QtQmlIntegration>

// Photos of the people already named, for the People place. Paths come from
// the face index; rows speak the same roles as a folder listing so the photo
// grid can show them. A path whose file is gone is left out.
class FacePhotosModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QStringList paths READ paths WRITE setPaths NOTIFY pathsChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit FacePhotosModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QStringList paths() const { return m_paths; }
    void setPaths(const QStringList &paths);
    int count() const { return int(m_rows.size()); }

Q_SIGNALS:
    void pathsChanged();
    void countChanged();

private:
    struct Row {
        QString path;
        QString displayName;
        QString contentType;
        qint64 size = 0;
        QDateTime modified;
    };

    void rebuild();

    QStringList m_paths;
    QList<Row> m_rows;
};
