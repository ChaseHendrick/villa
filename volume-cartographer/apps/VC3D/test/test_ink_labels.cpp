// Ink labels: raster, history, files, controller, overlay and keys.

#include "ink_labels/InkLabelRaster.hpp"
#include "ink_labels/InkLabelStore.hpp"

#include "vc/core/util/QuadSurface.hpp"
#include "vc/core/util/RenderCanvas.hpp"
#include "vc/core/util/Tiff.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtConcurrent/QtConcurrent>
#include <QtTest/QtTest>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <tiffio.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;
using namespace vc3d::ink_labels;

namespace {

fs::path toPath(const QTemporaryDir& dir)
{
    return fs::path(dir.path().toStdString());
}

uint64_t commitTo(TileRaster& raster, History& history, const Shape& shape, PaintOp op, const char* label = "paint")
{
    EditTxn txn(raster, label, Actor::User);
    const uint64_t changed = txn.apply(shape, op);
    HistoryEntry entry = txn.commit(history.nextSerial());
    if (!entry.changes.empty())
        history.push(std::move(entry));
    return changed;
}

Shape disc(double x, double y, double r)
{
    return {Shape::Kind::Disc, {{x, y}}, r};
}

Shape capsule(cv::Point2d a, cv::Point2d b, double r)
{
    return {Shape::Kind::Capsule, {a, b}, r};
}

Shape polygon(std::vector<cv::Point2d> pts)
{
    return {Shape::Kind::Polygon, std::move(pts), 0};
}

cv::Mat_<uint8_t> fullImage(const TileRaster& raster)
{
    cv::Mat_<uint8_t> out;
    raster.readRect(cv::Rect(cv::Point(0, 0), raster.size()), out);
    return out;
}

std::array<uint64_t, 8> bruteCounts(const TileRaster& raster)
{
    std::array<uint64_t, 8> counts{};
    const cv::Mat_<uint8_t> img = fullImage(raster);
    for (int y = 0; y < img.rows; ++y)
        for (int x = 0; x < img.cols; ++x)
            ++counts[std::size_t(img(y, x) & 7)];
    return counts;
}

bool sameImage(const cv::Mat& a, const cv::Mat& b)
{
    if (a.size() != b.size() || a.type() != b.type())
        return false;
    cv::Mat diff;
    cv::compare(a, b, diff, cv::CMP_NE);
    return cv::countNonZero(diff.reshape(1)) == 0;
}

cv::Mat_<uint8_t> readGray(const fs::path& path)
{
    cv::Mat img = cv::imread(path.string(), cv::IMREAD_UNCHANGED);
    return cv::Mat_<uint8_t>(img);
}

struct TiffTags {
    uint32_t width = 0, height = 0, tileW = 0, tileH = 0;
    uint16_t spp = 0, bps = 0, compression = 0;
};

TiffTags readTags(const fs::path& path)
{
    TiffTags t;
    TIFF* tf = TIFFOpen(path.string().c_str(), "r");
    if (!tf)
        return t;
    TIFFGetField(tf, TIFFTAG_IMAGEWIDTH, &t.width);
    TIFFGetField(tf, TIFFTAG_IMAGELENGTH, &t.height);
    TIFFGetField(tf, TIFFTAG_TILEWIDTH, &t.tileW);
    TIFFGetField(tf, TIFFTAG_TILELENGTH, &t.tileH);
    TIFFGetFieldDefaulted(tf, TIFFTAG_SAMPLESPERPIXEL, &t.spp);
    TIFFGetFieldDefaulted(tf, TIFFTAG_BITSPERSAMPLE, &t.bps);
    TIFFGetFieldDefaulted(tf, TIFFTAG_COMPRESSION, &t.compression);
    TIFFClose(tf);
    return t;
}

CanvasKey keyFor(cv::Size canvas)
{
    // A grid of canvas/20 at scale 0.05 renders exactly `canvas`.
    return canvasKeyFor(vc::RenderCanvas::make({canvas.width / 20, canvas.height / 20}, {0.05f, 0.05f}, 1.0));
}

SaveRequest saveRequest(const TileRaster& raster, const StorePaths& paths, const CanvasKey& key, Target target, uint64_t serial)
{
    SaveRequest req;
    req.snapshot = raster.snapshot();
    req.paths = paths;
    req.sidecar.canvas = key;
    req.sidecar.segmentName = paths.prefix;
    req.sidecar.stateSerial = serial;
    req.sidecar.savedBy = "test";
    req.target = target;
    return req;
}

}  // namespace

class TestInkLabels : public QObject
{
    Q_OBJECT

private slots:
    void paintOpTable();
    void summaryCodesPool();
    void stampsPaintAndSeamsMatch();
    void polygonsFillAndCollapse();
    void uniformCollapseIgnoresOutOfCanvas();
    void snapshotIsolation();
    void historyUndoRedoAndBudget();
    void dirtyTrackingAcrossSaves();
    void countsMatchBruteForce();
    void storePathsAndSuffixes();
    void tiffRoundTrip();
    void loadRefusesWrongShapesAndWarns();
    void sidecarCanvasRoundTrip();
    void saveProtocolDetectsOutsideEdits();
    void autosaveTargetLeavesTrainerFiles();
    void workerSaveReadsSnapshot();
};

