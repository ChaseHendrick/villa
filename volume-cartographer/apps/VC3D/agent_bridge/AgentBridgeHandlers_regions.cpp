#include "agent_bridge/AgentBridgeServer.hpp"
#include "agent_bridge/AgentBridgeInternal.hpp"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "CState.hpp"
#include "CWindow.hpp"
#include "RegionController.hpp"

#include "vc/core/types/Volume.hpp"

// The regions.* family: thin handlers over RegionController, the domain
// controller the Regions dock also uses. Coordinates on the wire are the
// current volume's voxel coordinates, as for every bridge method; regions are
// stored in their annotation frame and converted with the region's frame map.

namespace regions = vc3d::regions;

namespace
{

const char* const kServerOwned[] =
    {"revision",
     "author",
     "created",
     "modified",
     "frame",
     "readOnly",
     "readOnlyReason",
     "compatible",
     "incompatibleReason",
     "computed",
     "umbilicus",
     "shapeFrame"};

[[noreturn]] void throwUnknownRegion(const QString& id)
{
    QJsonObject data;
    data["kind"] = "region";
    data["id"] = id;
    throw AgentBridgeError{-32007, QStringLiteral("Unknown region: %1").arg(id), data};
}

// "shape.theta_start_deg" -> "shape.thetaStartDeg": file paths in validation
// messages become the wire names the caller sent.
QString wireParam(const QString& path)
{
    if (path.startsWith(QStringLiteral("shape.umbilicus"))) {
        return QStringLiteral("shape");
    }
    QString out;
    bool upper = false;
    for (const QChar c : path) {
        if (c == QLatin1Char('_')) {
            upper = true;
            continue;
        }
        out.append(upper ? c.toUpper() : c);
        upper = false;
    }
    return out;
}

[[noreturn]] void throwResult(const RegionController::Result& result)
{
    using Code = RegionController::Code;
    QJsonObject data;
    switch (result.code) {
        case Code::NoProject:
            throw AgentBridgeError{-32000, result.message, data};
        case Code::NoVolume:
            throw AgentBridgeError{-32001, result.message, data};
        case Code::Invalid:
            data["param"] = wireParam(result.param);
            throw AgentBridgeError{-32602, result.message, data};
        case Code::NotFound:
            if (result.param == QLatin1String("volumeId")) {
                data["kind"] = "volume";
            } else {
                data["kind"] = "region";
            }
            throw AgentBridgeError{-32007, result.message, data};
        case Code::Conflict:
            data["param"] = result.param.isEmpty() ? QStringLiteral("expectedRevision") : result.param;
            if (result.currentRevision) {
                data["currentRevision"] = static_cast<double>(*result.currentRevision);
            }
            throw AgentBridgeError{-32602, result.message, data};
        case Code::ReadOnly:
        case Code::Incompatible:
            throw AgentBridgeError{-32009, result.message, data};
        case Code::JobRunning:
            data["source"] = "region";
            throw AgentBridgeError{-32004, result.message, data};
        case Code::Busy:
        case Code::Io:
            data["detail"] = result.message;
            throw AgentBridgeError{-32005, result.message, data};
        case Code::Ok:
            break;
    }
    data["detail"] = result.message;
    throw AgentBridgeError{-32010, QStringLiteral("Internal error"), data};
}

QJsonObject point(const cv::Vec3d& p)
{
    QJsonObject o;
    o["x"] = p[0];
    o["y"] = p[1];
    o["z"] = p[2];
    return o;
}

cv::Vec3d pointFrom(const QJsonValue& value)
{
    const QJsonObject o = value.toObject();
    return {o.value("x").toDouble(), o.value("y").toDouble(), o.value("z").toDouble()};
}

// The wire form of a region. With a frame map the shape is in the current
// volume's voxel coordinates (shapeFrame "volume"); without one (a region
// that does not fit the volume) it stays in its own frame (shapeFrame
// "region").
QJsonObject regionDoc(const RegionController& controller, const regions::Region& region, bool readOnly, const std::string& readOnlyReason, bool withUmbilicus)
{
    QJsonObject doc;
    doc["id"] = QString::fromStdString(region.id);
    doc["name"] = QString::fromStdString(region.name);
    doc["color"] = QString::fromStdString(region.color);
    QJsonArray tags;
    for (const auto& tag : region.tags) {
        tags.append(QString::fromStdString(tag));
    }
    doc["tags"] = tags;
    doc["status"] = QString::fromStdString(std::string(regions::statusName(region.status)));
    doc["notes"] = QString::fromStdString(region.notes);
    doc["author"] = QString::fromStdString(region.author);
    doc["created"] = QString::fromStdString(region.created);
    doc["modified"] = QString::fromStdString(region.modified);
    doc["revision"] = static_cast<double>(region.revision);
    QJsonObject frame;
    frame["coordinateBaseShapeZyx"] = QJsonArray{
        static_cast<double>(region.frame.baseShapeZyx[0]),
        static_cast<double>(region.frame.baseShapeZyx[1]),
        static_cast<double>(region.frame.baseShapeZyx[2])};
    if (region.frame.voxelSizeUm) {
        frame["voxelSizeUm"] = *region.frame.voxelSizeUm;
    }
    frame["volumeId"] = QString::fromStdString(region.frame.volumeId);
    frame["coordinateSpace"] = QString::fromStdString(region.frame.coordinateSpace);
    doc["frame"] = frame;

    const regions::FrameCheck check = controller.frameCheck(region);
    const double k = check.map ? check.map->regionToViewer() : 1.0;
    doc["shapeFrame"] = check.map ? QStringLiteral("volume") : QStringLiteral("region");
    QJsonObject shape;
    if (const regions::Box* box = regions::boxOf(region)) {
        shape["type"] = "box";
        shape["min"] = point(cv::Vec3d(box->min[0], box->min[1], box->min[2]) * k);
        shape["max"] = point(cv::Vec3d(box->max[0], box->max[1], box->max[2]) * k);
    } else if (const regions::RayArc* arc = regions::arcOf(region)) {
        shape["type"] = "ray_arc";
        shape["thetaStartDeg"] = arc->thetaStartDeg;
        shape["thetaSweepDeg"] = arc->thetaSweepDeg;
        shape["radiusMin"] = arc->radiusMin * k;
        shape["radiusMax"] = arc->radiusMax * k;
        shape["zMin"] = arc->zMin * k;
        shape["zMax"] = arc->zMax * k;
        QJsonObject umbilicus;
        umbilicus["sourcePath"] = QString::fromStdString(arc->umbilicus.sourcePath);
        umbilicus["scale"] = arc->umbilicus.scale;
        umbilicus["scaleSource"] = QString::fromStdString(arc->umbilicus.scaleSource);
        umbilicus["seam"] = QString::fromStdString(arc->umbilicus.seam);
        umbilicus["controlPointCount"] = static_cast<double>(arc->umbilicus.controlPoints.size());
        if (withUmbilicus) {
            QJsonArray points;
            for (const auto& c : arc->umbilicus.controlPoints) {
                points.append(point(c * k));
            }
            umbilicus["controlPoints"] = points;
        }
        doc["umbilicus"] = umbilicus;
    }
    doc["shape"] = shape;
    doc["readOnly"] = readOnly;
    if (readOnly) {
        doc["readOnlyReason"] = QString::fromStdString(readOnlyReason);
    }
    doc["compatible"] = check.map.has_value();
    if (!check.map) {
        doc["incompatibleReason"] = QString::fromStdString(check.reason);
    }

    QJsonObject computed;
    if (const auto bounds = controller.regionBounds(region)) {
        QJsonObject b;
        b["min"] = point(bounds->at(0) * k);
        b["max"] = point(bounds->at(1) * k);
        computed["bounds"] = b;
        computed["center"] = point(0.5 * (bounds->at(0) + bounds->at(1)) * k);
    }
    // Where theta and r are measured from: the arc's umbilicus center at its
    // middle slice.
    if (const regions::RayArc* arc = regions::arcOf(region)) {
        if (const auto compiled = controller.compiledArc(region)) {
            const double z = 0.5 * (arc->zMin + arc->zMax);
            const cv::Vec2d c = compiled->centerAt(z);
            computed["umbilicusCenter"] = point(cv::Vec3d(c[0], c[1], z) * k);
        }
    }
    const QString id = QString::fromStdString(region.id);
    if (check.map && controller.find(id)) {
        const auto estimates = controller.levelEstimates(id, QString());
        QJsonArray levels;
        for (const auto& estimate : estimates) {
            QJsonObject entry;
            entry["level"] = estimate.level;
            entry["voxels"] = static_cast<double>(estimate.voxels);
            entry["bytes"] = static_cast<double>(estimate.bytes);
            levels.append(entry);
            if (estimate.level == 0) {
                computed["voxelEstimateLevel0"] = static_cast<double>(estimate.voxels);
            }
        }
        computed["bytesPerLevel"] = levels;
        computed["autoLevel"] = RegionController::autoLevel(estimates);
    }
    QJsonArray stats;
    for (const auto& saved : controller.savedStats(id)) {
        QJsonObject entry;
        entry["volumeId"] = saved.volumeId;
        entry["level"] = saved.level;
        entry["stale"] = saved.stale;
        entry["path"] = saved.path;
        stats.append(entry);
    }
    computed["stats"] = stats;
    doc["computed"] = computed;
    return doc;
}

QJsonObject regionDoc(const RegionController& controller, const regions::Loaded& loaded, bool withUmbilicus)
{
    return regionDoc(controller, loaded.region, loaded.readOnly, loaded.readOnlyReason, withUmbilicus);
}

void requireInVolume(const cv::Vec3d& p, const std::array<int, 3>& shapeXyz, const char* param)
{
    const cv::Vec3f f(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
    if (!volumePointInBounds(f, shapeXyz)) {
        QJsonObject data;
        data["param"] = QString::fromLatin1(param);
        throw AgentBridgeError{
            -32003,
            QStringLiteral("%1 (%2, %3, %4) is outside the current volume (%5 x %6 x %7)")
                .arg(QLatin1String(param))
                .arg(p[0])
                .arg(p[1])
                .arg(p[2])
                .arg(shapeXyz[0])
                .arg(shapeXyz[1])
                .arg(shapeXyz[2]),
            data};
    }
}

}  // namespace

RegionController* AgentBridgeServer::regionController() const
{
    CState* state = _window ? _window->_state : nullptr;
    if (!state || !state->hasVpkg()) {
        throw AgentBridgeError{-32000, "No volume package loaded", {}};
    }
    RegionController* controller = _window->_regionController.get();
    if (!controller) {
        QJsonObject data;
        data["detail"] = "region controller is unavailable";
        throw AgentBridgeError{-32010, "Internal error", data};
    }
    controller->syncDirectory();
    if (controller->directory().isEmpty()) {
        throw AgentBridgeError{-32000, QStringLiteral("Save the project first; regions are stored next to the project file."), {}};
    }
    return controller;
}

QJsonObject AgentBridgeServer::handleRegionsList(const QJsonValue& params)
{
    const QJsonObject p = params.toObject();
    RegionController* controller = regionController();
    const bool full = p.value("full").toBool(false);
    QJsonArray list;
    if (p.contains("id")) {
        const QString id = p.value("id").toString();
        const regions::Loaded* loaded = controller->find(id);
        if (!loaded) {
            throwUnknownRegion(id);
        }
        list.append(regionDoc(*controller, *loaded, true));
    } else {
        for (const auto& loaded : controller->regions()) {
            list.append(regionDoc(*controller, loaded, full));
        }
    }
    QJsonArray loadErrors;
    for (const auto& error : controller->loadErrors()) {
        QJsonObject entry;
        entry["file"] = QString::fromStdString(error.file);
        entry["path"] = QString::fromStdString(error.path);
        entry["message"] = QString::fromStdString(error.message);
        loadErrors.append(entry);
    }
    QJsonObject result;
    result["directory"] = controller->directory();
    result["regions"] = list;
    result["loadErrors"] = loadErrors;
    return result;
}

QJsonObject AgentBridgeServer::handleRegionsSave(const QJsonValue& params)
{
    const QJsonObject p = params.toObject();
    for (const char* key : kServerOwned) {
        if (p.contains(QLatin1String(key))) {
            throwParamError(key, QStringLiteral("is set by VC3D and cannot be supplied"));
        }
    }
    const QJsonObject shapeIn = p.value("shape").toObject();
    if (shapeIn.contains(QStringLiteral("umbilicus"))) {
        QJsonObject data;
        data["param"] = "shape.umbilicus";
        throw AgentBridgeError{-32602, QStringLiteral("shape.umbilicus is set by VC3D (it embeds the project's umbilicus) and cannot be supplied"), data};
    }
    RegionController* controller = regionController();
    CState* state = _window->_state;
    const auto volume = state ? state->currentVolume() : nullptr;
    if (!volume) {
        throw AgentBridgeError{-32001, "No current volume", {}};
    }
    const std::array<int, 3> shapeXyz = volume->shapeXyz();

    const bool creating = !p.contains("id");
    const bool dryRun = p.value("dryRun").toBool(false);
    regions::Region next;
    std::optional<std::uint64_t> expectedRevision;
    if (p.contains("expectedRevision")) {
        expectedRevision = static_cast<std::uint64_t>(p.value("expectedRevision").toDouble());
    }
    // The frame map turns wire coordinates (current volume voxels) into the
    // region frame.
    regions::FrameMap map;
    if (creating) {
        if (!p.contains("shape")) {
            throwParamError("shape", QStringLiteral("is required to create a region"));
        }
        if (expectedRevision) {
            throwParamError("expectedRevision", QStringLiteral("only applies to an update (give id)"));
        }
        const auto current = controller->currentMap();
        if (!current) {
            throw AgentBridgeError{-32009, controller->unavailableReason(), {}};
        }
        map = *current;
        // Name and color default like the GUI's ("Region N", the palette).
        next.color.clear();
    } else {
        const QString id = p.value("id").toString();
        const regions::Loaded* loaded = controller->find(id);
        if (!loaded) {
            throwUnknownRegion(id);
        }
        const regions::FrameCheck check = controller->frameCheck(loaded->region);
        if (!check.map) {
            throw AgentBridgeError{
                -32009, QStringLiteral("Region %1 does not fit the current volume: %2").arg(id, QString::fromStdString(check.reason)), {}};
        }
        next = loaded->region;
        map = *check.map;
    }

    if (p.contains("name")) {
        next.name = p.value("name").toString().trimmed().toStdString();
    }
    if (p.contains("color")) {
        next.color = p.value("color").toString().toStdString();
        if (!regions::isValidColor(next.color)) {
            throwParamError("color", QStringLiteral("must be a color like \"#ffb000\""));
        }
    }
    if (p.contains("tags")) {
        next.tags.clear();
        for (const QJsonValue& tag : p.value("tags").toArray()) {
            const QString text = tag.toString().trimmed();
            if (text.isEmpty()) {
                throwParamError("tags", QStringLiteral("must not contain empty strings"));
            }
            next.tags.push_back(text.toStdString());
        }
    }
    if (p.contains("status")) {
        next.status = *regions::parseStatus(p.value("status").toString().toStdString());
    }
    if (p.contains("notes")) {
        next.notes = p.value("notes").toString().toStdString();
    }

    if (p.contains("shape")) {
        const QString type = shapeIn.value("type").toString();
        const double toRegion = map.viewerToRegion();
        const bool sameType = !creating && QString::fromStdString(std::string(regions::shapeTypeName(next.shape))) == type;
        const auto need = [&](const char* key) {
            if (!shapeIn.contains(QLatin1String(key))) {
                throwParamError((std::string("shape.") + key).c_str(), sameType ? QStringLiteral("is required") : QStringLiteral("is required when the shape type is set or changed"));
            }
        };
        if (type == QLatin1String("box")) {
            if (shapeIn.contains(QStringLiteral("seam"))) {
                throwParamError("shape.seam", QStringLiteral("applies to a ray arc only"));
            }
            regions::Box box = sameType ? *regions::boxOf(next) : regions::Box{};
            if (!sameType) {
                need("min");
                need("max");
            }
            for (const char* key : {"min", "max"}) {
                if (!shapeIn.contains(QLatin1String(key))) {
                    continue;
                }
                const cv::Vec3d v = pointFrom(shapeIn.value(QLatin1String(key)));
                requireInVolume(v, shapeXyz, (std::string("shape.") + key).c_str());
                cv::Vec3i& target = std::string(key) == "min" ? box.min : box.max;
                for (int axis = 0; axis < 3; ++axis) {
                    target[axis] = static_cast<int>(std::llround(v[axis] * toRegion));
                }
            }
            next.shape = box;
        } else {
            regions::RayArc arc = sameType ? *regions::arcOf(next) : regions::RayArc{};
            if (!sameType) {
                for (const char* key : {"thetaStartDeg", "thetaSweepDeg", "radiusMin", "radiusMax", "zMin", "zMax"}) {
                    need(key);
                }
                // A new arc's theta runs from +x unless the caller names
                // another seam; the Regions dock's seam choice does not apply.
                const std::string seam = shapeIn.value("seam").toString(QStringLiteral("+x")).toStdString();
                if (!controller->embedProjectUmbilicus(arc, map, seam)) {
                    QJsonObject data;
                    data["kind"] = "umbilicus";
                    throw AgentBridgeError{-32007, controller->umbilicus().message, data};
                }
            } else if (shapeIn.contains(QStringLiteral("seam"))) {
                arc.umbilicus.seam = shapeIn.value("seam").toString().toStdString();
            }
            const auto number = [&](const char* key, double fallback) {
                return shapeIn.contains(QLatin1String(key)) ? shapeIn.value(QLatin1String(key)).toDouble() : fallback;
            };
            arc.thetaStartDeg = number("thetaStartDeg", arc.thetaStartDeg);
            arc.thetaSweepDeg = number("thetaSweepDeg", arc.thetaSweepDeg);
            arc.radiusMin = shapeIn.contains("radiusMin") ? shapeIn.value("radiusMin").toDouble() * toRegion : arc.radiusMin;
            arc.radiusMax = shapeIn.contains("radiusMax") ? shapeIn.value("radiusMax").toDouble() * toRegion : arc.radiusMax;
            for (const char* key : {"zMin", "zMax"}) {
                if (!shapeIn.contains(QLatin1String(key))) {
                    continue;
                }
                const double z = shapeIn.value(QLatin1String(key)).toDouble();
                if (z < 0.0 || z >= shapeXyz[2]) {
                    QJsonObject data;
                    data["param"] = QStringLiteral("shape.%1").arg(QLatin1String(key));
                    throw AgentBridgeError{
                        -32003,
                        QStringLiteral("shape.%1 %2 is outside the current volume (0..%3)").arg(QLatin1String(key)).arg(z).arg(shapeXyz[2] - 1),
                        data};
                }
                (std::string(key) == "zMin" ? arc.zMin : arc.zMax) = static_cast<int>(std::llround(z * toRegion));
            }
            next.shape = arc;
        }
    }

    const RegionController::Result result =
        creating ? controller->create(std::move(next), dryRun) : controller->update(std::move(next), expectedRevision, dryRun);
    if (!result.ok() || !result.region) {
        throwResult(result);
    }
    if (dryRun) {
        QJsonObject doc = regionDoc(*controller, *result.region, false, {}, true);
        doc["dryRun"] = true;
        return doc;
    }
    const regions::Loaded* saved = controller->find(QString::fromStdString(result.region->id));
    if (!saved) {
        throw AgentBridgeError{-32005, QStringLiteral("The region was saved but could not be read back"), {}};
    }
    return regionDoc(*controller, *saved, true);
}

QJsonObject AgentBridgeServer::handleRegionsDelete(const QJsonValue& params)
{
    const QJsonObject p = params.toObject();
    RegionController* controller = regionController();
    const QString id = p.value("id").toString();
    if (!controller->find(id)) {
        throwUnknownRegion(id);
    }
    std::optional<std::uint64_t> expectedRevision;
    if (p.contains("expectedRevision")) {
        expectedRevision = static_cast<std::uint64_t>(p.value("expectedRevision").toDouble());
    }
    const RegionController::Result result = controller->remove(id, expectedRevision);
    if (!result.ok()) {
        throwResult(result);
    }
    QJsonObject out;
    out["deleted"] = true;
    out["id"] = id;
    out["trashPath"] = QDir(controller->directory()).filePath(QStringLiteral(".trash/%1%2").arg(id, QLatin1String(regions::kFileSuffix.data())));
    return out;
}

QJsonObject AgentBridgeServer::handleRegionsStats(const QJsonValue& params)
{
    const QJsonObject p = params.toObject();
    RegionController* controller = regionController();
    requireSourceIdle(QStringLiteral("region"));
    RegionController::StatsJob job;
    for (const QJsonValue& id : p.value("ids").toArray()) {
        job.ids << id.toString();
    }
    job.volumeId = p.value("volumeId").toString();
    const QJsonValue level = p.value("level");
    if (level.isDouble()) {
        job.level = static_cast<int>(level.toDouble());
        if (job.level < 0) {
            throwParamError("level", QStringLiteral("must be \"auto\" or a stored level (0 or more)"));
        }
    } else if (level.isString() && level.toString() != QLatin1String("auto")) {
        throwParamError("level", QStringLiteral("must be \"auto\" or a stored level (0 or more)"));
    }
    if (p.contains("threshold")) {
        job.threshold = p.value("threshold").toDouble();
    }
    if (p.contains("percentiles")) {
        job.percentiles.clear();
        for (const QJsonValue& value : p.value("percentiles").toArray()) {
            job.percentiles.push_back(value.toDouble());
        }
    }
    for (const QString& id : job.ids) {
        if (!controller->find(id)) {
            throwUnknownRegion(id);
        }
    }
    const RegionController::Result result = controller->startStats(job);
    if (!result.ok()) {
        throwResult(result);
    }
    // RegionController::jobStarted registered the job (handleRegionJobStarted).
    QJsonObject out;
    out["jobId"] = activeJobId(QStringLiteral("region"));
    out["kind"] = QString::fromLatin1(RegionController::kStatsJobKind);
    out["source"] = "region";
    return out;
}

QJsonObject AgentBridgeServer::handleRegionsExport(const QJsonValue& params)
{
    const QJsonObject p = params.toObject();
    RegionController* controller = regionController();
    requireSourceIdle(QStringLiteral("region"));
    const QString id = p.value("id").toString();
    if (!controller->find(id)) {
        throwUnknownRegion(id);
    }
    const QString volumeId = p.value("volumeId").toString();
    const QJsonValue level = p.value("level");
    int chosen = -1;
    if (level.isDouble()) {
        chosen = static_cast<int>(level.toDouble());
        if (chosen < 0) {
            throwParamError("level", QStringLiteral("must be \"auto\" or a stored level (0 or more)"));
        }
    } else if (level.isString() && level.toString() != QLatin1String("auto")) {
        throwParamError("level", QStringLiteral("must be \"auto\" or a stored level (0 or more)"));
    }
    if (chosen < 0) {
        chosen = controller->autoLevel(id, volumeId);
    }
    const QString outputDir = p.value("outputDir").toString();
    if (!outputDir.isEmpty() && QDir::isRelativePath(outputDir)) {
        throwParamError("outputDir", QStringLiteral("must be an absolute path"));
    }
    const RegionController::Result result = controller->startExport(id, volumeId, chosen, outputDir);
    if (!result.ok()) {
        throwResult(result);
    }
    QJsonObject out;
    out["jobId"] = activeJobId(QStringLiteral("region"));
    out["kind"] = QString::fromLatin1(RegionController::kExportJobKind);
    out["source"] = "region";
    out["level"] = chosen;
    return out;
}

// --- Region job lifecycle -> source:"region" jobs ---

void AgentBridgeServer::handleRegionJobStarted(const QString& kind, const QString& label)
{
    // Started from the Regions dock, regions.stats or regions.export alike:
    // the controller runs one job at a time, so its lifecycle is the job's
    // authority, and an agent can see and cancel a dock-started job too.
    if (!_activeJobs.contains(QStringLiteral("region"))) {
        beginJob(QStringLiteral("region"), kind, label, /*broadcastStart=*/true);
    }
}

void AgentBridgeServer::handleRegionJobProgress(double fraction)
{
    auto it = _activeJobs.find(QStringLiteral("region"));
    if (it == _activeJobs.end()) {
        return;
    }
    JobRecord& job = it.value();
    const QString message = QStringLiteral("%1 (%2%)").arg(job.label).arg(qRound(std::clamp(fraction, 0.0, 1.0) * 100.0));
    job.message = message;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - _lastConsoleBroadcastMs < 100) {
        return;
    }
    _lastConsoleBroadcastMs = now;
    broadcastJobProgress(job, QStringLiteral("output"), message);
}

void AgentBridgeServer::handleRegionJobFinished(bool success, const QString& message, const QJsonObject& result, const QStringList& outputPaths)
{
    auto it = _activeJobs.find(QStringLiteral("region"));
    if (it == _activeJobs.end()) {
        return;
    }
    it->resultJson = result;
    finishJob(QStringLiteral("region"), success, message, outputPaths.value(0));
}
