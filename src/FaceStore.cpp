#include "FaceStore.h"

#include "Location.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

bool underRoot(const QString &root, const QString &path)
{
    if (root.isEmpty() || path.isEmpty())
        return false;
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return path.startsWith(prefix);
}

// An empty root is the whole library. A set root stays inside that folder.
bool inScope(const QString &root, const QString &path)
{
    if (path.isEmpty())
        return false;
    if (root.isEmpty())
        return true;
    return underRoot(root, path);
}

QByteArray embeddingBlob(const QVector<float> &embedding)
{
    return QByteArray(reinterpret_cast<const char *>(embedding.constData()),
                      embedding.size() * int(sizeof(float)));
}

QVector<float> embeddingFromBlob(const QByteArray &blob)
{
    if (blob.size() % int(sizeof(float)) != 0)
        return {};
    QVector<float> values(blob.size() / int(sizeof(float)));
    memcpy(values.data(), blob.constData(), size_t(blob.size()));
    return values;
}

} // namespace

FaceStore::FaceStore(QObject *parent)
    : QObject(parent)
{
}

FaceStore::~FaceStore()
{
    const QString name = m_connection;
    if (name.isEmpty())
        return;
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
}

bool FaceStore::isOpen() const
{
    return const_cast<FaceStore *>(this)->ensureOpen();
}

float FaceStore::cosine(const QVector<float> &a, const QVector<float> &b)
{
    if (a.isEmpty() || a.size() != b.size())
        return -1.f;

    double dot = 0;
    double na = 0;
    double nb = 0;
    for (int i = 0; i < a.size(); ++i) {
        dot += double(a.at(i)) * double(b.at(i));
        na += double(a.at(i)) * double(a.at(i));
        nb += double(b.at(i)) * double(b.at(i));
    }
    if (na <= 0 || nb <= 0)
        return -1.f;
    return float(dot / (std::sqrt(na) * std::sqrt(nb)));
}

QString FaceStore::databasePath() const
{
    const QString override = qEnvironmentVariable("OMANTA_FACES_DB");
    if (!override.isEmpty())
        return override;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/omanta/faces.sqlite");
}

QString FaceStore::cropDirectory() const
{
    const QString override = qEnvironmentVariable("OMANTA_FACES_CROP_DIR");
    if (!override.isEmpty())
        return override;
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/omanta/face-crops");
}

bool FaceStore::ensureOpen()
{
    if (m_db.isOpen())
        return true;
    if (m_openTried)
        return false;
    m_openTried = true;

    const QString path = databasePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    m_cropDir = cropDirectory();
    QDir().mkpath(m_cropDir);

    m_connection = QStringLiteral("omanta-faces-%1").arg(quintptr(this));
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    m_db.setDatabaseName(path);
    if (!m_db.open())
        return false;

    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("PRAGMA foreign_keys = ON"))
        || !q.exec(QStringLiteral("PRAGMA journal_mode = WAL"))
        || !q.exec(QStringLiteral(
               "CREATE TABLE IF NOT EXISTS photos ("
               "  uri TEXT PRIMARY KEY,"
               "  size INTEGER NOT NULL,"
               "  mtime_ms INTEGER NOT NULL,"
               "  width INTEGER NOT NULL,"
               "  height INTEGER NOT NULL)"))
        || !q.exec(QStringLiteral(
               "CREATE TABLE IF NOT EXISTS people ("
               "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name TEXT NOT NULL DEFAULT '',"
               "  stranger INTEGER NOT NULL DEFAULT 0)"))
        || !q.exec(QStringLiteral(
               "CREATE TABLE IF NOT EXISTS faces ("
               "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  uri TEXT NOT NULL,"
               "  person_id INTEGER NOT NULL,"
               "  x REAL NOT NULL,"
               "  y REAL NOT NULL,"
               "  w REAL NOT NULL,"
               "  h REAL NOT NULL,"
               "  score REAL NOT NULL,"
               "  embedding BLOB NOT NULL,"
               "  crop_path TEXT NOT NULL DEFAULT '',"
               "  FOREIGN KEY (uri) REFERENCES photos(uri) ON DELETE CASCADE,"
               "  FOREIGN KEY (person_id) REFERENCES people(id))"))
        || !q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS faces_uri ON faces(uri)"))
        || !q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS faces_person ON faces(person_id)"))
        || !ensureStrangerColumn()) {
        m_db.close();
        return false;
    }
    return true;
}