void TestInkLabels::paintOpTable()
{
    QCOMPARE(paintOp(PaintClass::Ink, false, false), (PaintOp{3, 0}));
    QCOMPARE(paintOp(PaintClass::Ink, true, false), (PaintOp{0, 3}));
    QCOMPARE(paintOp(PaintClass::Background, false, false), (PaintOp{2, 1}));
    QCOMPARE(paintOp(PaintClass::Background, false, true), (PaintOp{2, 0}));
    QCOMPARE(paintOp(PaintClass::Background, true, false), (PaintOp{0, 3}));
    QCOMPARE(paintOp(PaintClass::Validation, false, false), (PaintOp{4, 0}));
    QCOMPARE(paintOp(PaintClass::Validation, true, true), (PaintOp{0, 4}));

    // Background over ink makes background; keeping ink leaves ink supervised.
    QCOMPARE(int(paintOp(PaintClass::Background, false, false).apply(3)), 2);
    QCOMPARE(int(paintOp(PaintClass::Background, false, true).apply(3)), 3);
    // The eraser returns to unknown and keeps validation.
    QCOMPARE(int(paintOp(PaintClass::Ink, true, false).apply(7)), 4);
    QCOMPARE(int(paintOp(PaintClass::Background, true, false).apply(2)), 0);
    // Erasing unknown pixels changes nothing.
    QCOMPARE(int(paintOp(PaintClass::Ink, true, false).apply(0)), 0);
    QCOMPARE(int(paintOp(PaintClass::Validation, true, false).apply(3)), 3);
}

void TestInkLabels::summaryCodesPool()
{
    QCOMPARE(int(summaryCode(0)), 0);
    QCOMPARE(int(summaryCode(kInk | kSupervision)), 0b1011);
    QCOMPARE(int(summaryCode(kSupervision)), 0b1010);
    QCOMPARE(int(summaryCode(kValidation)), 0b0100);
    // Any-bits OR, the all-supervised bit ANDs.
    QCOMPARE(int(poolCodes(summaryCode(3), summaryCode(0))), 0b1001);
    QCOMPARE(int(poolCodes(summaryCode(2), summaryCode(3))), 0b1011);
    QCOMPARE(int(poolCodes(summaryCode(4), summaryCode(2))), 0b1100);
    // The neutral code is the identity.
    for (int v = 0; v < 8; ++v)
        QCOMPARE(int(poolCodes(summaryCode(uint8_t(v)), kNeutralCode)), int(summaryCode(uint8_t(v))));
}

void TestInkLabels::stampsPaintAndSeamsMatch()
{
    const PaintOp ink = paintOp(PaintClass::Ink, false, false);

    // The smallest click still paints its pixel.
    {
        TileRaster raster({600, 600});
        History history;
        QVERIFY(commitTo(raster, history, disc(10.5, 10.5, 0.5), ink) >= 1);
        QCOMPARE(int(raster.at(10, 10)), 3);
        QVERIFY(commitTo(raster, history, disc(300.1, 20.9, 0.5), ink) >= 1);
        QCOMPARE(int(raster.at(300, 20)), 3);
    }

    // A fast drag leaves no gap along the segment.
    {
        TileRaster raster({600, 600});
        History history;
        const cv::Point2d a(40.3, 51.7), b(340.9, 260.2);
        commitTo(raster, history, capsule(a, b, 3.0), ink);
        for (int i = 0; i <= 2000; ++i) {
            const double t = i / 2000.0;
            const cv::Point2d p = a + t * (b - a);
            QCOMPARE(int(raster.at(int(std::floor(p.x)), int(std::floor(p.y)))), 3);
        }
    }

    // A disc across four tiles equals the same disc drawn in one window.
    {
        TileRaster raster({600, 600});
        History history;
        const cv::Point2d c(256.3, 255.7);
        const double r = 40.0;
        commitTo(raster, history, disc(c.x, c.y, r), ink);
        cv::Mat_<uint8_t> reference(600, 600, uint8_t(0));
        cv::circle(reference,
                   cv::Point(int(std::lround((c.x - 0.5) * 256)), int(std::lround((c.y - 0.5) * 256))),
                   int(std::lround(r * 256)),
                   cv::Scalar(255),
                   cv::FILLED,
                   cv::LINE_8,
                   8);
        const cv::Mat_<uint8_t> painted = fullImage(raster);
        QCOMPARE(cv::countNonZero((painted == 3) != (reference == 255)), 0);
        QVERIFY(cv::countNonZero(reference) > 4000);
        // It really touched all four tiles.
        for (int index : {0, 1, 3, 4})
            QVERIFY(raster.tile(index).isDense());
    }

    // A point far off the canvas cannot wrap around in the fixed-point
    // rasterizer and paint the wrong side: such a shape changes nothing.
    {
        TileRaster raster({600, 600});
        History history;
        QCOMPARE(commitTo(raster, history, capsule({-1.0e7, 10.5}, {100.5, 10.5}, 3.0), ink), uint64_t(0));
        QCOMPARE(commitTo(raster, history, polygon({{-1.0e9, 0}, {100, 0}, {100, 100}}), ink), uint64_t(0));
        QCOMPARE(bruteCounts(raster)[0], uint64_t(600) * 600);
        // Inside the range a long stroke paints exactly its on-canvas part.
        QVERIFY(commitTo(raster, history, capsule({-1.0e6, 10.5}, {100.5, 10.5}, 1.0), ink) > 0);
        QCOMPARE(int(raster.at(50, 10)), 3);
        QCOMPARE(int(raster.at(0, 10)), 3);
        QCOMPARE(int(raster.at(599, 10)), 0);
        QCOMPARE(int(raster.at(150, 10)), 0);
    }
}

