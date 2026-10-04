#include "Faces.h"

#include "Location.h"

#include <QFileInfo>
#include <QUrl>

Faces::Faces(QObject *parent)
    : QObject(parent)
{
    m_people.setStore(&m_store);
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(200);
    connect(&m_refresh, &QTimer::timeout, this, [this] { noteEdit(); });
    connect(&m_indexer, &FaceIndexer::progress, this, &Faces::statusChanged);
    connect(&m_indexer, &FaceIndexer::finished, this, [this] {
        noteEdit();
        Q_EMIT statusChanged();
    });
    connect(&m_indexer, &FaceIndexer::photoIndexed, this, [this] {
        if (!m_refresh.isActive())
            m_refresh.start();
    });
    connect(&m_store, &FaceStore::changed, this, [this] {
        if (!m_refresh.isActive())
            m_refresh.start();
    });
}

Faces::~Faces()
{
    m_indexer.disconnect(this);
    m_indexer.stop();
}

void Faces::watch(const QString &folder, bool showHidden, bool enabled)
{
    if (!enabled || folder.isEmpty() || !Location::isLocal(folder)) {
        const bool was = m_watching || m_library || m_indexer.indexing() || !m_selected.isEmpty();
        m_indexer.stop();
        m_watching = false;
        m_library = false;
        m_root.clear();
        m_people.setScope({}, false);
        m_selected.clear();
        if (was) {
            Q_EMIT statusChanged();
            Q_EMIT selectionChanged();
        }
        return;
    }

    const QString root = Location::clean(folder);
    // Same folder, already watching: a settings change that is not this
    // switch must not start the walk over.
    if (root == m_root && showHidden == m_showHidden && m_watching && !m_library)
        return;

    if (root != m_root)
        m_selected.clear();
    m_root = root;
    m_showHidden = showHidden;
    m_watching = true;
    m_library = false;
    m_people.setScope(root, false);
    m_indexer.start(&m_store, root, showHidden);
    Q_EMIT statusChanged();
    Q_EMIT selectionChanged();
}

void Faces::openLibrary()
{
    if (m_library && !m_watching && !m_indexer.indexing())
        return;

    m_indexer.stop();
    m_watching = false;
    m_library = true;
    m_root.clear();
    m_selected.clear();
    m_people.setScope({}, true);
    m_store.forgetMissingFiles();
    Q_EMIT statusChanged();
    Q_EMIT selectionChanged();
}

void Faces::rescan()
{
    if (m_library) {
        m_store.forgetMissingFiles();
        noteEdit();
        return;
    }
    if (!m_watching || m_root.isEmpty())
        return;
    m_indexer.start(&m_store, m_root, m_showHidden);
    Q_EMIT statusChanged();
}

void Faces::setPaused(bool paused)
{
    if (m_paused == paused)
        return;
    m_paused = paused;
    applyHold();
    Q_EMIT statusChanged();
}

void Faces::setScrolling(bool scrolling)
{
    if (m_scrolling == scrolling)
        return;
    m_scrolling = scrolling;
    applyHold();
}

void Faces::applyHold()
{
    m_indexer.setHeld(m_paused || m_scrolling);
}

void Faces::togglePerson(qint64 personId)
{
    if (personId == 0)
        return;
    const int at = m_selected.indexOf(personId);
    if (at >= 0) {
        m_selected.removeAt(at);
    } else if (m_selected.size() < 2) {
        m_selected.append(personId);
    } else {
        m_selected.removeLast();
        m_selected.append(personId);
    }
    Q_EMIT selectionChanged();
}

bool Faces::renamePerson(qint64 personId, const QString &name)
{
    if (!m_store.renamePerson(personId, name))
        return false;
    noteEdit();
    return true;
}

bool Faces::applyName(qint64 personId, const QString &name, const QVariantList &excludeFaceIds)
{
    QList<qint64> exclude;
    exclude.reserve(excludeFaceIds.size());
    for (const QVariant &value : excludeFaceIds)
        exclude.append(value.toLongLong());
    if (!m_store.applyName(personId, name, exclude))
        return false;
    noteEdit();
    return true;
}

bool Faces::mergeSelected()
{
    if (m_selected.size() != 2)
        return false;
    const qint64 keep = m_selected.at(0);
    const qint64 drop = m_selected.at(1);
    if (!m_store.mergePeople(keep, drop))
        return false;
    m_selected = { keep };
    noteEdit();
    return true;
}