bool FaceStore::ensureStrangerColumn()
{
    QSqlQuery info(m_db);
    if (!info.exec(QStringLiteral("PRAGMA table_info(people)")))
        return false;
    while (info.next()) {
        if (info.value(1).toString() == QLatin1String("stranger"))
            return true;
    }
    QSqlQuery alter(m_db);
    return alter.exec(QStringLiteral(
        "ALTER TABLE people ADD COLUMN stranger INTEGER NOT NULL DEFAULT 0"));
}

QVector<FaceStore::Candidate> FaceStore::loadCandidates() const
{
    QVector<Candidate> found;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT person_id, embedding FROM faces")))
        return found;
    while (q.next()) {
        const QVector<float> embedding = embeddingFromBlob(q.value(1).toByteArray());
        if (embedding.isEmpty())
            continue;
        found.append({ q.value(0).toLongLong(), embedding });
    }
    return found;
}

bool FaceStore::isCurrent(const QString &path, qint64 size, qint64 mtimeMs)
{
    if (!ensureOpen())
        return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT size, mtime_ms FROM photos WHERE uri = ?"));
    q.addBindValue(Location::clean(path));
    if (!q.exec() || !q.next())
        return false;
    return q.value(0).toLongLong() == size && q.value(1).toLongLong() == mtimeMs;
}

void FaceStore::replacePhoto(const QString &path, qint64 size, qint64 mtimeMs,
                             int width, int height, const QVector<FaceSample> &faces)
{
    if (!ensureOpen())
        return;

    const QString uri = Location::clean(path);
    const QVector<Candidate> existing = loadCandidates();

    // Match against what is already stored, including this file's previous
    // faces, before those rows are deleted. Faces being added in this call
    // are not candidates for each other.
    QVector<qint64> matched;
    matched.reserve(faces.size());
    for (const FaceSample &face : faces) {
        qint64 personId = 0;
        float best = kSamePersonCosine;
        if (face.embedding.size() == FaceEmbeddingLength) {
            for (const Candidate &candidate : existing) {
                if (candidate.embedding.size() != face.embedding.size())
                    continue;
                const float score = cosine(face.embedding, candidate.embedding);
                if (score >= best) {
                    best = score;
                    personId = candidate.personId;
                }
            }
        }
        matched.append(personId);
    }

    QStringList oldCrops;
    {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral("SELECT crop_path FROM faces WHERE uri = ?"));
        q.addBindValue(uri);
        if (q.exec()) {
            while (q.next()) {
                const QString crop = q.value(0).toString();
                if (!crop.isEmpty())
                    oldCrops.append(crop);
            }
        }
    }

    if (!m_db.transaction())
        return;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM photos WHERE uri = ?"));
    q.addBindValue(uri);
    if (!q.exec()) {
        m_db.rollback();
        return;
    }

    q.prepare(QStringLiteral(
        "INSERT INTO photos (uri, size, mtime_ms, width, height) VALUES (?, ?, ?, ?, ?)"));
    q.addBindValue(uri);
    q.addBindValue(size);
    q.addBindValue(mtimeMs);
    q.addBindValue(width);
    q.addBindValue(height);
    if (!q.exec()) {
        m_db.rollback();
        return;
    }

    QVector<qint64> faceIds;
    faceIds.reserve(faces.size());
    for (int i = 0; i < faces.size(); ++i) {
        qint64 personId = matched.at(i);
        if (personId == 0) {
            QSqlQuery person(m_db);
            person.prepare(QStringLiteral("INSERT INTO people (name) VALUES ('')"));
            if (!person.exec()) {
                m_db.rollback();
                return;
            }
            personId = person.lastInsertId().toLongLong();
        }

        const FaceSample &face = faces.at(i);
        q.prepare(QStringLiteral(
            "INSERT INTO faces (uri, person_id, x, y, w, h, score, embedding, crop_path) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, '')"));
        q.addBindValue(uri);
        q.addBindValue(personId);
        q.addBindValue(face.box.x());
        q.addBindValue(face.box.y());
        q.addBindValue(face.box.width());
        q.addBindValue(face.box.height());
        q.addBindValue(face.score);
        q.addBindValue(embeddingBlob(face.embedding));
        if (!q.exec()) {
            m_db.rollback();
            return;
        }
        faceIds.append(q.lastInsertId().toLongLong());
    }

    if (!q.exec(QStringLiteral(
            "DELETE FROM people WHERE id NOT IN (SELECT person_id FROM faces)"))) {
        m_db.rollback();
        return;
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return;
    }

    for (int i = 0; i < faces.size(); ++i) {
        if (faces.at(i).crop.isNull())
            continue;
        const QString cropPath = m_cropDir + QLatin1Char('/') + QString::number(faceIds.at(i))
            + QStringLiteral(".png");
        if (!faces.at(i).crop.save(cropPath, "PNG"))
            continue;
        QSqlQuery update(m_db);
        update.prepare(QStringLiteral("UPDATE faces SET crop_path = ? WHERE id = ?"));
        update.addBindValue(cropPath);
        update.addBindValue(faceIds.at(i));
        update.exec();
    }

    for (const QString &crop : oldCrops)
        QFile::remove(crop);

    Q_EMIT changed();
}