void TestInkLabels::polygonsFillAndCollapse()
{
    const PaintOp ink = paintOp(PaintClass::Ink, false, false);

    // A concave L: the filled area is within one perimeter of the true area.
    {
        TileRaster raster({400, 400});
        History history;
        commitTo(raster, history, polygon({{20, 20}, {220, 20}, {220, 80}, {80, 80}, {80, 260}, {20, 260}}), ink);
        const double area = 200.0 * 60.0 + 60.0 * 180.0;
        const double perimeter = 2.0 * (200.0 + 240.0);
        const auto counts = raster.counts();
        QVERIFY(std::abs(double(counts[3]) - area) <= perimeter);
        QCOMPARE(int(raster.at(150, 150)), 0);  // the notch stays empty
        QCOMPARE(int(raster.at(50, 200)), 3);
    }

    // A figure eight fills both lobes.
    {
        TileRaster raster({300, 300});
        History history;
        commitTo(raster, history, polygon({{20, 20}, {120, 120}, {220, 20}, {220, 220}, {120, 120}, {20, 220}}), ink);
        QCOMPARE(int(raster.at(40, 120)), 3);
        QCOMPARE(int(raster.at(200, 120)), 3);
    }

    // A huge polygon leaves its interior tiles Uniform.
    {
        TileRaster raster({7000, 7000});
        History history;
        commitTo(raster, history, polygon({{100, 100}, {6100, 100}, {6100, 6100}, {100, 6100}}), ink);
        const int center = raster.tileIndex(12, 12);
        QVERIFY(!raster.tile(center).isDense());
        QCOMPARE(int(raster.tile(center).uniform), 3);
        // Only tiles the edges cross stay dense.
        QVERIFY(raster.denseTileCount() <= 4 * 25);
        // OpenCV's scanline fill includes pixels on the outline.
        QVERIFY(std::abs(double(raster.counts()[3]) - 6000.0 * 6000.0) <= 4.0 * 6000.0);
    }
}

void TestInkLabels::uniformCollapseIgnoresOutOfCanvas()
{
    TileRaster raster({300, 300});
    History history;
    commitTo(raster, history, polygon({{-10, -10}, {310, -10}, {310, 310}, {-10, 310}}), paintOp(PaintClass::Background, false, false));
    QCOMPARE(raster.denseTileCount(), std::size_t(0));
    for (int i = 0; i < raster.tileCount(); ++i)
        QCOMPARE(int(raster.tile(i).uniform), 2);
    QCOMPARE(raster.counts()[2], uint64_t(300) * 300);

    // An edge tile painted fully inside the canvas collapses too.
    commitTo(raster, history, polygon({{256, 256}, {300, 256}, {300, 300}, {256, 300}}), paintOp(PaintClass::Ink, false, false));
    QVERIFY(!raster.tile(3).isDense());
    QCOMPARE(int(raster.tile(3).uniform), 3);
}

void TestInkLabels::snapshotIsolation()
{
    TileRaster raster({1000, 600});
    History history;
    commitTo(raster, history, disc(100, 100, 30), paintOp(PaintClass::Ink, false, false));
    commitTo(raster, history, disc(700, 400, 30), paintOp(PaintClass::Ink, false, false));
    const TileRaster snap = raster.snapshot();
    const cv::Mat_<uint8_t> before = fullImage(snap);

    commitTo(raster, history, disc(110, 100, 30), paintOp(PaintClass::Background, false, false));
    QVERIFY(sameImage(fullImage(snap), before));
    QVERIFY(!sameImage(fullImage(raster), before));
    // Tiles the edit never touched are still shared.
    const int untouched = raster.tileIndex(2, 1);
    QVERIFY(raster.tile(untouched).isDense());
    QCOMPARE(snap.tile(untouched).dense.get(), raster.tile(untouched).dense.get());
    QVERIFY(snap.tile(0).dense.get() != raster.tile(0).dense.get());
}

