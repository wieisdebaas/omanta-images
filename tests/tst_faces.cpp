#include "FaceEngine.h"
#include "FaceIndexer.h"
#include "FacePhotoFilter.h"
#include "FacePhotosModel.h"
#include "FaceStore.h"
#include "FaceTypes.h"

#include "DirectoryModel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

// Face index: embeddings cluster into people, names survive a re-read, and
// a photo is not indexed again until its size or mtime changes. The database
// is a temp file — this suite must never touch the real faces index.
class TestFaces : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();

    void cosineOfIdenticalVectorsIsOne();
    void cosineOfOrthogonalVectorsIsZero();
    void sameEmbeddingJoinsOnePerson();
    void differentEmbeddingsStaySeparatePeople();
    void renamePersistsAcrossAReopen();
    void reindexKeepsTheNameWhenTheFaceStillMatches();
    void aChangedFileIsNoLongerCurrent();
    void peopleStayInsideTheirFolder();
    void clearDropsPeopleAndCrops();
    void filterHidesPhotosOfOtherPeople();
    void mergeKeepsTheSurvivingPersonAndName();
    void mergeTakesANameTheKeeperDidNotHave();
    void detachMovesOneFaceToANewPerson();
    void namingCanLeaveAMistakeOut();
    void aMissingFileDropsItsFaces();
    void photosWithBothPeople();
    void namedPeopleSkipAnyoneWithoutAName();
    void strangersShareOneTileAndStayOutOfPeople();
    void photoListSkipsAMissingFile();
    void detectsTheSamplePortrait();
    void indexesAFolderOfOnePortrait();
    void pauseHoldsTheScanUntilReleased();

private:
    QTemporaryDir m_dir;

    QVector<float> axis(int i) const
    {
        QVector<float> v(FaceEmbeddingLength, 0.f);
        v[i] = 1.f;
        return v;
    }

    // cosine(axis(0), this) is 1/sqrt(2) ≈ 0.707, above the same-person cut.
    QVector<float> nearAxis0() const
    {
        QVector<float> v = axis(0);
        v[1] = 1.f;
        return v;
    }

    FaceSample sample(const QVector<float> &embedding, float score = 0.9f) const
    {
        FaceSample face;
        face.box = QRectF(10, 20, 40, 50);
        face.score = score;
        face.embedding = embedding;
        face.crop = QImage(8, 8, QImage::Format_RGB32);
        face.crop.fill(Qt::red);
        return face;
    }

    QString photo(const QString &name) const
    {
        return QDir(m_dir.path()).filePath(name);
    }
};

void TestFaces::init()
{
    QVERIFY(m_dir.isValid());
    const QString db = m_dir.filePath(QStringLiteral("faces-%1.sqlite")
                                          .arg(QTest::currentTestFunction()));
    QFile::remove(db);
    qputenv("OMANTA_FACES_DB", db.toUtf8());
    qputenv("OMANTA_FACES_CROP_DIR",
            m_dir.filePath(QStringLiteral("crops-%1").arg(QTest::currentTestFunction())).toUtf8());
}

void TestFaces::cosineOfIdenticalVectorsIsOne()
{
    QCOMPARE(FaceStore::cosine(axis(0), axis(0)), 1.f);
}

void TestFaces::cosineOfOrthogonalVectorsIsZero()
{
    // QCOMPARE's float check rejects exact zeros, so the distance is asserted
    // directly.
    QVERIFY(std::abs(FaceStore::cosine(axis(0), axis(1))) < 1e-6);
}

void TestFaces::sameEmbeddingJoinsOnePerson()
{
    FaceStore store;
    store.replacePhoto(photo("a.jpg"), 10, 1000, 100, 80, { sample(axis(0)) });
    store.replacePhoto(photo("b.jpg"), 11, 1001, 100, 80, { sample(axis(0)) });

    const QVector<PersonInfo> people = store.peopleUnder(m_dir.path());
    QCOMPARE(people.size(), 1);
    QCOMPARE(people.first().photoCount, 2);
    QCOMPARE(store.photosForPerson(people.first().id, m_dir.path()).size(), 2);
}