QVector<PersonInfo> FaceStore::collectPeople(const QString &root, bool namedOnly) const
{
    QVector<PersonInfo> people;
    if (!namedOnly && root.isEmpty())
        return people;
    if (!const_cast<FaceStore *>(this)->ensureOpen())
        return people;

    const QString folder = root.isEmpty() ? QString() : Location::clean(root);
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "SELECT people.id, people.name, faces.uri, faces.score, faces.crop_path, "
            "       people.stranger "
            "FROM faces JOIN people ON people.id = faces.person_id")))
        return people;

    struct Accum {
        PersonInfo person;
        QSet<QString> photos;
    };
    QHash<qint64, Accum> byId;
    while (q.next()) {
        const QString name = q.value(1).toString();
        const bool stranger = q.value(5).toInt() != 0;
        if (namedOnly && (stranger || name.trimmed().isEmpty()))
            continue;
        const QString uri = q.value(2).toString();
        if (!namedOnly && !underRoot(folder, uri))
            continue;
        if (namedOnly && !folder.isEmpty() && !underRoot(folder, uri))
            continue;
        const qint64 id = q.value(0).toLongLong();
        Accum &accum = byId[id];
        accum.person.id = id;
        accum.person.name = name;
        accum.person.stranger = stranger;
        accum.photos.insert(uri);
        const float score = q.value(3).toFloat();
        if (accum.person.cropPath.isEmpty() || score >= accum.person.score) {
            accum.person.score = score;
            const QString crop = q.value(4).toString();
            if (!crop.isEmpty())
                accum.person.cropPath = crop;
        }
    }

    people.reserve(byId.size());
    for (auto it = byId.cbegin(); it != byId.cend(); ++it) {
        PersonInfo person = it->person;
        person.photoCount = int(it->photos.size());
        people.append(person);
    }
    std::sort(people.begin(), people.end(), [](const PersonInfo &a, const PersonInfo &b) {
        if (a.stranger != b.stranger)
            return a.stranger;
        if (a.score != b.score)
            return a.score > b.score;
        return a.id < b.id;
    });
    return people;
}

QVector<PersonInfo> FaceStore::peopleUnder(const QString &root) const
{
    return collectPeople(root, false);
}

QVector<PersonInfo> FaceStore::namedPeople() const
{
    return collectPeople(QString(), true);
}

QStringList FaceStore::photosForPerson(qint64 personId, const QString &root) const
{
    QStringList paths;
    if (!const_cast<FaceStore *>(this)->ensureOpen())
        return paths;

    const QString folder = root.isEmpty() ? QString() : Location::clean(root);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT DISTINCT uri FROM faces WHERE person_id = ?"));
    q.addBindValue(personId);
    if (!q.exec())
        return paths;
    while (q.next()) {
        const QString uri = q.value(0).toString();
        if (inScope(folder, uri))
            paths.append(uri);
    }
    paths.sort();
    return paths;
}

QStringList FaceStore::photosWithBoth(qint64 personA, qint64 personB, const QString &root) const
{
    QStringList paths;
    if (personA == 0 || personB == 0 || personA == personB)
        return paths;
    if (!const_cast<FaceStore *>(this)->ensureOpen())
        return paths;

    const QString folder = root.isEmpty() ? QString() : Location::clean(root);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT DISTINCT a.uri FROM faces a "
        "JOIN faces b ON a.uri = b.uri "
        "WHERE a.person_id = ? AND b.person_id = ?"));
    q.addBindValue(personA);
    q.addBindValue(personB);
    if (!q.exec())
        return paths;
    while (q.next()) {
        const QString uri = q.value(0).toString();
        if (inScope(folder, uri))
            paths.append(uri);
    }
    paths.sort();
    return paths;
}