void TestInkLabels::historyUndoRedoAndBudget()
{
    TileRaster raster({800, 800});
    History history;
    const PaintOp ink = paintOp(PaintClass::Ink, false, false);
    const cv::Mat_<uint8_t> empty = fullImage(raster);
    commitTo(raster, history, disc(200, 200, 50), ink);
    const cv::Mat_<uint8_t> one = fullImage(raster);
    const auto oneCounts = raster.counts();
    commitTo(raster, history, capsule({100, 600}, {700, 600}, 20), ink);
    const cv::Mat_<uint8_t> two = fullImage(raster);
    QCOMPARE(history.undoDepth(), std::size_t(2));

    QVERIFY(history.undo(raster));
    QVERIFY(sameImage(fullImage(raster), one));
    QCOMPARE(raster.counts(), oneCounts);
    QVERIFY(history.undo(raster));
    QVERIFY(sameImage(fullImage(raster), empty));
    QVERIFY(!history.undo(raster));
    QVERIFY(history.redo(raster));
    QVERIFY(history.redo(raster));
    QVERIFY(sameImage(fullImage(raster), two));
    QVERIFY(!history.redo(raster));

    // A new edit clears redo.
    QVERIFY(history.undo(raster));
    QCOMPARE(history.redoDepth(), std::size_t(1));
    commitTo(raster, history, disc(600, 200, 10), ink);
    QCOMPARE(history.redoDepth(), std::size_t(0));

    // A tight budget drops the oldest entries and keeps the newest.
    TileRaster small({1024, 256});
    const std::size_t budget = 2 * std::size_t(kTilePixels) + 200;
    History tight(budget);
    std::vector<uint64_t> serials;
    for (int i = 0; i < 4; ++i) {
        EditTxn txn(small, "stroke", Actor::User);
        txn.apply(disc(128 + 256 * i, 128, 20), ink);
        HistoryEntry e = txn.commit(tight.nextSerial());
        serials.push_back(e.serial);
        tight.push(std::move(e));
    }
    QVERIFY(tight.bytes() <= budget);
    QCOMPARE(tight.undoDepth(), std::size_t(2));
    QCOMPARE(tight.top()->serial, serials.back());
    // Undo everything that is left: the base is the newest dropped entry.
    while (tight.undo(small)) {
    }
    QCOMPARE(tight.stateSerial(), serials[1]);
    QCOMPARE(int(small.at(128, 128)), 3);  // the dropped strokes stay
    QCOMPARE(int(small.at(384, 128)), 3);
    QCOMPARE(int(small.at(640, 128)), 0);

    // A single entry over the budget is still kept.
    History tiny(1);
    TileRaster r2({300, 300});
    commitTo(r2, tiny, disc(150, 150, 100), ink);
    QCOMPARE(tiny.undoDepth(), std::size_t(1));
}

void TestInkLabels::dirtyTrackingAcrossSaves()
{
    TileRaster raster({512, 512});
    History history;
    const PaintOp ink = paintOp(PaintClass::Ink, false, false);
    QVERIFY(!history.dirty());
    commitTo(raster, history, disc(100, 100, 10), ink);
    QVERIFY(history.dirty());

    // A save of this state starts, then the user paints again.
    const uint64_t savedSerial = history.stateSerial();
    commitTo(raster, history, disc(300, 300, 10), ink);
    history.markClean(savedSerial);
    QVERIFY(history.dirty());
    QVERIFY(history.undo(raster));
    QVERIFY(!history.dirty());
    QVERIFY(history.redo(raster));
    QVERIFY(history.dirty());
    history.markClean(history.stateSerial());
    QVERIFY(!history.dirty());
    QVERIFY(history.undo(raster));
    QVERIFY(history.dirty());

    history.reset(41);
    QVERIFY(!history.dirty());
    QCOMPARE(history.stateSerial(), uint64_t(41));
    QCOMPARE(history.nextSerial(), uint64_t(42));
}

void TestInkLabels::countsMatchBruteForce()
{
    TileRaster raster({1100, 700});
    History history;
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> ux(-20, 1120), uy(-20, 720), ur(0.5, 60);
    std::uniform_int_distribution<int> kind(0, 3), cls(0, 2), coin(0, 1);
    for (int i = 0; i < 500; ++i) {
        const PaintOp op = paintOp(PaintClass(cls(rng)), coin(rng) == 1, coin(rng) == 1);
        switch (kind(rng)) {
            case 0:
                commitTo(raster, history, disc(ux(rng), uy(rng), ur(rng)), op);
                break;
            case 1:
                commitTo(raster, history, capsule({ux(rng), uy(rng)}, {ux(rng), uy(rng)}, ur(rng)), op);
                break;
            case 2:
                commitTo(raster, history, polygon({{ux(rng), uy(rng)}, {ux(rng), uy(rng)}, {ux(rng), uy(rng)}, {ux(rng), uy(rng)}}), op);
                break;
            default: {
                if (coin(rng) == 1 && history.undo(raster))
                    break;
                cv::Mat_<uint8_t> mask(37, 53, uint8_t(0));
                cv::circle(mask, {26, 18}, 15, cv::Scalar(1), cv::FILLED);
                EditTxn txn(raster, "mask", Actor::Agent);
                txn.applyMask({int(ux(rng)), int(uy(rng))}, mask, op);
                HistoryEntry e = txn.commit(history.nextSerial());
                if (!e.changes.empty())
                    history.push(std::move(e));
            }
        }
    }
    QCOMPARE(raster.counts(), bruteCounts(raster));
    uint64_t total = 0;
    for (auto c : raster.counts())
        total += c;
    QCOMPARE(total, uint64_t(1100) * 700);
}