void Faces::markAsStranger(qint64 personId)
{
    const qint64 bucket = m_store.markAsStranger(personId);
    if (bucket == 0)
        return;
    for (qint64 &id : m_selected) {
        if (id == personId)
            id = bucket;
    }
    QList<qint64> unique;
    for (qint64 id : m_selected) {
        if (!unique.contains(id))
            unique.append(id);
    }
    m_selected = unique;
    noteEdit();
}

qint64 Faces::detachFace(qint64 faceId)
{
    const qint64 created = m_store.detachFace(faceId);
    if (created == 0)
        return 0;
    noteEdit();
    return created;
}

QVariantList Faces::cluster(qint64 personId) const
{
    QVariantList list;
    const QVector<PersonFace> faces = m_store.facesOfPerson(personId, scopeRoot());
    for (const PersonFace &face : faces) {
        list.append(QVariantMap {
            { QStringLiteral("faceId"), face.faceId },
            { QStringLiteral("crop"), face.cropPath.isEmpty()
                ? QString() : QUrl::fromLocalFile(face.cropPath).toString() },
            { QStringLiteral("path"), face.uri },
        });
    }
    return list;
}

QVariantList Faces::facesOn(const QString &path) const
{
    QVariantList list;
    const QVector<FaceMark> marks = m_store.facesFor(path);
    for (const FaceMark &mark : marks) {
        list.append(QVariantMap {
            { QStringLiteral("faceId"), mark.faceId },
            { QStringLiteral("personId"), mark.personId },
            { QStringLiteral("name"), mark.name },
            { QStringLiteral("stranger"), mark.stranger },
            { QStringLiteral("x"), mark.box.x() },
            { QStringLiteral("y"), mark.box.y() },
            { QStringLiteral("width"), mark.box.width() },
            { QStringLiteral("height"), mark.box.height() },
            { QStringLiteral("imageWidth"), mark.imageWidth },
            { QStringLiteral("imageHeight"), mark.imageHeight },
        });
    }
    return list;
}

QString Faces::previewSource(const QString &path) const
{
    return QStringLiteral("image://facepreview/p/")
        + QString::fromLatin1(QUrl::toPercentEncoding(path, "/"));
}

QString Faces::status() const
{
    if (!m_watching)
        return {};
    if (!m_indexer.error().isEmpty())
        return m_indexer.error();
    if (m_indexer.indexing() && m_paused)
        return QStringLiteral("Face indexing paused");
    if (m_indexer.indexing()) {
        if (m_indexer.total() <= 0)
            return QStringLiteral("Indexing faces…");
        return QStringLiteral("Indexing faces… %1 of %2")
            .arg(m_indexer.done())
            .arg(m_indexer.total());
    }
    if (m_indexer.capped())
        return QStringLiteral("Face index reached its limit");
    return {};
}

QVariantList Faces::selectedPersonIds() const
{
    QVariantList ids;
    for (qint64 id : m_selected)
        ids.append(id);
    return ids;
}

QString Faces::selectedLabel() const
{
    if (m_selected.isEmpty())
        return {};
    if (m_selected.size() == 1)
        return m_people.labelFor(m_selected.first());
    return m_people.labelFor(m_selected.at(0)) + QStringLiteral(" and ")
        + m_people.labelFor(m_selected.at(1));
}

QString Faces::scopeRoot() const
{
    return m_library ? QString() : m_root;
}

QStringList Faces::selectedPaths() const
{
    if (m_selected.isEmpty())
        return {};
    if (!m_library && m_root.isEmpty())
        return {};
    const QString root = scopeRoot();
    if (m_selected.size() == 1)
        return m_store.photosForPerson(m_selected.first(), root);
    return m_store.photosWithBoth(m_selected.at(0), m_selected.at(1), root);
}

QStringList Faces::libraryPaths() const
{
    if (m_selected.isEmpty())
        return m_store.photosOfNamed();
    if (m_selected.size() == 1)
        return m_store.photosForPerson(m_selected.first(), QString());
    return m_store.photosWithBoth(m_selected.at(0), m_selected.at(1), QString());
}

void Faces::noteEdit()
{
    m_people.reload();
    pruneSelection();
    ++m_revision;
    Q_EMIT revisionChanged();
    Q_EMIT selectionChanged();
}

void Faces::pruneSelection()
{
    QList<qint64> kept;
    for (qint64 id : m_selected) {
        if (m_store.personExists(id))
            kept.append(id);
    }
    m_selected = kept;
}
