#include "agent_bridge/AgentBridgeCommandCatalog.hpp"

#include "CommandCatalog.hpp"

#include <QJsonArray>

QJsonObject commandInfoToJson(const CommandInfo& command)
{
    const QString shortcut = command.shortcut.toString(QKeySequence::PortableText);
    QJsonObject result;
    result[QStringLiteral("id")] = command.id;
    result[QStringLiteral("title")] = command.title;
    result[QStringLiteral("category")] = command.category;
    result[QStringLiteral("shortcut")] = shortcut.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(shortcut);
    result[QStringLiteral("checkable")] = command.checkable;
    result[QStringLiteral("checked")] = command.checkable ? QJsonValue(command.checked) : QJsonValue(QJsonValue::Null);
    result[QStringLiteral("enabled")] = command.enabled;
    return result;
}

QJsonObject listCatalogCommands(const CommandCatalog& catalog)
{
    QJsonArray commands;
    for (const CommandInfo& command : catalog.commands()) {
        commands.append(commandInfoToJson(command));
    }
    return {{QStringLiteral("commands"), commands}};
}
