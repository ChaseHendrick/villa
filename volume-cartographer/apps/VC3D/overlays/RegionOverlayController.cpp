#include "RegionOverlayController.hpp"

#include "../RegionController.hpp"
#include "../volume_viewers/VolumeViewerBase.hpp"

#include <QFont>
#include <QPainterPath>

#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

namespace regions = vc3d::regions;

namespace
{

constexpr const char* kOverlayGroup = "region_overlays";
constexpr qreal kOutlineZ = 118.0;
constexpr qreal kActiveOutlineZ = 119.0;
constexpr qreal kHandleZ = 121.0;
constexpr qreal kLabelZ = 120.0;
constexpr qreal kHandleRadius = 4.5;

QPointF toScene(VolumeViewerBase* viewer, const cv::Vec3d& p)
{
    return viewer->volumeToScene(cv::Vec3f(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])));
}

}  // namespace

RegionOverlayController::RegionOverlayController(RegionController* regions, QObject* parent)
    : ViewerOverlayControllerBase(kOverlayGroup, parent), _regions(regions)
{
    if (_regions) {
        connect(_regions, &RegionController::regionsChanged, this, &RegionOverlayController::refreshAll);
        connect(_regions, &RegionController::activeRegionChanged, this, &RegionOverlayController::refreshAll);
        connect(_regions, &RegionController::toolChanged, this, &RegionOverlayController::refreshAll);
        connect(_regions, &RegionController::draftChanged, this, &RegionOverlayController::refreshAll);
    }
}

bool RegionOverlayController::isOverlayEnabledFor(VolumeViewerBase* viewer) const
{
    if (!_regions || !viewer || !RegionController::planeOf(viewer)) {
        return false;
    }
    return !_regions->regions().empty() || _regions->draftBox().has_value() || _regions->draftArc().has_value();
}

std::optional<QPointF> RegionOverlayController::drawArc(
    VolumeViewerBase* viewer,
    OverlayBuilder& builder,
    const regions::RayArc& arc,
    const regions::CompiledArc& compiled,
    const regions::FrameMap& map,
    const regions::Plane& plane,
    regions::PlaneKind kind,
    const ArcStyle& style)
{
    const double k = map.regionToViewer();
    const auto sceneOf = [&](const cv::Vec3d& regionPoint) { return toScene(viewer, regions::toViewer(regionPoint, map)); };
    const cv::Vec3d originRegion = regions::toRegion(plane.origin, map);
    if (kind == regions::PlaneKind::AxisZ) {
        const double zRegion = originRegion[2];
        const QPointF a = toScene(viewer, plane.origin);
        const QPointF b = toScene(viewer, plane.origin + cv::Vec3d(arc.radiusMax * k, 0.0, 0.0));
        const double radiusPx = std::max(1.0, std::hypot(b.x() - a.x(), b.y() - a.y()));
        // Chord error under a quarter pixel, at most half a degree per step.
        const double stepDeg = std::max(0.05, std::min(0.5, 2.0 * std::acos(std::max(-1.0, 1.0 - 0.25 / radiusPx)) * 180.0 / std::numbers::pi));
        std::optional<QPointF> anchor;
        for (const auto& path : regions::arcOutlineHorizontal(arc, compiled, zRegion, stepDeg)) {
            std::vector<QPointF> points;
            points.reserve(path.size());
            for (const auto& p : path) {
                points.push_back(sceneOf(p));
            }
            if (points.size() < 2) {
                continue;
            }
            if (!anchor) {
                anchor = points.front();
            }
            builder.addLineStrip(points, true, style.outline);
        }
        if (anchor && style.handles) {
            for (const auto& handle : regions::arcHandles(arc, compiled, zRegion)) {
                builder.addCircle(sceneOf(handle.regionPos), kHandleRadius, true, style.handle);
            }
        }
        return anchor;
    }
    if (kind != regions::PlaneKind::AxisX && kind != regions::PlaneKind::AxisY && kind != regions::PlaneKind::Vertical) {
        return std::nullopt;
    }
    const cv::Vec2d n(plane.normal[0], plane.normal[1]);
    const double length = std::hypot(n[0], n[1]);
    if (!(length > 0.0)) {
        return std::nullopt;
    }
    const cv::Vec2d h(-n[1] / length, n[0] / length);
    const cv::Vec2d originXY(originRegion[0], originRegion[1]);
    // One row per slice is exact; rows thinner than a scene pixel are
    // thinned to every m-th slice.
    const QPointF a = toScene(viewer, plane.origin);
    const QPointF b = toScene(viewer, plane.origin + cv::Vec3d(0.0, 0.0, k));
    const double pxPerSlice = std::hypot(b.x() - a.x(), b.y() - a.y());
    const int stride = pxPerSlice >= 1.0 || !(pxPerSlice > 0.0) ? 1 : static_cast<int>(std::ceil(1.0 / pxPerSlice));
    const auto segments = regions::arcOutlineVertical(arc, compiled, originXY, h, arc.zMin, arc.zMax, stride);
    if (segments.empty()) {
        return std::nullopt;
    }
    QPainterPath path;
    std::optional<QPointF> anchor;
    for (const auto& segment : segments) {
        const QPointF p0 = sceneOf(segment[0]);
        const QPointF p1 = sceneOf(segment[1]);
        path.moveTo(p0);
        path.lineTo(p1);
        for (const QPointF& p : {p0, p1}) {
            if (!anchor || p.y() < anchor->y() - 0.5 || (std::abs(p.y() - anchor->y()) <= 0.5 && p.x() < anchor->x())) {
                anchor = p;
            }
        }
    }
    OverlayStyle stroke = style.outline;
    stroke.brushColor = Qt::transparent;
    builder.addPainterPath(path, stroke);
    return anchor;
}