void TestFaces::differentEmbeddingsStaySeparatePeople()
{
    FaceStore store;
    store.replacePhoto(photo("a.jpg"), 10, 1000, 100, 80, { sample(axis(0)), sample(axis(3), 0.4f) });

    const QVector<PersonInfo> people = store.peopleUnder(m_dir.path());
    QCOMPARE(people.size(), 2);
    // The stronger face is the cover, so the first chip is the one the user sees.
    QVERIFY(people.first().score >= people.last().score);
}

void TestFaces::renamePersistsAcrossAReopen()
{
    qint64 id = 0;
    {
        FaceStore store;
        store.replacePhoto(photo("a.jpg"), 10, 1000, 100, 80, { sample(axis(0)) });
        id = store.peopleUnder(m_dir.path()).first().id;
        QVERIFY(store.renamePerson(id, QStringLiteral("Robin")));
    }

    FaceStore store;
    const QVector<PersonInfo> people = store.peopleUnder(m_dir.path());
    QCOMPARE(people.size(), 1);
    QCOMPARE(people.first().id, id);
    QCOMPARE(people.first().name, QStringLiteral("Robin"));
    QVERIFY(!people.first().cropPath.isEmpty());
    QVERIFY(QFileInfo::exists(people.first().cropPath));

    const QVector<FaceMark> marks = store.facesFor(photo("a.jpg"));
    QCOMPARE(marks.size(), 1);
    QCOMPARE(marks.first().personId, id);
    QCOMPARE(marks.first().name, QStringLiteral("Robin"));
    QCOMPARE(marks.first().imageWidth, 100);
    QCOMPARE(marks.first().imageHeight, 80);
    QCOMPARE(marks.first().box, QRectF(10, 20, 40, 50));
}

void TestFaces::reindexKeepsTheNameWhenTheFaceStillMatches()
{
    FaceStore store;
    store.replacePhoto(photo("a.jpg"), 10, 1000, 100, 80, { sample(axis(0)) });
    const qint64 id = store.peopleUnder(m_dir.path()).first().id;
    QVERIFY(store.renamePerson(id, QStringLiteral("Robin")));

    store.replacePhoto(photo("a.jpg"), 12, 2000, 100, 80, { sample(nearAxis0()) });

    const QVector<PersonInfo> people = store.peopleUnder(m_dir.path());
    QCOMPARE(people.size(), 1);
    QCOMPARE(people.first().id, id);
    QCOMPARE(people.first().name, QStringLiteral("Robin"));
    QVERIFY(store.isCurrent(photo("a.jpg"), 12, 2000));
    QVERIFY(!store.isCurrent(photo("a.jpg"), 10, 1000));
}

void TestFaces::aChangedFileIsNoLongerCurrent()
{
    FaceStore store;
    QVERIFY(!store.isCurrent(photo("a.jpg"), 10, 1000));
    store.replacePhoto(photo("a.jpg"), 10, 1000, 100, 80, {});
    QVERIFY(store.isCurrent(photo("a.jpg"), 10, 1000));
    QVERIFY(!store.isCurrent(photo("a.jpg"), 10, 1001));
    QCOMPARE(store.peopleUnder(m_dir.path()).size(), 0);
}

void TestFaces::peopleStayInsideTheirFolder()
{
    FaceStore store;
    const QString album = m_dir.filePath(QStringLiteral("album"));
    const QString other = m_dir.filePath(QStringLiteral("album-other"));
    QDir().mkpath(album);
    QDir().mkpath(other);
    store.replacePhoto(album + QStringLiteral("/a.jpg"), 10, 1000, 100, 80, { sample(axis(0)) });
    store.replacePhoto(other + QStringLiteral("/b.jpg"), 10, 1000, 100, 80, { sample(axis(1)) });

    QCOMPARE(store.peopleUnder(album).size(), 1);
    QCOMPARE(store.photosForPerson(store.peopleUnder(album).first().id, album).size(), 1);
    QCOMPARE(store.peopleUnder(album + QStringLiteral("-other")).size(), 1);
}

