#pragma once

#include "FaceTypes.h"

#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>

class FaceStore;

// Walks one local folder on a background thread and writes faces into the
// store. One photo at a time, so the window stays usable. Cancelling bumps
// a generation: the photo already in the detector is finished and then dropped.
class FaceIndexer : public QObject
{
    Q_OBJECT

public:
    explicit FaceIndexer(QObject *parent = nullptr);
    ~FaceIndexer() override;

    void start(FaceStore *store, const QString &root, bool showHidden);
    void stop();
    // While held, the photo already in the detector finishes, and the next
    // one waits. The user's pause and a fast scroll both hold the queue.
    void setHeld(bool held);
    bool held() const { return m_held; }

    bool indexing() const { return m_indexing; }
    int done() const { return m_done; }
    int total() const { return m_total; }
    bool capped() const { return m_capped; }
    QString error() const { return m_error; }

Q_SIGNALS:
    void progress(int done, int total);
    void photoIndexed();
    void finished();

private:
    class Worker;

    void collectFinished(quint64 token, const QStringList &files, bool capped);
    void pump();
    void detected(quint64 token, const QString &path, qint64 size, qint64 mtimeMs,
                  int width, int height, const QVector<FaceSample> &faces,
                  bool decoded, bool modelsMissing, const QString &error);

    QThread m_thread;
    Worker *m_worker = nullptr;
    FaceStore *m_store = nullptr;
    std::atomic<quint64> m_generation { 0 };
    QStringList m_queue;
    QString m_root;
    int m_done = 0;
    int m_total = 0;
    bool m_indexing = false;
    bool m_capped = false;
    bool m_held = false;
    QString m_error;
};