void RegionOverlayController::collectPrimitives(VolumeViewerBase* viewer, OverlayBuilder& builder)
{
    if (!_regions || !viewer) {
        return;
    }
    const auto plane = RegionController::planeOf(viewer);
    if (!plane) {
        return;
    }
    const regions::PlaneKind kind = regions::classifyPlane(*plane);
    const QString activeId = _regions->activeId();
    const QString draftId = _regions->draftRegionId();
    const auto draft = _regions->draftBox();
    const auto draftArc = _regions->draftArc();
    const bool toolsOn = _regions->tool() != RegionController::Tool::None || _regions->editing();

    QFont labelFont;
    labelFont.setPointSizeF(9.0);
    labelFont.setBold(true);

    for (const auto& loaded : _regions->regions()) {
        const QString id = QString::fromStdString(loaded.region.id);
        const regions::FrameCheck check = _regions->frameCheck(loaded.region);
        if (!check.map) {
            continue;
        }
        QColor color(QString::fromStdString(loaded.region.color));
        if (!color.isValid()) {
            color = QColor(255, 176, 0);
        }
        const bool active = id == activeId;
        const bool editingThis = !draftId.isEmpty() && draftId == id;
        OverlayStyle outline;
        outline.penColor = color;
        outline.penWidth = active ? 2.5 : 1.5;
        outline.z = active ? kActiveOutlineZ : kOutlineZ;
        outline.penJoin = Qt::MiterJoin;
        if (loaded.readOnly) {
            outline.penStyle = Qt::DashDotLine;
        }
        OverlayStyle handleStyle;
        handleStyle.penColor = QColor(20, 20, 20);
        handleStyle.penWidth = 1.2;
        handleStyle.brushColor = color;
        handleStyle.z = kHandleZ;
        const bool showHandles = active && toolsOn && !loaded.readOnly;

        std::optional<QPointF> anchor;
        if (const regions::Box* stored = regions::boxOf(loaded.region)) {
            const regions::Box box = editingThis && draft ? *draft : *stored;
            const auto section = regions::boxCrossSection(box, *check.map, *plane);
            if (section.empty()) {
                continue;
            }
            std::vector<QPointF> points;
            points.reserve(section.size());
            for (const auto& p : section) {
                points.push_back(toScene(viewer, p));
            }
            OverlayStyle boxOutline = outline;
            if (active) {
                QColor fill = color;
                fill.setAlpha(28);
                boxOutline.brushColor = fill;
            }
            builder.addLineStrip(points, true, boxOutline);
            // Label at the outline's top-left vertex in screen terms.
            anchor = points.front();
            for (const auto& p : points) {
                if (p.y() < anchor->y() - 0.5 || (std::abs(p.y() - anchor->y()) <= 0.5 && p.x() < anchor->x())) {
                    anchor = p;
                }
            }
            if (showHandles) {
                // The center handle moves the whole box: a hollow ring.
                OverlayStyle moveStyle = handleStyle;
                moveStyle.penColor = color;
                moveStyle.penWidth = 2.0;
                moveStyle.brushColor = QColor(20, 20, 20, 160);
                for (const auto& handle : regions::boxHandles(box, *check.map, *plane, kind)) {
                    const bool body = handle.kind == regions::HandleKind::Body;
                    builder.addCircle(toScene(viewer, handle.viewerPos), body ? kHandleRadius + 1.5 : kHandleRadius, true, body ? moveStyle : handleStyle);
                }
            }
        } else if (const regions::RayArc* stored = regions::arcOf(loaded.region)) {
            const auto compiled = _regions->compiledArc(loaded.region);
            if (!compiled) {
                continue;
            }
            const regions::RayArc arc = editingThis && draftArc ? *draftArc : *stored;
            anchor = drawArc(viewer, builder, arc, *compiled, *check.map, *plane, kind, ArcStyle{outline, handleStyle, showHandles});
        }
        if (!anchor) {
            continue;
        }
        QString label = QString::fromStdString(loaded.region.name);
        if (loaded.readOnly) {
            label += QStringLiteral(" (read-only)");
        }
        OverlayStyle text;
        text.penColor = color;
        text.z = kLabelZ;
        builder.addText(*anchor + QPointF(2.0, -18.0), label, labelFont, text, true);
    }

    OverlayStyle draftStyle;
    draftStyle.penColor = QColor(255, 255, 255);
    draftStyle.penWidth = 1.5;
    draftStyle.penStyle = Qt::DashLine;
    draftStyle.z = kActiveOutlineZ;

    // A new box being drawn.
    if (draft && draftId.isEmpty()) {
        if (const auto map = _regions->currentMap()) {
            const auto section = regions::boxCrossSection(*draft, *map, *plane);
            if (!section.empty()) {
                std::vector<QPointF> points;
                points.reserve(section.size());
                for (const auto& p : section) {
                    points.push_back(toScene(viewer, p));
                }
                OverlayStyle style = draftStyle;
                style.brushColor = QColor(255, 255, 255, 24);
                builder.addLineStrip(points, true, style);
            }
        }
    }
    // Where theta is measured from, while drawing or editing a ray arc: a
    // cross at the umbilicus center of this slice and a faint theta = 0 ray.
    const regions::Loaded* active = _regions->find(activeId);
    const bool arcContext = _regions->tool() == RegionController::Tool::RayArc || (_regions->editing() && active && regions::arcOf(active->region));
    if (arcContext && kind == regions::PlaneKind::AxisZ) {
        const auto map = _regions->currentMap();
        const auto compiled = _regions->currentUmbilicusCompiled();
        const auto frame = _regions->currentFrame();
        if (map && compiled && frame) {
            const cv::Vec3d originRegion = regions::toRegion(plane->origin, *map);
            const cv::Vec2d c = compiled->centerAt(originRegion[2]);
            const cv::Vec2d seam = compiled->direction(0.0);
            const cv::Vec3i extent = regions::frameExtentXyz(*frame);
            const double reach = std::hypot(static_cast<double>(extent[0]), static_cast<double>(extent[1]));
            const QPointF center = toScene(viewer, regions::toViewer(cv::Vec3d(c[0], c[1], originRegion[2]), *map));
            const QPointF rayEnd =
                toScene(viewer, regions::toViewer(cv::Vec3d(c[0] + seam[0] * reach, c[1] + seam[1] * reach, originRegion[2]), *map));
            OverlayStyle reference;
            reference.penColor = QColor(255, 255, 255, 150);
            reference.penWidth = 1.5;
            reference.z = kOutlineZ;
            builder.addLineStrip({center + QPointF(-7.0, 0.0), center + QPointF(7.0, 0.0)}, false, reference);
            builder.addLineStrip({center + QPointF(0.0, -7.0), center + QPointF(0.0, 7.0)}, false, reference);
            OverlayStyle ray = reference;
            ray.penColor = QColor(255, 255, 255, 90);
            ray.penStyle = Qt::DotLine;
            builder.addLineStrip({center, rayEnd}, false, ray);
        }
    }

    // A new ray arc between its first click and the cursor.
    if (draftArc && draftId.isEmpty()) {
        const auto map = _regions->currentMap();
        const auto compiled = _regions->currentUmbilicusCompiled();
        if (map && compiled) {
            drawArc(viewer, builder, *draftArc, *compiled, *map, *plane, kind, ArcStyle{draftStyle, draftStyle, false});
        }
    }
}
