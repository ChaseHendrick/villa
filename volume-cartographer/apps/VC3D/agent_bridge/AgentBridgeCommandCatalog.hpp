#pragma once

// Bridge view of the GUI's command catalog (command.list). Kept free of
// AgentBridgeServer so the reply can be tested on its own.

#include <QJsonObject>

class CommandCatalog;
struct CommandInfo;

// {id, title, category, shortcut, checkable, checked, enabled}. shortcut is
// portable text such as "Ctrl+K", or null; checked is null unless the
// command is checkable.
QJsonObject commandInfoToJson(const CommandInfo& command);

// {commands: [...]} in catalog order.
QJsonObject listCatalogCommands(const CommandCatalog& catalog);