void TestFaces::clearDropsPeopleAndCrops()
{
    FaceStore store;
    store.replacePhoto(photo("a.jpg"), 10, 1000, 100, 80, { sample(axis(0)) });
    const QString crop = store.peopleUnder(m_dir.path()).first().cropPath;
    QVERIFY(QFileInfo::exists(crop));

    store.clear();
    QCOMPARE(store.peopleUnder(m_dir.path()).size(), 0);
    QVERIFY(!store.isCurrent(photo("a.jpg"), 10, 1000));
    QVERIFY(!QFileInfo::exists(crop));
}

void TestFaces::filterHidesPhotosOfOtherPeople()
{
    auto *source = new QStandardItemModel(this);
    source->setItemRoleNames({
        { DirectoryModel::NameRole, "name" },
        { DirectoryModel::DisplayNameRole, "displayName" },
        { DirectoryModel::FilePathRole, "filePath" },
    });
    const QString robin = photo("robin.jpg");
    const QString sam = photo("sam.jpg");
    for (const QString &path : { robin, sam }) {
        auto *item = new QStandardItem;
        item->setData(QFileInfo(path).fileName(), DirectoryModel::NameRole);
        item->setData(QFileInfo(path).fileName(), DirectoryModel::DisplayNameRole);
        item->setData(path, DirectoryModel::FilePathRole);
        source->appendRow(item);
    }

    FacePhotoFilter filter;
    filter.setSourceModel(source);
    QCOMPARE(filter.count(), 2);
    QCOMPARE(filter.proxyRowForName(QStringLiteral("sam.jpg")), 1);

    filter.setFiltering(true);
    filter.setPaths({ robin });
    QCOMPARE(filter.count(), 1);
    QCOMPARE(filter.valueAt(0, QStringLiteral("filePath")).toString(), robin);
    QCOMPARE(filter.proxyRowForName(QStringLiteral("sam.jpg")), -1);
    QCOMPARE(filter.findByPrefix(QStringLiteral("rob"), 0), 0);

    filter.setFiltering(false);
    QCOMPARE(filter.count(), 2);
}

void TestFaces::mergeKeepsTheSurvivingPersonAndName()
{
    FaceStore store;
    store.replacePhoto(photo(QStringLiteral("a.jpg")), 10, 1000, 100, 80, { sample(axis(0)) });
    store.replacePhoto(photo(QStringLiteral("b.jpg")), 11, 1001, 100, 80, { sample(axis(1)) });
    const qint64 keep = store.facesFor(photo(QStringLiteral("a.jpg"))).first().personId;
    const qint64 drop = store.facesFor(photo(QStringLiteral("b.jpg"))).first().personId;
    QVERIFY(store.renamePerson(keep, QStringLiteral("Robin")));
    QVERIFY(store.renamePerson(drop, QStringLiteral("Sam")));

    QVERIFY(store.mergePeople(keep, drop));

    const QVector<PersonInfo> people = store.peopleUnder(m_dir.path());
    QCOMPARE(people.size(), 1);
    QCOMPARE(people.first().id, keep);
    QCOMPARE(people.first().name, QStringLiteral("Robin"));
    QCOMPARE(people.first().photoCount, 2);
    QVERIFY(!store.personExists(drop));
}

void TestFaces::mergeTakesANameTheKeeperDidNotHave()
{
    FaceStore store;
    store.replacePhoto(photo(QStringLiteral("a.jpg")), 10, 1000, 100, 80, { sample(axis(0)) });
    store.replacePhoto(photo(QStringLiteral("b.jpg")), 11, 1001, 100, 80, { sample(axis(1)) });
    const qint64 keep = store.facesFor(photo(QStringLiteral("a.jpg"))).first().personId;
    const qint64 drop = store.facesFor(photo(QStringLiteral("b.jpg"))).first().personId;
    QVERIFY(store.renamePerson(drop, QStringLiteral("Robin")));

    QVERIFY(store.mergePeople(keep, drop));

    const QVector<PersonInfo> people = store.peopleUnder(m_dir.path());
    QCOMPARE(people.size(), 1);
    QCOMPARE(people.first().id, keep);
    QCOMPARE(people.first().name, QStringLiteral("Robin"));
}

