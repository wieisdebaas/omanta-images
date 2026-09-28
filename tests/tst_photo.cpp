#include "PhotoModel.h"
#include "DirectoryModel.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

class TestPhoto : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void discoversNestedImagesWithRelativePaths();
    void excludesNonImages();
    void skipsHiddenUnlessShown();
    void doesNotFollowSymlinkDirectories();
    void changingPathCancelsPreviousWalk();
    void inactiveModelDoesNotScan();

private:
    static void writeImage(const QString &path);
    static void writeText(const QString &path);
};

void TestPhoto::writeImage(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(path, "PNG"));
}

void TestPhoto::writeText(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("notes");
}

void TestPhoto::discoversNestedImagesWithRelativePaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeImage(dir.filePath(QStringLiteral("root.png")));
    writeImage(dir.filePath(QStringLiteral("Jan/a.jpg")));
    writeImage(dir.filePath(QStringLiteral("Jan/Feb/b.jpg")));
    writeText(dir.filePath(QStringLiteral("readme.txt")));

    PhotoModel model;
    model.setPath(dir.path());
    model.setActive(true);

    QTRY_VERIFY_WITH_TIMEOUT(!model.scanning() && model.count() == 3, 10000);

    QStringList names;
    int janRow = -1;
    for (int row = 0; row < model.count(); ++row) {
        const QString name = model.data(model.index(row), DirectoryModel::NameRole).toString();
        names << name;
        if (name == QLatin1String("Jan/a.jpg"))
            janRow = row;
    }
    names.sort();
    QCOMPARE(names, (QStringList{ QStringLiteral("Jan/Feb/b.jpg"),
                                  QStringLiteral("Jan/a.jpg"),
                                  QStringLiteral("root.png") }));
    QVERIFY(janRow >= 0);
    QCOMPARE(model.data(model.index(janRow), DirectoryModel::FilePathRole).toString(),
             dir.filePath(QStringLiteral("Jan/a.jpg")));
}

void TestPhoto::excludesNonImages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeImage(dir.filePath(QStringLiteral("pic.png")));
    writeText(dir.filePath(QStringLiteral("doc.txt")));
    QDir().mkpath(dir.filePath(QStringLiteral("album")));

    PhotoModel model;
    model.setPath(dir.path());
    model.setActive(true);

    QTRY_VERIFY_WITH_TIMEOUT(!model.scanning() && model.count() == 1, 10000);
    QCOMPARE(model.data(model.index(0), DirectoryModel::NameRole).toString(),
             QStringLiteral("pic.png"));
}

void TestPhoto::skipsHiddenUnlessShown()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeImage(dir.filePath(QStringLiteral("visible.png")));
    writeImage(dir.filePath(QStringLiteral(".secret/hidden.png")));

    PhotoModel model;
    model.setPath(dir.path());
    model.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(!model.scanning(), 10000);
    QCOMPARE(model.count(), 1);

    model.setShowHidden(true);
    QTRY_VERIFY_WITH_TIMEOUT(!model.scanning() && model.count() == 2, 10000);
}

void TestPhoto::doesNotFollowSymlinkDirectories()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeImage(dir.filePath(QStringLiteral("only.png")));
    QVERIFY(::symlink(dir.path().toUtf8().constData(),
                      dir.filePath(QStringLiteral("loop")).toUtf8().constData()) == 0);

    PhotoModel model;
    model.setPath(dir.path());
    model.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(!model.scanning() && model.count() == 1, 10000);
}

void TestPhoto::changingPathCancelsPreviousWalk()
{
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    writeImage(first.filePath(QStringLiteral("a/one.png")));
    writeImage(second.filePath(QStringLiteral("two.png")));

    PhotoModel model;
    model.setPath(first.path());
    model.setActive(true);
    model.setPath(second.path());

    QTRY_VERIFY_WITH_TIMEOUT(!model.scanning() && model.count() == 1, 10000);
    QCOMPARE(model.data(model.index(0), DirectoryModel::NameRole).toString(),
             QStringLiteral("two.png"));
}

void TestPhoto::inactiveModelDoesNotScan()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeImage(dir.filePath(QStringLiteral("pic.png")));

    PhotoModel model;
    model.setPath(dir.path());
    QCOMPARE(model.count(), 0);
    QVERIFY(!model.scanning());

    model.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(!model.scanning() && model.count() == 1, 10000);

    model.setActive(false);
    QVERIFY(!model.scanning());
}

QTEST_GUILESS_MAIN(TestPhoto)
#include "tst_photo.moc"
