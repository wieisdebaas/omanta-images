#include "FaceEngine.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>

#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>

#include <algorithm>
#include <cmath>

namespace {

// OpenCV's own default (0.9) misses a lot of ordinary photos. 0.7 keeps
// clear faces and drops most of the noise.
constexpr float kDetectScore = 0.7f;
constexpr int kDetectLongSide = 1024;

QString modelFile(const QString &name)
{
    return FaceEngine::modelsDirectory() + QLatin1Char('/') + name;
}

QImage cropFromBgr(const cv::Mat &bgr)
{
    if (bgr.empty())
        return {};
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    QImage image(rgb.data, rgb.cols, rgb.rows, int(rgb.step), QImage::Format_RGB888);
    return image.copy();
}

} // namespace

struct FaceEngine::Impl {
    cv::Ptr<cv::FaceDetectorYN> detector;
    cv::Ptr<cv::FaceRecognizerSF> recognizer;
};

FaceEngine::FaceEngine()
    : m(std::make_unique<Impl>())
{
}

FaceEngine::~FaceEngine() = default;

QString FaceEngine::modelsDirectory()
{
    const QString override = qEnvironmentVariable("OMANTA_FACE_MODELS");
    if (!override.isEmpty())
        return override;

    const QString besideApp = QCoreApplication::applicationDirPath()
        + QStringLiteral("/../share/omanta/faces");
    if (QFileInfo::exists(besideApp + QStringLiteral("/face_recognition_sface_2021dec.onnx")))
        return QFileInfo(besideApp).absoluteFilePath();

#ifdef OMANTA_FACE_MODELS_DIR
    return QStringLiteral(OMANTA_FACE_MODELS_DIR);
#else
    return besideApp;
#endif
}

bool FaceEngine::isLoaded() const
{
    return m && m->detector && m->recognizer;
}

bool FaceEngine::load(QString *error)
{
    const QString detectorPath = modelFile(QStringLiteral("face_detection_yunet_2023mar.onnx"));
    const QString recognizerPath = modelFile(QStringLiteral("face_recognition_sface_2021dec.onnx"));
    if (!QFileInfo::exists(detectorPath) || !QFileInfo::exists(recognizerPath)) {
        if (error) {
            *error = QStringLiteral("Face models are not installed (expected under %1)")
                         .arg(modelsDirectory());
        }
        return false;
    }

    m->detector = cv::FaceDetectorYN::create(detectorPath.toStdString(), "",
                                             cv::Size(320, 320), kDetectScore, 0.3f, 5000);
    m->recognizer = cv::FaceRecognizerSF::create(recognizerPath.toStdString(), "");
    if (!m->detector || !m->recognizer) {
        if (error)
            *error = QStringLiteral("Could not load the face models");
        return false;
    }
    return true;
}

FaceEngine::Result FaceEngine::detect(const QString &path, QString *error)
{
    Result result;
    if (!isLoaded() && !load(error)) {
        result.modelsMissing = true;
        return result;
    }

    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize full = reader.size();
    if (full.isValid() && full.width() > 0 && full.height() > 0) {
        const int longSide = std::max(full.width(), full.height());
        if (longSide > kDetectLongSide) {
            const double scale = double(kDetectLongSide) / double(longSide);
            reader.setScaledSize(QSize(std::max(1, int(std::lround(full.width() * scale))),
                                       std::max(1, int(std::lround(full.height() * scale)))));
        }
    }

    const QImage image = reader.read();
    if (image.isNull()) {
        if (error)
            *error = reader.errorString();
        return result;
    }
    result.decoded = true;
    if (!full.isValid() || full.width() <= 0 || full.height() <= 0)
        full = image.size();
    result.width = full.width();
    result.height = full.height();

    // YuNet wants both sides to be multiples of 32. Cropping a few pixels
    // off the bottom-right keeps the scale identical to the decoded image.
    const int detWidth = std::max(32, image.width() / 32 * 32);
    const int detHeight = std::max(32, image.height() / 32 * 32);
    if (image.width() < 32 || image.height() < 32)
        return result;

    QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    if (detWidth != rgb.width() || detHeight != rgb.height())
        rgb = rgb.copy(0, 0, detWidth, detHeight);

    cv::Mat wrapped(rgb.height(), rgb.width(), CV_8UC3, rgb.bits(), rgb.bytesPerLine());
    cv::Mat bgr;
    cv::cvtColor(wrapped, bgr, cv::COLOR_RGB2BGR);

    m->detector->setInputSize(bgr.size());
    cv::Mat faces;
    m->detector->detect(bgr, faces);
    if (faces.empty())
        return result;

    const double sx = double(full.width()) / double(image.width());
    const double sy = double(full.height()) / double(image.height());

    for (int row = 0; row < faces.rows; ++row) {
        const float x = faces.at<float>(row, 0);
        const float y = faces.at<float>(row, 1);
        const float w = faces.at<float>(row, 2);
        const float h = faces.at<float>(row, 3);
        const float score = faces.at<float>(row, 14);
        if (w < 8.f || h < 8.f)
            continue;

        cv::Mat aligned;
        cv::Mat feature;
        m->recognizer->alignCrop(bgr, faces.row(row), aligned);
        m->recognizer->feature(aligned, feature);
        if (feature.empty())
            continue;
        feature = feature.reshape(1, 1);
        if (feature.cols != FaceEmbeddingLength)
            continue;

        FaceSample sample;
        sample.box = QRectF(x * sx, y * sy, w * sx, h * sy);
        sample.score = score;
        sample.embedding.resize(FaceEmbeddingLength);
        for (int col = 0; col < FaceEmbeddingLength; ++col)
            sample.embedding[col] = feature.at<float>(0, col);
        sample.crop = cropFromBgr(aligned);
        result.faces.append(sample);
    }
    return result;
}
