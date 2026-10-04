#pragma once

#include "FaceTypes.h"

#include <QString>

#include <memory>

// YuNet finds the boxes. SFace turns each box into a 128-number vector.
// Both models ship with this program and run on the CPU. Nothing is sent
// off the machine.
class FaceEngine
{
public:
    FaceEngine();
    ~FaceEngine();

    FaceEngine(const FaceEngine &) = delete;
    FaceEngine &operator=(const FaceEngine &) = delete;

    static QString modelsDirectory();

    bool load(QString *error);
    bool isLoaded() const;

    struct Result {
        int width = 0;
        int height = 0;
        QVector<FaceSample> faces;
        bool modelsMissing = false;
        bool decoded = false;
    };

    // `path` is a local file. Boxes are in the oriented image's pixel space,
    // which is also what the preview draws.
    Result detect(const QString &path, QString *error);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