QStringList FaceStore::photosOfNamed() const
{
    QStringList paths;
    if (!const_cast<FaceStore *>(this)->ensureOpen())
        return paths;

    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "SELECT DISTINCT faces.uri FROM faces "
            "JOIN people ON people.id = faces.person_id "
            "WHERE people.name <> '' AND people.stranger = 0")))
        return paths;
    while (q.next())
        paths.append(q.value(0).toString());
    paths.sort();
    return paths;
}

QVector<FaceMark> FaceStore::facesFor(const QString &path) const
{
    QVector<FaceMark> marks;
    if (!const_cast<FaceStore *>(this)->ensureOpen())
        return marks;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT faces.id, faces.person_id, people.name, faces.x, faces.y, faces.w, faces.h, "
        "       faces.score, photos.width, photos.height, people.stranger "
        "FROM faces "
        "JOIN people ON people.id = faces.person_id "
        "JOIN photos ON photos.uri = faces.uri "
        "WHERE faces.uri = ?"));
    q.addBindValue(Location::clean(path));
    if (!q.exec())
        return marks;
    while (q.next()) {
        FaceMark mark;
        mark.faceId = q.value(0).toLongLong();
        mark.personId = q.value(1).toLongLong();
        mark.name = q.value(2).toString();
        mark.box = QRectF(q.value(3).toDouble(), q.value(4).toDouble(),
                          q.value(5).toDouble(), q.value(6).toDouble());
        mark.score = q.value(7).toFloat();
        mark.imageWidth = q.value(8).toInt();
        mark.imageHeight = q.value(9).toInt();
        mark.stranger = q.value(10).toInt() != 0;
        marks.append(mark);
    }
    return marks;
}

QVector<PersonFace> FaceStore::facesOfPerson(qint64 personId, const QString &root) const
{
    QVector<PersonFace> faces;
    if (!const_cast<FaceStore *>(this)->ensureOpen())
        return faces;

    const QString folder = root.isEmpty() ? QString() : Location::clean(root);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, crop_path, uri FROM faces WHERE person_id = ? ORDER BY score DESC, id"));
    q.addBindValue(personId);
    if (!q.exec())
        return faces;
    while (q.next()) {
        const QString uri = q.value(2).toString();
        if (!inScope(folder, uri))
            continue;
        PersonFace face;
        face.faceId = q.value(0).toLongLong();
        face.cropPath = q.value(1).toString();
        face.uri = uri;
        faces.append(face);
    }
    return faces;
}

bool FaceStore::personExists(qint64 personId) const
{
    if (personId == 0 || !const_cast<FaceStore *>(this)->ensureOpen())
        return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM people WHERE id = ?"));
    q.addBindValue(personId);
    return q.exec() && q.next();
}

void FaceStore::dropEmptyPeople()
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("DELETE FROM people WHERE id NOT IN (SELECT person_id FROM faces)"));
}

bool FaceStore::renamePerson(qint64 personId, const QString &name)
{
    if (!ensureOpen())
        return false;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return false;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE people SET name = ? WHERE id = ? AND stranger = 0"));
    q.addBindValue(trimmed);
    q.addBindValue(personId);
    if (!q.exec() || q.numRowsAffected() <= 0)
        return false;
    Q_EMIT changed();
    return true;
}

qint64 FaceStore::markAsStranger(qint64 personId)
{
    if (personId == 0 || !ensureOpen())
        return 0;

    QSqlQuery read(m_db);
    read.prepare(QStringLiteral("SELECT stranger FROM people WHERE id = ?"));
    read.addBindValue(personId);
    if (!read.exec() || !read.next())
        return 0;
    if (read.value(0).toInt() != 0)
        return personId;

    QSqlQuery find(m_db);
    if (!find.exec(QStringLiteral("SELECT id FROM people WHERE stranger = 1 LIMIT 1")))
        return 0;
    const qint64 bucket = find.next() ? find.value(0).toLongLong() : 0;

    if (!m_db.transaction())
        return 0;

    QSqlQuery q(m_db);
    if (bucket == 0) {
        q.prepare(QStringLiteral("UPDATE people SET stranger = 1, name = '' WHERE id = ?"));
        q.addBindValue(personId);
        if (!q.exec() || !m_db.commit()) {
            m_db.rollback();
            return 0;
        }
        Q_EMIT changed();
        return personId;
    }

    q.prepare(QStringLiteral("UPDATE faces SET person_id = ? WHERE person_id = ?"));
    q.addBindValue(bucket);
    q.addBindValue(personId);
    if (!q.exec()) {
        m_db.rollback();
        return 0;
    }
    q.prepare(QStringLiteral("DELETE FROM people WHERE id = ?"));
    q.addBindValue(personId);
    if (!q.exec() || !m_db.commit()) {
        m_db.rollback();
        return 0;
    }
    Q_EMIT changed();
    return bucket;
}

