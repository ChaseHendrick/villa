#include "ink_labels/InkLabelRegions.hpp"

#include <QJsonObject>

#include <algorithm>
#include <set>

namespace vc3d::ink_labels {

const char* regionStatusName(RegionStatus status)
{
    switch (status) {
        case RegionStatus::Todo:
            return "todo";
        case RegionStatus::Done:
            return "done";
        case RegionStatus::Skip:
            return "skip";
    }
    return "todo";
}

std::optional<RegionStatus> regionStatusFromName(const QString& name)
{
    for (const RegionStatus s : {RegionStatus::Todo, RegionStatus::Done, RegionStatus::Skip})
        if (name == QLatin1String(regionStatusName(s)))
            return s;
    return std::nullopt;
}

QJsonArray regionsToJson(const RegionList& regions)
{
    QJsonArray out;
    for (const LabelRegion& r : regions) {
        QJsonObject o;
        o["id"] = r.id;
        o["x"] = r.rect.x;
        o["y"] = r.rect.y;
        o["width"] = r.rect.width;
        o["height"] = r.rect.height;
        o["note"] = QString::fromStdString(r.note);
        o["status"] = QLatin1String(regionStatusName(r.status));
        o["validation"] = r.validation;
        o["created_utc"] = QString::fromStdString(r.createdUtc);
        out.append(o);
    }
    return out;
}

RegionList regionsFromJson(const QJsonArray& array, cv::Size canvas)
{
    RegionList out;
    std::set<int> ids;
    for (const QJsonValue& value : array) {
        if (!value.isObject())
            continue;
        const QJsonObject o = value.toObject();
        LabelRegion r;
        r.id = o.value("id").toInt(0);
        r.rect = cv::Rect(o.value("x").toInt(), o.value("y").toInt(), o.value("width").toInt(), o.value("height").toInt());
        if (canvas.area() > 0)
            r.rect &= cv::Rect(cv::Point(0, 0), canvas);
        if (r.id <= 0 || r.rect.width <= 0 || r.rect.height <= 0 || !ids.insert(r.id).second)
            continue;
        r.note = o.value("note").toString().toStdString();
        r.status = regionStatusFromName(o.value("status").toString()).value_or(RegionStatus::Todo);
        r.validation = o.value("validation").toBool(false);
        r.createdUtc = o.value("created_utc").toString().toStdString();
        out.push_back(std::move(r));
    }
    return out;
}

int nextRegionId(const RegionList& regions)
{
    int id = 0;
    for (const LabelRegion& r : regions)
        id = std::max(id, r.id);
    return id + 1;
}

const LabelRegion* findRegion(const RegionList& regions, int id)
{
    const auto it = std::find_if(regions.begin(), regions.end(), [id](const LabelRegion& r) { return r.id == id; });
    return it == regions.end() ? nullptr : &*it;
}

}  // namespace vc3d::ink_labels