void TestInkLabels::storePathsAndSuffixes()
{
    const StorePaths paths = storePathsFor("/data/project", "w045");
    QCOMPARE(QString::fromStdString(paths.dir.string()), QStringLiteral("/data/project/ink_labels/w045"));
    QCOMPARE(QString::fromStdString(paths.file(LabelKind::Ink).filename().string()), QStringLiteral("w045_inklabels.tif"));
    QCOMPARE(QString::fromStdString(paths.file(LabelKind::Supervision).filename().string()), QStringLiteral("w045_supervision_mask.tif"));
    QCOMPARE(QString::fromStdString(paths.file(LabelKind::Validation).filename().string()), QStringLiteral("w045_validation_mask.tif"));
    StorePaths v2 = paths;
    v2.version = 2;
    QCOMPARE(QString::fromStdString(v2.file(LabelKind::Ink).filename().string()), QStringLiteral("w045_inklabels_v2.tif"));

    // The trainer reads .tif/.tiff/.zarr and create_label_zarrs .tif/.tiff/.png:
    // every auxiliary file must use another extension.
    const std::vector<fs::path> auxiliary{paths.sidecar(),
                                          paths.sidecar(Target::Autosave),
                                          paths.file(LabelKind::Ink, Target::Autosave),
                                          paths.file(LabelKind::Validation, Target::Autosave),
                                          paths.previous(LabelKind::Supervision),
                                          paths.lock(),
                                          fs::path(paths.file(LabelKind::Ink).string() + ".tmp-12-3")};
    for (const auto& p : auxiliary) {
        const std::string ext = p.extension().string();
        QVERIFY2(ext != ".tif" && ext != ".tiff" && ext != ".png" && ext != ".zarr", p.string().c_str());
    }
}

void TestInkLabels::tiffRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const cv::Size canvas(4097, 2049);
    const CanvasKey key = canvasKeyFor(vc::RenderCanvas::make({4097, 2049}, {1.f, 1.f}, 1.0));
    QCOMPARE(key.width, 4097);
    const StorePaths paths = storePathsFor(toPath(dir), "seg");
    TileRaster raster(canvas);
    History history;
    commitTo(raster, history, polygon({{0, 0}, {4097, 0}, {4097, 2049}, {0, 2049}}), paintOp(PaintClass::Background, false, false));
    commitTo(raster, history, capsule({30, 40}, {4000, 2000}, 25), paintOp(PaintClass::Ink, false, false));
    commitTo(raster, history, disc(4090, 2040, 30), paintOp(PaintClass::Ink, false, false));

    SaveRequest req = saveRequest(raster, paths, key, Target::Trainer, history.stateSerial());
    const SaveOutcome out = saveLabels(req);
    QVERIFY2(out.ok, qPrintable(out.error));
    QVERIFY(fs::exists(paths.file(LabelKind::Ink)));
    QVERIFY(fs::exists(paths.file(LabelKind::Supervision)));
    QVERIFY(!fs::exists(paths.file(LabelKind::Validation)));
    for (const LabelKind kind : {LabelKind::Ink, LabelKind::Supervision}) {
        const TiffTags t = readTags(paths.file(kind));
        QCOMPARE(t.width, uint32_t(4097));
        QCOMPARE(t.height, uint32_t(2049));
        QCOMPARE(t.tileW, uint32_t(1024));
        QCOMPARE(t.tileH, uint32_t(1024));
        QCOMPARE(t.spp, uint16_t(1));
        QCOMPARE(t.bps, uint16_t(8));
        QCOMPARE(t.compression, uint16_t(COMPRESSION_LZW));
        const cv::Mat_<uint8_t> img = readGray(paths.file(kind));
        QCOMPARE(img.size(), canvas);
        const int nonBinary = cv::countNonZero((img != 0) & (img != 255));
        QCOMPARE(nonBinary, 0);
    }
    const cv::Mat_<uint8_t> inkImage = readGray(paths.file(LabelKind::Ink));
    QCOMPARE(uint64_t(cv::countNonZero(inkImage)), raster.counts()[3]);

    const LoadOutcome back = loadLabels(paths, key, Target::Trainer);
    QVERIFY2(back.ok, qPrintable(back.error));
    QVERIFY(back.warnings.isEmpty());
    QVERIFY(sameImage(fullImage(back.raster), fullImage(raster)));
    QCOMPARE(back.raster.counts(), raster.counts());
    QVERIFY(!back.validationPresent);

    // Validation is written when asked (a V bit is set or a file exists).
    commitTo(raster, history, disc(1000, 1000, 100), paintOp(PaintClass::Validation, false, false));
    SaveRequest req2 = saveRequest(raster, paths, key, Target::Trainer, history.stateSerial());
    req2.writeValidation = true;
    req2.expectedSha256 = out.sha256;
    const SaveOutcome out2 = saveLabels(req2);
    QVERIFY2(out2.ok, qPrintable(out2.error));
    QVERIFY(fs::exists(paths.file(LabelKind::Validation)));
    const LoadOutcome back2 = loadLabels(paths, key, Target::Trainer);
    QVERIFY(back2.ok);
    QVERIFY(back2.validationPresent);
    QVERIFY(sameImage(fullImage(back2.raster), fullImage(raster)));
}