void TestFaces::detachMovesOneFaceToANewPerson()
{
    FaceStore store;
    store.replacePhoto(photo(QStringLiteral("a.jpg")), 10, 1000, 100, 80, { sample(axis(0)) });
    store.replacePhoto(photo(QStringLiteral("b.jpg")), 11, 1001, 100, 80, { sample(axis(0)) });
    const qint64 id = store.peopleUnder(m_dir.path()).first().id;
    QVERIFY(store.renamePerson(id, QStringLiteral("Robin")));
    const qint64 faceId = store.facesFor(photo(QStringLiteral("b.jpg"))).first().faceId;

    const qint64 created = store.detachFace(faceId);
    QVERIFY(created != 0);
    QVERIFY(created != id);

    const QVector<PersonInfo> people = store.peopleUnder(m_dir.path());
    QCOMPARE(people.size(), 2);
    const PersonInfo *named = nullptr;
    const PersonInfo *split = nullptr;
    for (const PersonInfo &person : people) {
        if (person.id == id)
            named = &person;
        if (person.id == created)
            split = &person;
    }
    QVERIFY(named);
    QVERIFY(split);
    QCOMPARE(named->name, QStringLiteral("Robin"));
    QCOMPARE(named->photoCount, 1);
    QCOMPARE(split->name, QString());
    QCOMPARE(split->photoCount, 1);
    QCOMPARE(store.facesFor(photo(QStringLiteral("b.jpg"))).first().personId, created);
}

void TestFaces::namingCanLeaveAMistakeOut()
{
    FaceStore store;
    store.replacePhoto(photo(QStringLiteral("a.jpg")), 10, 1000, 100, 80, { sample(axis(0)) });
    store.replacePhoto(photo(QStringLiteral("b.jpg")), 11, 1001, 100, 80, { sample(axis(0)) });
    const qint64 id = store.peopleUnder(m_dir.path()).first().id;
    const qint64 mistake = store.facesFor(photo(QStringLiteral("b.jpg"))).first().faceId;

    QVERIFY(store.applyName(id, QStringLiteral("Robin"), { mistake }));
    QVERIFY(!store.applyName(id, QStringLiteral("Robin"), {
        store.facesFor(photo(QStringLiteral("a.jpg"))).first().faceId, mistake }));

    const QVector<PersonInfo> people = store.peopleUnder(m_dir.path());
    QCOMPARE(people.size(), 2);
    QStringList namedPhotos;
    int unnamed = 0;
    for (const PersonInfo &person : people) {
        if (person.name == QLatin1String("Robin"))
            namedPhotos = store.photosForPerson(person.id, m_dir.path());
        else
            ++unnamed;
    }
    QCOMPARE(unnamed, 1);
    QCOMPARE(namedPhotos, QStringList({ photo(QStringLiteral("a.jpg")) }));
    QCOMPARE(store.facesFor(photo(QStringLiteral("b.jpg"))).first().name, QString());
}

void TestFaces::aMissingFileDropsItsFaces()
{
    const QString album = m_dir.filePath(QStringLiteral("album"));
    const QString other = m_dir.filePath(QStringLiteral("other"));
    QVERIFY(QDir().mkpath(album));
    QVERIFY(QDir().mkpath(other));
    const QString kept = album + QStringLiteral("/a.jpg");
    const QString gone = album + QStringLiteral("/gone.jpg");
    const QString elsewhere = other + QStringLiteral("/b.jpg");
    QFile file(kept);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
    file.close();

    FaceStore store;
    store.replacePhoto(kept, 10, 1000, 100, 80, { sample(axis(0)) });
    store.replacePhoto(gone, 11, 1001, 100, 80, { sample(axis(1)) });
    store.replacePhoto(elsewhere, 12, 1002, 100, 80, { sample(axis(2)) });
    QStringList crops;
    for (const PersonInfo &person : store.peopleUnder(m_dir.path()))
        crops.append(person.cropPath);

    QCOMPARE(store.forgetAbsent(album, {}), 1);
    QCOMPARE(store.peopleUnder(album).size(), 1);
    QCOMPARE(store.photosForPerson(store.peopleUnder(album).first().id, album),
             QStringList({ kept }));
    QCOMPARE(store.peopleUnder(other).size(), 1);

    int cropsLeft = 0;
    for (const QString &crop : crops) {
        if (QFileInfo::exists(crop))
            ++cropsLeft;
    }
    QCOMPARE(cropsLeft, 2);
}

