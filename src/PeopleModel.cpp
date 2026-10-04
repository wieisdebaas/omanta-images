#include "PeopleModel.h"

#include "FaceStore.h"

#include <QUrl>

PeopleModel::PeopleModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PeopleModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_people.size());
}

QHash<int, QByteArray> PeopleModel::roleNames() const
{
    return {
        { PersonIdRole, "personId" },
        { NameRole, "name" },
        { LabelRole, "label" },
        { PhotoCountRole, "photoCount" },
        { CropRole, "crop" },
        { NamedRole, "named" },
        { StrangerRole, "stranger" },
    };
}

QVariant PeopleModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_people.size())
        return {};
    const PersonInfo &person = m_people.at(index.row());
    switch (role) {
    case PersonIdRole:
        return person.id;
    case NameRole:
        return person.name;
    case LabelRole:
        if (person.stranger)
            return QStringLiteral("Strangers");
        return person.name.isEmpty() ? QStringLiteral("Add name") : person.name;
    case PhotoCountRole:
        return person.photoCount;
    case CropRole:
        return person.cropPath.isEmpty() ? QString()
                                         : QUrl::fromLocalFile(person.cropPath).toString();
    case NamedRole:
        return !person.stranger && !person.name.isEmpty();
    case StrangerRole:
        return person.stranger;
    default:
        return {};
    }
}

QString PeopleModel::labelFor(qint64 personId) const
{
    for (int row = 0; row < m_people.size(); ++row) {
        if (m_people.at(row).id == personId) {
            if (m_people.at(row).stranger)
                return QStringLiteral("Strangers");
            return m_people.at(row).name.isEmpty()
                ? QStringLiteral("Person %1").arg(row + 1)
                : m_people.at(row).name;
        }
    }
    return {};
}

void PeopleModel::setStore(FaceStore *store)
{
    m_store = store;
}

void PeopleModel::setScope(const QString &root, bool namedOnly)
{
    if (m_root == root && m_namedOnly == namedOnly)
        return;
    m_root = root;
    m_namedOnly = namedOnly;
    reload();
}

void PeopleModel::reload()
{
    beginResetModel();
    if (!m_store)
        m_people.clear();
    else if (m_namedOnly)
        m_people = m_store->namedPeople();
    else
        m_people = m_store->peopleUnder(m_root);
    endResetModel();
    Q_EMIT countChanged();
}
