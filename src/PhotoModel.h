#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QString>
#include <QtQmlIntegration>

#include <gio/gio.h>

#include "FileEntry.h"

// Recursive image listing for Ctrl+3. Speaks the same roles as DirectoryModel
// so FileSortFilterModel and PhotoGridView need no special cases. The `name`
// role is the path relative to the photo root — unique across subfolders.
//
// Metadata only: decoding belongs to PhotoThumbnailProvider. Breadth-first,
// cancellable by generation, few directories in flight, soft caps on photos /
// visited dirs / depth.
class PhotoModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(bool capped READ capped NOTIFY cappedChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)

public:
    explicit PhotoModel(QObject *parent = nullptr);
    ~PhotoModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString path() const { return m_path; }
    void setPath(const QString &path);

    bool active() const { return m_active; }
    void setActive(bool active);

    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool showHidden);

    bool scanning() const { return m_scanning; }
    bool capped() const { return m_capped; }
    int count() const { return int(m_results.size()); }
    QString errorMessage() const { return m_errorMessage; }

    Q_INVOKABLE void reload();

Q_SIGNALS:
    void pathChanged();
    void activeChanged();
    void showHiddenChanged();
    void scanningChanged();
    void cappedChanged();
    void countChanged();
    void errorMessageChanged();
    void needsMount(const QString &location);

private:
    struct Result {
        FileEntry entry;
        QString relPath;
    };

    struct PendingDir {
        GFile *file = nullptr;
        QString relPrefix;
        int depth = 0;
    };

    struct EnumerateCtx {
        QPointer<PhotoModel> self;
        quint64 generation = 0;
        QString relPrefix;
        int depth = 0;
    };

    static void onEnumerateReady(GObject *source, GAsyncResult *res, gpointer data);
    static void onNextFilesReady(GObject *source, GAsyncResult *res, gpointer data);

    void restart();
    void stop();
    void pump();
    void finishDir();
    void consume(GList *infos, const QString &relPrefix, int depth);
    void flushPending();
    void clearQueue();
    void setScanning(bool scanning);
    void setErrorMessage(const QString &message);

    QString m_path;
    bool m_active = false;
    bool m_showHidden = false;
    bool m_scanning = false;
    bool m_capped = false;
    QString m_errorMessage;

    QList<Result> m_results;
    QList<Result> m_pending;
    QList<PendingDir> m_queue;
    int m_inFlight = 0;
    int m_visitedDirs = 0;
    quint64 m_generation = 0;
    GCancellable *m_cancellable = nullptr;
};
