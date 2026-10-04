#pragma once

#include <QImage>
#include <QRectF>
#include <QString>
#include <QVector>

// SFace's feature() vector. Cosine similarity is computed on these; the
// recognizer itself is not required to store or compare them.
constexpr int FaceEmbeddingLength = 128;

// OpenCV's published SFace cosine cut: scores at or above this are the same
// person. Higher means more alike (it is similarity, not a distance).
constexpr float kSamePersonCosine = 0.363f;

// One detected face, in the oriented pixel space of the source image.
struct FaceSample {
    QRectF box;
    float score = 0.f;
    QVector<float> embedding;
    QImage crop;
};

// A person row for the chip bar. `name` is empty until the user types one.
// `score` is the strongest face under the folder being viewed, which also
// picks the cover crop.
struct PersonInfo {
    qint64 id = 0;
    QString name;
    int photoCount = 0;
    QString cropPath;
    float score = 0.f;
    // The single shared bucket for people the user marked as strangers.
    bool stranger = false;
};

// One face already grouped under a person, for the naming dialog.
struct PersonFace {
    qint64 faceId = 0;
    QString cropPath;
    QString uri;
};

// A face box drawn on one photo.
struct FaceMark {
    qint64 faceId = 0;
    qint64 personId = 0;
    QString name;
    QRectF box;
    int imageWidth = 0;
    int imageHeight = 0;
    float score = 0.f;
    bool stranger = false;
};