void TestFaces::photosWithBothPeople()
{
    FaceStore store;
    store.replacePhoto(photo(QStringLiteral("group.jpg")), 10, 1000, 100, 80,
                       { sample(axis(0), 0.9f), sample(axis(1), 0.4f) });
    store.replacePhoto(photo(QStringLiteral("solo.jpg")), 11, 1001, 100, 80, { sample(axis(0)) });
    const QVector<FaceMark> marks = store.facesFor(photo(QStringLiteral("group.jpg")));
    QCOMPARE(marks.size(), 2);
    const qint64 shared = store.facesFor(photo(QStringLiteral("solo.jpg"))).first().personId;
    const qint64 other = marks.at(0).personId == shared ? marks.at(1).personId
                                                       : marks.at(0).personId;

    QCOMPARE(store.photosWithBoth(shared, other, m_dir.path()),
             QStringList({ photo(QStringLiteral("group.jpg")) }));
    QCOMPARE(store.photosForPerson(shared, QString()).size(), 2);
}

void TestFaces::namedPeopleSkipAnyoneWithoutAName()
{
    FaceStore store;
    store.replacePhoto(photo(QStringLiteral("a.jpg")), 10, 1000, 100, 80, { sample(axis(0)) });
    store.replacePhoto(photo(QStringLiteral("b.jpg")), 11, 1001, 100, 80, { sample(axis(1)) });
    const qint64 named = store.facesFor(photo(QStringLiteral("a.jpg"))).first().personId;
    QVERIFY(store.renamePerson(named, QStringLiteral("Robin")));

    const QVector<PersonInfo> people = store.namedPeople();
    QCOMPARE(people.size(), 1);
    QCOMPARE(people.first().name, QStringLiteral("Robin"));
    QCOMPARE(store.photosOfNamed(), QStringList({ photo(QStringLiteral("a.jpg")) }));
}

void TestFaces::strangersShareOneTileAndStayOutOfPeople()
{
    FaceStore store;
    store.replacePhoto(photo(QStringLiteral("a.jpg")), 10, 1000, 100, 80, { sample(axis(0), 0.4f) });
    store.replacePhoto(photo(QStringLiteral("b.jpg")), 11, 1001, 100, 80, { sample(axis(1), 0.95f) });
    const qint64 first = store.facesFor(photo(QStringLiteral("a.jpg"))).first().personId;
    const qint64 second = store.facesFor(photo(QStringLiteral("b.jpg"))).first().personId;
    QVERIFY(store.renamePerson(first, QStringLiteral("Robin")));

    const qint64 bucket = store.markAsStranger(first);
    QCOMPARE(bucket, first);
    QCOMPARE(store.markAsStranger(second), bucket);
    QCOMPARE(store.markAsStranger(bucket), bucket);
    QVERIFY(!store.renamePerson(bucket, QStringLiteral("Robin")));

    const QVector<PersonInfo> people = store.peopleUnder(m_dir.path());
    QCOMPARE(people.size(), 1);
    QVERIFY(people.first().stranger);
    QCOMPARE(people.first().name, QString());
    QCOMPARE(people.first().photoCount, 2);
    QCOMPARE(store.namedPeople().size(), 0);
    QCOMPARE(store.photosOfNamed().size(), 0);

    store.replacePhoto(photo(QStringLiteral("c.jpg")), 12, 1002, 100, 80, { sample(nearAxis0(), 0.2f) });
    store.replacePhoto(photo(QStringLiteral("d.jpg")), 13, 1003, 100, 80, { sample(axis(3), 0.99f) });

    const QVector<PersonInfo> after = store.peopleUnder(m_dir.path());
    QCOMPARE(after.size(), 2);
    QVERIFY(after.first().stranger);
    QCOMPARE(after.first().id, bucket);
    QCOMPARE(after.first().photoCount, 3);
    QVERIFY(!after.last().stranger);

    const qint64 faceId = store.facesFor(photo(QStringLiteral("c.jpg"))).first().faceId;
    QVERIFY(store.facesFor(photo(QStringLiteral("c.jpg"))).first().stranger);
    const qint64 split = store.detachFace(faceId);
    QVERIFY(split != 0);
    QVERIFY(split != bucket);

    const QVector<PersonInfo> splitPeople = store.peopleUnder(m_dir.path());
    QVERIFY(splitPeople.first().stranger);
    bool sawSplit = false;
    for (const PersonInfo &person : splitPeople) {
        if (person.id != split)
            continue;
        sawSplit = true;
        QVERIFY(!person.stranger);
        QCOMPARE(person.photoCount, 1);
    }
    QVERIFY(sawSplit);
}

