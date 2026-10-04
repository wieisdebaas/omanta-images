#pragma once

#include "FaceTypes.h"

#include <QAbstractListModel>
#include <QtQmlIntegration>

class FaceStore;

// The chip row. In a folder it lists everyone found there, named or not.
// In the People place it lists only people who already have a name.
class PeopleModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Faces.people")

    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        PersonIdRole = Qt::UserRole + 1,
        NameRole,
        LabelRole,
        PhotoCountRole,
        CropRole,
        NamedRole,
        StrangerRole,
    };
    Q_ENUM(Roles)

    explicit PeopleModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_people.size()); }
    QString labelFor(qint64 personId) const;

    void setStore(FaceStore *store);
    void setScope(const QString &root, bool namedOnly);
    void reload();

Q_SIGNALS:
    void countChanged();

private:
    FaceStore *m_store = nullptr;
    QString m_root;
    bool m_namedOnly = false;
    QVector<PersonInfo> m_people;
};
