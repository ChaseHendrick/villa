#include "RegionOverlayController.hpp"

#include "../RegionController.hpp"
#include "../volume_viewers/VolumeViewerBase.hpp"

#include <QFont>

#include <cmath>
#include <limits>
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
    return !_regions->regions().empty() || _regions->draftBox().has_value();
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
}