void TestFaces::photoListSkipsAMissingFile()
{
    const QString kept = photo(QStringLiteral("kept.jpg"));
    QFile file(kept);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
    file.close();

    FacePhotosModel model;
    model.setPaths({ kept, photo(QStringLiteral("missing.jpg")) });
    QCOMPARE(model.count(), 1);
    QCOMPARE(model.data(model.index(0, 0), DirectoryModel::NameRole).toString(),
             QFileInfo(kept).absoluteFilePath());
    QCOMPARE(model.data(model.index(0, 0), DirectoryModel::IsDirRole).toBool(), false);
    QVERIFY(!model.data(model.index(0, 0), DirectoryModel::ContentTypeRole).toString().isEmpty());
}

void TestFaces::detectsTheSamplePortrait()
{
    const QString image = QStringLiteral(OMANTA_FACE_FIXTURE);
    QVERIFY2(QFileInfo::exists(image), qPrintable(image));

    FaceEngine engine;
    QString error;
    QVERIFY2(engine.load(&error), qPrintable(error));
    const FaceEngine::Result result = engine.detect(image, &error);
    QVERIFY2(result.decoded, qPrintable(error));
    QVERIFY2(!result.faces.isEmpty(), "the sample portrait should contain a face");
    QCOMPARE(result.faces.first().embedding.size(), FaceEmbeddingLength);
    QVERIFY(result.faces.first().box.width() > 1);
    QVERIFY(!result.faces.first().crop.isNull());
}

void TestFaces::indexesAFolderOfOnePortrait()
{
    const QString image = QStringLiteral(OMANTA_FACE_FIXTURE);
    const QString album = m_dir.filePath(QStringLiteral("scan-album"));
    QDir(album).removeRecursively();
    QVERIFY(QDir().mkpath(album));
    QVERIFY(QFile::copy(image, album + QStringLiteral("/face.jpg")));

    FaceStore store;
    FaceIndexer indexer;
    QSignalSpy done(&indexer, &FaceIndexer::finished);
    indexer.start(&store, album, false);
    QVERIFY(done.wait(60000));

    const QVector<PersonInfo> people = store.peopleUnder(album);
    QCOMPARE(people.size(), 1);
    QCOMPARE(people.first().photoCount, 1);
    QVERIFY(store.isCurrent(album + QStringLiteral("/face.jpg"),
                            QFileInfo(album + QStringLiteral("/face.jpg")).size(),
                            QFileInfo(album + QStringLiteral("/face.jpg")).lastModified().toMSecsSinceEpoch()));
}

void TestFaces::pauseHoldsTheScanUntilReleased()
{
    const QString image = QStringLiteral(OMANTA_FACE_FIXTURE);
    const QString album = m_dir.filePath(QStringLiteral("pause-album"));
    QVERIFY(QDir().mkpath(album));
    QVERIFY(QFile::copy(image, album + QStringLiteral("/face.jpg")));

    FaceStore store;
    FaceIndexer indexer;
    indexer.setHeld(true);
    QSignalSpy done(&indexer, &FaceIndexer::finished);
    indexer.start(&store, album, false);
    QVERIFY(indexer.indexing());
    QVERIFY(!done.wait(400));
    QVERIFY(indexer.indexing());

    indexer.setHeld(false);
    QVERIFY(done.wait(60000));
    QCOMPARE(store.peopleUnder(album).size(), 1);
}

QTEST_GUILESS_MAIN(TestFaces)
#include "tst_faces.moc"
