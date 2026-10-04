#pragma once

#include "FaceIndexer.h"
#include "FaceStore.h"
#include "PeopleModel.h"

#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration>

// The photo view's face layer, and the People place. The toolbar switch
// decides whether a folder is being indexed. The People place only reads
// people who already have a name.
class Faces : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(PeopleModel *people READ people CONSTANT)
    Q_PROPERTY(bool indexing READ indexing NOTIFY statusChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY statusChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(qint64 selectedPersonId READ selectedPersonId NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList selectedPersonIds READ selectedPersonIds NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedLabel READ selectedLabel NOTIFY selectionChanged)
    Q_PROPERTY(QStringList selectedPaths READ selectedPaths NOTIFY selectionChanged)
    Q_PROPERTY(QStringList libraryPaths READ libraryPaths NOTIFY selectionChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

public:
    explicit Faces(QObject *parent = nullptr);
    ~Faces() override;

    PeopleModel *people() { return &m_people; }

    bool indexing() const { return m_indexer.indexing(); }
    bool paused() const { return m_paused; }
    QString status() const;
    qint64 selectedPersonId() const { return m_selected.isEmpty() ? 0 : m_selected.first(); }
    QVariantList selectedPersonIds() const;
    QString selectedLabel() const;
    QStringList selectedPaths() const;
    QStringList libraryPaths() const;
    int revision() const { return m_revision; }

    // `enabled` is the toolbar switch. Remote folders are ignored.
    Q_INVOKABLE void watch(const QString &folder, bool showHidden, bool enabled);
    // The People place. Does not scan; it lists names already stored.
    Q_INVOKABLE void openLibrary();
    // Walk the current folder again. Unchanged files are skipped. In the
    // People place this only drops photos whose files are gone.
    Q_INVOKABLE void rescan();
    Q_INVOKABLE void setPaused(bool paused);
    Q_INVOKABLE void setScrolling(bool scrolling);
    Q_INVOKABLE void togglePerson(qint64 personId);
    Q_INVOKABLE bool renamePerson(qint64 personId, const QString &name);
    Q_INVOKABLE bool applyName(qint64 personId, const QString &name, const QVariantList &excludeFaceIds);
    Q_INVOKABLE bool mergeSelected();
    Q_INVOKABLE void markAsStranger(qint64 personId);
    Q_INVOKABLE qint64 detachFace(qint64 faceId);
    Q_INVOKABLE QVariantList cluster(qint64 personId) const;
    Q_INVOKABLE QVariantList facesOn(const QString &path) const;
    // image:// URL of the oriented photo, so face boxes match the pixels.
    Q_INVOKABLE QString previewSource(const QString &path) const;

Q_SIGNALS:
    void statusChanged();
    void selectionChanged();
    void revisionChanged();

private:
    void applyHold();
    void noteEdit();
    void pruneSelection();
    QString scopeRoot() const;

    FaceStore m_store;
    PeopleModel m_people;
    FaceIndexer m_indexer;
    QTimer m_refresh;
    QString m_root;
    bool m_showHidden = false;
    bool m_watching = false;
    bool m_library = false;
    bool m_paused = false;
    bool m_scrolling = false;
    QList<qint64> m_selected;
    int m_revision = 0;
};
