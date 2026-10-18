#include "agent_bridge/AgentBridgeServer.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

#include "themes/ThemeManager.hpp"

namespace {

using vc3d::theme::ThemeManager;

QJsonArray themeListJson()
{
    QJsonArray themes;
    for (const auto& info : ThemeManager::instance().themes()) {
        QJsonObject theme;
        theme["id"] = info.id;
        theme["name"] = info.name;
        theme["author"] = info.author;
        theme["description"] = info.description;
        theme["source"] = info.source;
        theme["path"] = info.path.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(info.path);
        theme["colorScheme"] = info.colorScheme;
        theme["active"] = info.active;
        themes.append(theme);
    }
    return themes;
}

QJsonArray problemsJson(const vc3d::theme::ThemeProblems& problems)
{
    QJsonArray list;
    for (const auto& problem : problems) {
        QJsonObject entry;
        entry["file"] = problem.file;
        entry["message"] = problem.message;
        entry["warning"] = problem.warning;
        list.append(entry);
    }
    return list;
}

bool isThemePath(const QString& value)
{
    return value.contains(QLatin1Char('/')) || value.contains(QLatin1Char('\\')) ||
           value.endsWith(QLatin1String(vc3d::theme::kThemeFileSuffix), Qt::CaseInsensitive);
}

[[noreturn]] void throwUnknownTheme(const QString& id)
{
    QJsonObject data;
    data["kind"] = "theme";
    data["id"] = id;
    throw AgentBridgeError{-32007, QStringLiteral("Unknown theme: %1").arg(id), data};
}

}  // namespace


QJsonObject AgentBridgeServer::handleThemeList(const QJsonValue& params)
{
    auto& themes = ThemeManager::instance();
    if (params.toObject().value("rescan").toBool(false))
        themes.reloadThemes();
    QJsonObject result;
    result["themes"] = themeListJson();
    result["active"] = themes.currentThemeId();
    result["problems"] = problemsJson(themes.discoveryProblems());
    return result;
}


QJsonObject AgentBridgeServer::handleThemeSet(const QJsonValue& params)
{
    const QJsonObject p = params.toObject();
    const QString id = p.value("id").toString().trimmed();
    const bool persist = p.value("persist").toBool(true);
    auto& themes = ThemeManager::instance();
    if (id.isEmpty()) {
        QJsonObject data{{"param", "id"}};
        throw AgentBridgeError{-32602, "id must not be blank", data};
    }
    const bool path = isThemePath(id);
    if (path && !QDir::isAbsolutePath(id)) {
        QJsonObject data{{"param", "id"}};
        throw AgentBridgeError{-32602, "a theme path must be absolute", data};
    }
    if (path ? !QFileInfo::exists(id) : !themes.registry().contains(vc3d::theme::normalizeThemeId(id)))
        throwUnknownTheme(id);

    QString error;
    vc3d::theme::ThemeProblems warnings;
    if (!themes.setTheme(id, persist, &error, &warnings)) {
        // A path names a theme file that did not load; an installed theme
        // that cannot be applied is an application failure.
        QJsonObject data{{"detail", error}};
        if (path) {
            data["param"] = "id";
            throw AgentBridgeError{-32602, QStringLiteral("Invalid theme: %1").arg(id), data};
        }
        throw AgentBridgeError{-32005, QStringLiteral("theme.set failed"), data};
    }
    QJsonObject result;
    result["id"] = themes.currentThemeId();
    result["persisted"] = persist;
    result["warnings"] = problemsJson(warnings);
    return result;
}


QJsonObject AgentBridgeServer::handleThemeImport(const QJsonValue& params)
{
    const QJsonObject p = params.toObject();
    const QString path = p.value("path").toString().trimmed();
    const bool overwrite = p.value("overwrite").toBool(false);
    if (!QDir::isAbsolutePath(path)) {
        QJsonObject data{{"param", "path"}};
        throw AgentBridgeError{-32602, "path must be absolute", data};
    }
    if (!QFileInfo::exists(path)) {
        QJsonObject data{{"kind", "path"}, {"id", path}};
        throw AgentBridgeError{-32007, QStringLiteral("Theme not found: %1").arg(path), data};
    }

    const auto outcome = ThemeManager::instance().importTheme(path, overwrite);
    if (outcome.alreadyExists) {
        QJsonObject data{{"id", outcome.id}, {"path", outcome.installedPath}};
        throw AgentBridgeError{
            -32005,
            QStringLiteral("a theme with the id %1 is already installed; pass overwrite=true to replace it")
                .arg(outcome.id),
            data};
    }
    if (!outcome.ok) {
        QJsonObject data;
        data["param"] = "path";
        data["detail"] = vc3d::theme::formatProblems(outcome.problems);
        data["problems"] = problemsJson(outcome.problems);
        throw AgentBridgeError{-32602, QStringLiteral("Invalid theme: %1").arg(path), data};
    }
    QJsonObject result;
    result["id"] = outcome.id;
    result["path"] = outcome.installedPath;
    result["warnings"] = problemsJson(outcome.problems);
    return result;
}
