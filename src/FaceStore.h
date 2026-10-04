#pragma once

#include "FaceTypes.h"

#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QStringList>

// Local face index. Embeddings and names live in one SQLite file under the
// user's data directory (OMANTA_FACES_DB overrides it for tests). Photos are
// remembered by path, size and mtime, the same invalidation the thumbnail
// cache uses: an unchanged file is not detected again.
//
// Assignment happens at insert. A new face joins the nearest stored person
// at or above kSamePersonCosine, including faces this same file already had,
// so a re-save keeps a name. Two faces in one newly seen photo do not match
// each other — a group photo should not collapse into one person.
class FaceStore : public QObject
{
    Q_OBJECT

public:
    explicit FaceStore(QObject *parent = nullptr);
    ~FaceStore() override;

    bool isOpen() const;

    static float cosine(const QVector<float> &a, const QVector<float> &b);

    bool isCurrent(const QString &path, qint64 size, qint64 mtimeMs);

    // Replaces every face of this photo. An empty list still marks the file
    // current, so a picture with no face is not detected again until it changes.
    void replacePhoto(const QString &path, qint64 size, qint64 mtimeMs,
                      int width, int height, const QVector<FaceSample> &faces);

    QVector<PersonInfo> peopleUnder(const QString &root) const;
    // Named people from every indexed folder. The People place lists these.
    QVector<PersonInfo> namedPeople() const;
    // An empty root means every indexed folder. A set root stays inside it.
    QStringList photosForPerson(qint64 personId, const QString &root) const;
    QStringList photosWithBoth(qint64 personA, qint64 personB, const QString &root) const;
    QStringList photosOfNamed() const;
    QVector<FaceMark> facesFor(const QString &path) const;
    QVector<PersonFace> facesOfPerson(qint64 personId, const QString &root) const;
    bool personExists(qint64 personId) const;

    bool renamePerson(qint64 personId, const QString &name);
    // Moves this person into the one stranger bucket. Later faces that match
    // anyone in that bucket join it. Returns the bucket id, or 0 on failure.
    qint64 markAsStranger(qint64 personId);
    // Keeps `keepId`. If that person has no name and the other does, the name
    // moves across. Otherwise the keeper's name wins. Faces follow the keeper.
    bool mergePeople(qint64 keepId, qint64 dropId);
    // Moves one face onto a new unnamed person. The rest of the group stay.
    qint64 detachFace(qint64 faceId);
    // Names `personId`. Faces in `excludeFaceIds` leave together as one new
    // unnamed person, so a mistake does not become several people. Refuses
    // when every face is excluded or the name is empty.
    bool applyName(qint64 personId, const QString &name, const QList<qint64> &excludeFaceIds);
    // Drops photos under `root` whose files are gone. A file that still exists
    // is kept even when this walk did not list it (hidden, or past the cap).
    int forgetAbsent(const QString &root, const QStringList &present);
    int forgetMissingFiles();
    void clear();

Q_SIGNALS:
    void changed();

private:
    struct Candidate {
        qint64 personId = 0;
        QVector<float> embedding;
    };

    bool ensureOpen();
    QVector<Candidate> loadCandidates() const;
    QVector<PersonInfo> collectPeople(const QString &root, bool namedOnly) const;
    bool ensureStrangerColumn();
    void dropEmptyPeople();
    QString databasePath() const;
    QString cropDirectory() const;

    QString m_connection;
    QSqlDatabase m_db;
    QString m_cropDir;
    mutable bool m_openTried = false;
};
