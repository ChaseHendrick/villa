#include "agent_bridge/AgentBridgeServer.hpp"


void AgentBridgeServer::registerCommandHandlers()
{
    registerMethod(
        {
            .name = QStringLiteral("command.list"),
            .errors = {-32010},
            .mcp = {
                .tool = QStringLiteral("vc3d_list_commands"),
            },
        },
        [this](const QJsonValue& p) { return handleCommandList(p); });
}
