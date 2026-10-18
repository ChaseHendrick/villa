#include "agent_bridge/AgentBridgeServer.hpp"

#include "CWindow.hpp"
#include "agent_bridge/AgentBridgeCommandCatalog.hpp"


QJsonObject AgentBridgeServer::handleCommandList(const QJsonValue&)
{
    CommandCatalog* catalog = _window ? _window->commandCatalog() : nullptr;
    if (!catalog) {
        QJsonObject data;
        data["detail"] = "main window is not available";
        throw AgentBridgeError{-32010, "Internal error", data};
    }
    return listCatalogCommands(*catalog);
}
