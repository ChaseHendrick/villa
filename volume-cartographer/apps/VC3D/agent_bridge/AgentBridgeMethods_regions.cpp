#include "agent_bridge/AgentBridgeServer.hpp"

namespace Params = AgentBridgeParams;
namespace Mcp = AgentBridgeMcpTools;

namespace {

AgentBridgeParam point3(const QString& name)
{
    return Params::optionalObject(
        name,
        {
            Params::requiredNumber(QStringLiteral("x")),
            Params::requiredNumber(QStringLiteral("y")),
            Params::requiredNumber(QStringLiteral("z")),
        });
}

// Every shape field is declared so a shape validates mechanically; which
// fields a given type needs, and the patch rules, are the handler's.
AgentBridgeParam shape()
{
    return Params::optionalObject(
        QStringLiteral("shape"),
        {
            Params::requiredStringEnum(QStringLiteral("type"), {QStringLiteral("box"), QStringLiteral("ray_arc")}),
            point3(QStringLiteral("min")),
            point3(QStringLiteral("max")),
            Params::optionalNumber(QStringLiteral("thetaStartDeg")),
            Params::optionalNumber(QStringLiteral("thetaSweepDeg")),
            Params::optionalNumber(QStringLiteral("radiusMin")),
            Params::optionalNumber(QStringLiteral("radiusMax")),
            Params::optionalNumber(QStringLiteral("zMin")),
            Params::optionalNumber(QStringLiteral("zMax")),
            Params::optionalStringEnum(QStringLiteral("seam"), {QStringLiteral("+x"), QStringLiteral("-x"), QStringLiteral("+y"), QStringLiteral("-y")}),
        });
}

// A stored pyramid level, or "auto" (also the meaning of an omitted level);
// the handler checks the level exists and the string is "auto".
AgentBridgeParam level()
{
    AgentBridgeParam param = Params::optionalInteger(QStringLiteral("level"));
    param.minimum.reset();
    param.maximum.reset();
    param.alternateTypes = {AgentBridgeParamType::String};
    return param;
}

}  // namespace

void AgentBridgeServer::registerRegionHandlers()
{
    registerMethod(
        {
            .name = QStringLiteral("regions.list"),
            .params = {
                Params::optionalString(QStringLiteral("id")),
                Params::optionalBoolean(QStringLiteral("full"), false),
            },
            .errors = {-32602, -32000, -32007},
            .mcp = Mcp::snakeCase(QStringLiteral("vc3d_list_regions")),
        },
        [this](const QJsonValue& p) { return handleRegionsList(p); });

    registerMethod(
        {
            .name = QStringLiteral("regions.save"),
            .params = {
                Params::optionalString(QStringLiteral("id")),
                Params::optionalSafeId(QStringLiteral("expectedRevision")),
                Params::optionalString(QStringLiteral("name")),
                Params::optionalString(QStringLiteral("color")),
                Params::optionalArray(QStringLiteral("tags"), AgentBridgeParamType::String),
                Params::optionalStringEnum(QStringLiteral("status"), {QStringLiteral("open"), QStringLiteral("resolved")}),
                Params::optionalString(QStringLiteral("notes")),
                shape(),
                Params::optionalBoolean(QStringLiteral("dryRun"), false),
            },
            .errors = {-32602, -32000, -32001, -32003, -32005, -32007, -32009},
            .mcp = Mcp::snakeCase(QStringLiteral("vc3d_save_region")),
        },
        [this](const QJsonValue& p) { return handleRegionsSave(p); });

    registerMethod(
        {
            .name = QStringLiteral("regions.delete"),
            .params = {
                Params::requiredString(QStringLiteral("id")),
                Params::optionalSafeId(QStringLiteral("expectedRevision")),
            },
            .errors = {-32602, -32000, -32005, -32007},
            .mcp = Mcp::snakeCase(QStringLiteral("vc3d_delete_region")),
        },
        [this](const QJsonValue& p) { return handleRegionsDelete(p); });

    registerMethod(
        {
            .name = QStringLiteral("regions.stats"),
            .params = {
                Params::requiredArray(QStringLiteral("ids"), AgentBridgeParamType::String),
                Params::optionalString(QStringLiteral("volumeId")),
                level(),
                Params::optionalNumber(QStringLiteral("threshold")),
                Params::optionalArray(
                    QStringLiteral("percentiles"),
                    AgentBridgeParam{
                        .type = AgentBridgeParamType::Number,
                        .finite = true,
                        .minimum = 0.0,
                        .maximum = 100.0,
                    }),
            },
            .errors = {-32602, -32000, -32001, -32004, -32005, -32007, -32009},
            .mcp = Mcp::snakeCase(QStringLiteral("vc3d_region_stats"), {QStringLiteral("wait")}),
        },
        [this](const QJsonValue& p) { return handleRegionsStats(p); });

    registerMethod(
        {
            .name = QStringLiteral("regions.export"),
            .params = {
                Params::requiredString(QStringLiteral("id")),
                Params::optionalString(QStringLiteral("volumeId")),
                level(),
                Params::optionalString(QStringLiteral("outputDir")),
            },
            .errors = {-32602, -32000, -32001, -32004, -32005, -32007, -32009},
            .mcp = Mcp::snakeCase(QStringLiteral("vc3d_export_region"), {QStringLiteral("wait")}),
        },
        [this](const QJsonValue& p) { return handleRegionsExport(p); });
}