bool FaceStore::mergePeople(qint64 keepId, qint64 dropId)
{
    if (keepId == 0 || dropId == 0 || keepId == dropId || !ensureOpen())
        return false;

    QSqlQuery readKeep(m_db);
    readKeep.prepare(QStringLiteral("SELECT name FROM people WHERE id = ?"));
    readKeep.addBindValue(keepId);
    if (!readKeep.exec() || !readKeep.next())
        return false;
    const QString keepName = readKeep.value(0).toString();

    QSqlQuery readDrop(m_db);
    readDrop.prepare(QStringLiteral("SELECT name FROM people WHERE id = ?"));
    readDrop.addBindValue(dropId);
    if (!readDrop.exec() || !readDrop.next())
        return false;
    const QString dropName = readDrop.value(0).toString();

    if (!m_db.transaction())
        return false;

    QSqlQuery q(m_db);
    if (keepName.trimmed().isEmpty() && !dropName.trimmed().isEmpty()) {
        q.prepare(QStringLiteral("UPDATE people SET name = ? WHERE id = ?"));
        q.addBindValue(dropName.trimmed());
        q.addBindValue(keepId);
        if (!q.exec()) {
            m_db.rollback();
            return false;
        }
    }
    q.prepare(QStringLiteral("UPDATE faces SET person_id = ? WHERE person_id = ?"));
    q.addBindValue(keepId);
    q.addBindValue(dropId);
    if (!q.exec()) {
        m_db.rollback();
        return false;
    }
    q.prepare(QStringLiteral("DELETE FROM people WHERE id = ?"));
    q.addBindValue(dropId);
    if (!q.exec() || !m_db.commit()) {
        m_db.rollback();
        return false;
    }
    Q_EMIT changed();
    return true;
}

qint64 FaceStore::detachFace(qint64 faceId)
{
    if (faceId == 0 || !ensureOpen())
        return 0;

    QSqlQuery read(m_db);
    read.prepare(QStringLiteral("SELECT person_id FROM faces WHERE id = ?"));
    read.addBindValue(faceId);
    if (!read.exec() || !read.next())
        return 0;

    if (!m_db.transaction())
        return 0;

    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("INSERT INTO people (name) VALUES ('')"))) {
        m_db.rollback();
        return 0;
    }
    const qint64 created = q.lastInsertId().toLongLong();
    q.prepare(QStringLiteral("UPDATE faces SET person_id = ? WHERE id = ?"));
    q.addBindValue(created);
    q.addBindValue(faceId);
    if (!q.exec() || q.numRowsAffected() <= 0) {
        m_db.rollback();
        return 0;
    }
    dropEmptyPeople();
    if (!m_db.commit()) {
        m_db.rollback();
        return 0;
    }
    Q_EMIT changed();
    return created;
}

bool FaceStore::applyName(qint64 personId, const QString &name, const QList<qint64> &excludeFaceIds)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || personId == 0 || !ensureOpen())
        return false;

    QSqlQuery who(m_db);
    who.prepare(QStringLiteral("SELECT stranger FROM people WHERE id = ?"));
    who.addBindValue(personId);
    if (!who.exec() || !who.next() || who.value(0).toInt() != 0)
        return false;

    QSqlQuery read(m_db);
    read.prepare(QStringLiteral("SELECT id FROM faces WHERE person_id = ?"));
    read.addBindValue(personId);
    if (!read.exec())
        return false;
    QSet<qint64> owned;
    while (read.next())
        owned.insert(read.value(0).toLongLong());
    if (owned.isEmpty())
        return false;

    QList<qint64> moving;
    for (qint64 faceId : excludeFaceIds) {
        if (owned.contains(faceId))
            moving.append(faceId);
    }
    if (moving.size() >= owned.size())
        return false;

    if (!m_db.transaction())
        return false;

    QSqlQuery q(m_db);
    if (!moving.isEmpty()) {
        if (!q.exec(QStringLiteral("INSERT INTO people (name) VALUES ('')"))) {
            m_db.rollback();
            return false;
        }
        const qint64 created = q.lastInsertId().toLongLong();
        for (qint64 faceId : moving) {
            QSqlQuery move(m_db);
            move.prepare(QStringLiteral(
                "UPDATE faces SET person_id = ? WHERE id = ? AND person_id = ?"));
            move.addBindValue(created);
            move.addBindValue(faceId);
            move.addBindValue(personId);
            if (!move.exec()) {
                m_db.rollback();
                return false;
            }
        }
    }

    q.prepare(QStringLiteral("UPDATE people SET name = ? WHERE id = ?"));
    q.addBindValue(trimmed);
    q.addBindValue(personId);
    if (!q.exec() || !m_db.commit()) {
        m_db.rollback();
        return false;
    }
    Q_EMIT changed();
    return true;
}

