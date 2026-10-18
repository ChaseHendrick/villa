#include "agent_bridge/AgentBridgeServer.hpp"

namespace Params = AgentBridgeParams;


void AgentBridgeServer::registerThemeHandlers()
{
    registerMethod(
        {
            .name = QStringLiteral("theme.list"),
            .params = {
                Params::optionalBoolean(QStringLiteral("rescan"), false),
            },
            .mcp = {.tool = QStringLiteral("vc3d_list_themes")},
        },
        [this](const QJsonValue& p) { return handleThemeList(p); });

    registerMethod(
        {
            .name = QStringLiteral("theme.set"),
            .params = {
                Params::requiredString(QStringLiteral("id")),
                Params::optionalBoolean(QStringLiteral("persist"), true),
            },
            .errors = {-32602, -32007, -32005},
            .mcp = {.tool = QStringLiteral("vc3d_set_theme")},
        },
        [this](const QJsonValue& p) { return handleThemeSet(p); });

    registerMethod(
        {
            .name = QStringLiteral("theme.import"),
            .params = {
                Params::requiredString(QStringLiteral("path")),
                Params::optionalBoolean(QStringLiteral("overwrite"), false),
            },
            .errors = {-32602, -32007, -32005},
            .mcp = {.tool = QStringLiteral("vc3d_import_theme")},
        },
        [this](const QJsonValue& p) { return handleThemeImport(p); });
}