void TestInkLabels::loadRefusesWrongShapesAndWarns()
{
    QTemporaryDir dir;
    const StorePaths paths = storePathsFor(toPath(dir), "seg");
    fs::create_directories(paths.dir);
    const CanvasKey key = keyFor({400, 200});

    cv::Mat_<uint8_t> wrong(100, 300, uint8_t(255));
    QVERIFY(cv::imwrite(paths.file(LabelKind::Ink).string(), wrong));
    LoadOutcome out = loadLabels(paths, key, Target::Trainer);
    QVERIFY(!out.ok);
    QVERIFY(out.error.contains(QStringLiteral("300 x 100")));
    QVERIFY(out.error.contains(QStringLiteral("400 x 200")));

    cv::Mat_<uint8_t> rotated(400, 200, uint8_t(255));
    QVERIFY(cv::imwrite(paths.file(LabelKind::Ink).string(), rotated));
    out = loadLabels(paths, key, Target::Trainer);
    QVERIFY(!out.ok);
    QVERIFY(out.error.contains(QStringLiteral("rotated")));

    // RGB uses channel 0 with a warning; anti-aliased values warn too.
    // imwrite stores BGR as RGB, so the red channel becomes sample 0.
    cv::Mat rgb(200, 400, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::rectangle(rgb, cv::Rect(10, 10, 50, 50), cv::Scalar(0, 0, 255), cv::FILLED);
    cv::rectangle(rgb, cv::Rect(200, 10, 50, 50), cv::Scalar(255, 255, 0), cv::FILLED);
    QVERIFY(cv::imwrite(paths.file(LabelKind::Ink).string(), rgb));
    cv::Mat_<uint8_t> soft(200, 400, uint8_t(0));
    soft(cv::Rect(100, 100, 20, 20)) = 255;
    soft(cv::Rect(120, 100, 5, 20)) = 128;
    soft(cv::Rect(125, 100, 2, 20)) = 64;
    QVERIFY(cv::imwrite(paths.file(LabelKind::Supervision).string(), soft));
    out = loadLabels(paths, key, Target::Trainer);
    QVERIFY2(out.ok, qPrintable(out.error));
    const QString warnings = out.warnings.join('\n');
    QVERIFY(warnings.contains(QStringLiteral("more than one sample")));
    QVERIFY(warnings.contains(QStringLiteral("128 (100 px)")));
    QVERIFY(warnings.contains(QStringLiteral("64 (40 px)")));
    QCOMPARE(int(out.raster.at(120, 110) & kSupervision), int(kSupervision));  // 128 counts as labeled
    const auto counts = out.raster.counts();
    QCOMPARE(counts[1] + counts[3] + counts[5] + counts[7], uint64_t(50 * 50));
}

void TestInkLabels::sidecarCanvasRoundTrip()
{
    QTemporaryDir dir;
    for (const float scale : {0.05f, 0.3f}) {
        const auto canvas = vc::RenderCanvas::make({152, 76}, {scale, scale}, 1.0);
        Sidecar s;
        s.canvas = canvasKeyFor(canvas);
        s.segmentName = "w045";
        s.segmentUuid = "abc";
        s.sha256[LabelKind::Ink] = "00ff";
        s.fileNames[LabelKind::Ink] = "w045_inklabels.tif";
        s.stateSerial = 412;
        s.predictionShown = std::string("/tmp/pred.png");
        s.edits = QJsonObject{{"user", QJsonObject{{"brush", 3}}}};
        s.provenance.append(QJsonObject{{"source", "test"}});
        const fs::path path = toPath(dir) / "s.json";
        QString error;
        QVERIFY2(writeSidecar(path, s, &error), qPrintable(error));
        const auto back = readSidecar(path, &error);
        QVERIFY2(back.has_value(), qPrintable(error));
        QVERIFY(back->canvas == s.canvas);
        QCOMPARE(back->canvas.scaleX(), scale);
        QCOMPARE(back->stateSerial, uint64_t(412));
        QCOMPARE(QString::fromStdString(back->sha256.at(LabelKind::Ink)), QStringLiteral("00ff"));
        QVERIFY(!back->sha256.count(LabelKind::Validation));
        QCOMPARE(QString::fromStdString(*back->predictionShown), QStringLiteral("/tmp/pred.png"));
        QCOMPARE(back->edits.value("user").toObject().value("brush").toInt(), 3);
        QCOMPARE(back->provenance.size(), 1);

        // The file names the scale both ways.
        QFile f(QString::fromStdString(path.string()));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(root.value("schema").toString(), QStringLiteral("vc3d.ink_labels/1"));
        QCOMPARE(root.value("canvas").toObject().value("scale_f32_hex").toArray().size(), 2);
        QVERIFY(root.value("files").toObject().value("validation_mask").isNull());
    }

    // A canvas mismatch refuses to load and names both canvases.
    const StorePaths paths = storePathsFor(toPath(dir), "seg");
    TileRaster raster({400, 200});
    const CanvasKey key = keyFor({400, 200});
    QVERIFY(saveLabels(saveRequest(raster, paths, key, Target::Trainer, 1)).ok);
    const CanvasKey other = keyFor({420, 200});
    const LoadOutcome out = loadLabels(paths, other, Target::Trainer);
    QVERIFY(!out.ok);
    QVERIFY(out.canvasMismatch);
    QVERIFY(out.error.contains(QStringLiteral("400 x 200 px")));
    QVERIFY(out.error.contains(QStringLiteral("420 x 200 px")));
}

void TestInkLabels::saveProtocolDetectsOutsideEdits()
{
    QTemporaryDir dir;
    const StorePaths paths = storePathsFor(toPath(dir), "seg");
    const CanvasKey key = keyFor({600, 400});
    TileRaster raster({600, 400});
    History history;
    commitTo(raster, history, disc(100, 100, 40), paintOp(PaintClass::Ink, false, false));
    const SaveOutcome first = saveLabels(saveRequest(raster, paths, key, Target::Trainer, history.stateSerial()));
    QVERIFY2(first.ok, qPrintable(first.error));

    // The sidecar records the hashes of the files in place.
    QString error;
    const auto sidecar = readSidecar(paths.sidecar(), &error);
    QVERIFY(sidecar.has_value());
    for (const auto& [kind, path] : first.files)
        QCOMPARE(sidecar->sha256.at(kind), fileSha256(path));

    // An outside edit blocks the save.
    const std::string firstInk = fileSha256(paths.file(LabelKind::Ink));
    cv::Mat_<uint8_t> edited = readGray(paths.file(LabelKind::Ink));
    edited(cv::Rect(300, 300, 10, 10)) = 255;
    QVERIFY(cv::imwrite(paths.file(LabelKind::Ink).string(), edited));
    commitTo(raster, history, disc(400, 200, 40), paintOp(PaintClass::Ink, false, false));
    SaveRequest second = saveRequest(raster, paths, key, Target::Trainer, history.stateSerial());
    second.expectedSha256 = first.sha256;
    const SaveOutcome blocked = saveLabels(second);
    QVERIFY(!blocked.ok);
    QCOMPARE(blocked.externallyEdited, QStringList{QStringLiteral("seg_inklabels.tif")});
    // Nothing was written; no temp file is left.
    for (const auto& entry : fs::directory_iterator(paths.dir))
        QVERIFY2(entry.path().string().find(".tmp-") == std::string::npos, entry.path().string().c_str());

    // Overwriting keeps the edited file as the previous generation.
    const std::string editedSha = fileSha256(paths.file(LabelKind::Ink));
    second.overwriteExternalEdits = true;
    const SaveOutcome forced = saveLabels(second);
    QVERIFY2(forced.ok, qPrintable(forced.error));
    QCOMPARE(fileSha256(paths.previous(LabelKind::Ink)), editedSha);
    QVERIFY(firstInk != editedSha);

    // A file swapped in from another save is reported on load.
    fs::copy_file(paths.previous(LabelKind::Ink), paths.file(LabelKind::Ink), fs::copy_options::overwrite_existing);
    const LoadOutcome loaded = loadLabels(paths, key, Target::Trainer);
    QVERIFY(loaded.ok);
    QVERIFY(loaded.warnings.join('\n').contains(QStringLiteral("seg_inklabels.tif")));
    QVERIFY(loaded.warnings.join('\n').contains(QStringLiteral(".prev")));
}

void TestInkLabels::autosaveTargetLeavesTrainerFiles()
{
    QTemporaryDir dir;
    const StorePaths paths = storePathsFor(toPath(dir), "seg");
    const CanvasKey key = keyFor({600, 400});
    TileRaster raster({600, 400});
    History history;
    commitTo(raster, history, disc(100, 100, 40), paintOp(PaintClass::Ink, false, false));
    const SaveOutcome trainer = saveLabels(saveRequest(raster, paths, key, Target::Trainer, history.stateSerial()));
    QVERIFY(trainer.ok);
    const std::string inkSha = fileSha256(paths.file(LabelKind::Ink));
    const std::string sidecarSha = fileSha256(paths.sidecar());

    commitTo(raster, history, disc(300, 200, 60), paintOp(PaintClass::Background, false, false));
    SaveRequest checkpoint = saveRequest(raster, paths, key, Target::Autosave, history.stateSerial());
    checkpoint.sidecar.writeSeq = 7;
    const SaveOutcome autosave = saveLabels(checkpoint);
    QVERIFY2(autosave.ok, qPrintable(autosave.error));
    QCOMPARE(fileSha256(paths.file(LabelKind::Ink)), inkSha);
    QCOMPARE(fileSha256(paths.sidecar()), sidecarSha);
    QVERIFY(fs::exists(paths.file(LabelKind::Ink, Target::Autosave)));
    QVERIFY(fs::exists(paths.sidecar(Target::Autosave)));
    QVERIFY(!fs::exists(paths.previous(LabelKind::Ink)));
    QString error;
    const auto autosaveSidecar = readSidecar(paths.sidecar(Target::Autosave), &error);
    QVERIFY(autosaveSidecar && autosaveSidecar->autosave);
    QCOMPARE(autosaveSidecar->writeSeq, uint64_t(7));
    QVERIFY(QString::fromStdString(autosaveSidecar->fileNames.at(LabelKind::Ink)).endsWith(QStringLiteral(".autosave")));

    const LoadOutcome restored = loadLabels(paths, key, Target::Autosave);
    QVERIFY2(restored.ok, qPrintable(restored.error));
    QVERIFY(sameImage(fullImage(restored.raster), fullImage(raster)));

    // A trainer save of a newer state removes the checkpoint.
    SaveRequest save = saveRequest(raster, paths, key, Target::Trainer, history.stateSerial());
    save.expectedSha256 = trainer.sha256;
    const SaveOutcome second = saveLabels(save);
    QVERIFY(second.ok);
    QVERIFY(!targetExists(paths, Target::Autosave));

    // So does a save of an older serial: after an undo the saved state can
    // have a smaller serial than a checkpoint written before the undo.
    QVERIFY(saveLabels(saveRequest(raster, paths, key, Target::Autosave, history.stateSerial() + 5)).ok);
    QVERIFY(targetExists(paths, Target::Autosave));
    SaveRequest afterUndo = saveRequest(raster, paths, key, Target::Trainer, 1);
    afterUndo.expectedSha256 = second.sha256;
    QVERIFY(saveLabels(afterUndo).ok);
    QVERIFY(!targetExists(paths, Target::Autosave));

    // A checkpoint that cannot be restored is moved aside, not deleted.
    QVERIFY(saveLabels(saveRequest(raster, paths, key, Target::Autosave, 9)).ok);
    QVERIFY(moveTargetAside(paths, Target::Autosave));
    QVERIFY(!targetExists(paths, Target::Autosave));
    fs::path aside = paths.sidecar(Target::Autosave);
    aside += ".unusable";
    QVERIFY(fs::exists(aside));
    aside = paths.file(LabelKind::Ink, Target::Autosave);
    aside += ".unusable";
    QVERIFY(fs::exists(aside));
}

void TestInkLabels::workerSaveReadsSnapshot()
{
    QTemporaryDir dir;
    const StorePaths paths = storePathsFor(toPath(dir), "seg");
    const CanvasKey key = keyFor({2000, 1400});
    TileRaster raster({2000, 1400});
    History history;
    commitTo(raster, history, polygon({{0, 0}, {2000, 0}, {2000, 1400}, {0, 1400}}), paintOp(PaintClass::Background, false, false));
    commitTo(raster, history, capsule({50, 50}, {1900, 1300}, 30), paintOp(PaintClass::Ink, false, false));
    const SaveRequest req = saveRequest(raster, paths, key, Target::Trainer, history.stateSerial());
    const cv::Mat_<uint8_t> expected = fullImage(req.snapshot);

    QFuture<SaveOutcome> future = QtConcurrent::run([req] { return saveLabels(req); });
    // Keep painting the live raster while the worker writes.
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> ux(0, 2000), uy(0, 1400);
    for (int i = 0; i < 200; ++i)
        commitTo(raster, history, disc(ux(rng), uy(rng), 25), paintOp(PaintClass::Ink, i % 2 == 0, false));
    future.waitForFinished();
    const SaveOutcome out = future.result();
    QVERIFY2(out.ok, qPrintable(out.error));
    const LoadOutcome back = loadLabels(paths, key, Target::Trainer);
    QVERIFY(back.ok);
    QVERIFY(sameImage(fullImage(back.raster), expected));
}

QTEST_GUILESS_MAIN(TestInkLabels)
#include "test_ink_labels.moc"