int FaceStore::forgetAbsent(const QString &root, const QStringList &present)
{
    if (root.isEmpty() || !ensureOpen())
        return 0;

    const QString folder = Location::clean(root);
    QSet<QString> keep;
    for (const QString &path : present)
        keep.insert(Location::clean(path));

    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT uri FROM photos")))
        return 0;
    QStringList missing;
    while (q.next()) {
        const QString uri = q.value(0).toString();
        if (!underRoot(folder, uri) || keep.contains(uri))
            continue;
        if (QFileInfo::exists(uri))
            continue;
        missing.append(uri);
    }
    if (missing.isEmpty())
        return 0;

    QStringList crops;
    for (const QString &uri : missing) {
        QSqlQuery crop(m_db);
        crop.prepare(QStringLiteral("SELECT crop_path FROM faces WHERE uri = ? AND crop_path <> ''"));
        crop.addBindValue(uri);
        if (!crop.exec())
            continue;
        while (crop.next())
            crops.append(crop.value(0).toString());
    }

    if (!m_db.transaction())
        return 0;
    for (const QString &uri : missing) {
        QSqlQuery drop(m_db);
        drop.prepare(QStringLiteral("DELETE FROM photos WHERE uri = ?"));
        drop.addBindValue(uri);
        if (!drop.exec()) {
            m_db.rollback();
            return 0;
        }
    }
    dropEmptyPeople();
    if (!m_db.commit()) {
        m_db.rollback();
        return 0;
    }
    for (const QString &path : crops)
        QFile::remove(path);
    Q_EMIT changed();
    return int(missing.size());
}

int FaceStore::forgetMissingFiles()
{
    if (!ensureOpen())
        return 0;

    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT uri FROM photos")))
        return 0;
    QStringList missing;
    while (q.next()) {
        const QString uri = q.value(0).toString();
        if (!QFileInfo::exists(uri))
            missing.append(uri);
    }
    if (missing.isEmpty())
        return 0;

    QStringList crops;
    for (const QString &uri : missing) {
        QSqlQuery crop(m_db);
        crop.prepare(QStringLiteral("SELECT crop_path FROM faces WHERE uri = ? AND crop_path <> ''"));
        crop.addBindValue(uri);
        if (!crop.exec())
            continue;
        while (crop.next())
            crops.append(crop.value(0).toString());
    }

    if (!m_db.transaction())
        return 0;
    for (const QString &uri : missing) {
        QSqlQuery drop(m_db);
        drop.prepare(QStringLiteral("DELETE FROM photos WHERE uri = ?"));
        drop.addBindValue(uri);
        if (!drop.exec()) {
            m_db.rollback();
            return 0;
        }
    }
    dropEmptyPeople();
    if (!m_db.commit()) {
        m_db.rollback();
        return 0;
    }
    for (const QString &path : crops)
        QFile::remove(path);
    Q_EMIT changed();
    return int(missing.size());
}

void FaceStore::clear()
{
    if (!ensureOpen())
        return;

    QSqlQuery q(m_db);
    q.exec(QStringLiteral("DELETE FROM faces"));
    q.exec(QStringLiteral("DELETE FROM people"));
    q.exec(QStringLiteral("DELETE FROM photos"));

    const QFileInfoList crops = QDir(m_cropDir).entryInfoList(QDir::Files);
    for (const QFileInfo &crop : crops)
        QFile::remove(crop.absoluteFilePath());

    Q_EMIT changed();
}
