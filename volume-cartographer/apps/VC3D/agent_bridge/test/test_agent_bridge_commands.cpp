// command.list over a command catalog of real QActions.

#include "CommandCatalog.hpp"
#include "agent_bridge/AgentBridgeCommandCatalog.hpp"

#include <QAction>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QtTest/QtTest>

class AgentBridgeCommandsTest : public QObject
{
    Q_OBJECT

private slots:
    void listReportsEveryCommandInCatalogOrder()
    {
        CommandCatalog catalog;
        QAction palette(QStringLiteral("Command &Palette..."));
        palette.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
        QAction bbox(QStringLiteral("Draw &BBox"));
        bbox.setCheckable(true);
        bbox.setChecked(true);
        QAction disabled(QStringLiteral("Zoom to fit segment"));
        disabled.setEnabled(false);
        catalog.add(QStringLiteral("command_palette"), &palette, QStringLiteral("Help"));
        catalog.add(QStringLiteral("draw_bbox"), &bbox, QStringLiteral("Actions"));
        catalog.add(QStringLiteral("zoom_to_fit"), &disabled, QStringLiteral("Viewer Controls"));

        const QJsonArray commands = listCatalogCommands(catalog).value(QStringLiteral("commands")).toArray();
        QCOMPARE(commands.size(), 3);

        const QJsonObject first = commands.at(0).toObject();
        const QStringList keys = first.keys();
        QCOMPARE(QSet<QString>(keys.begin(), keys.end()),
                 (QSet<QString>{QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("category"),
                                QStringLiteral("shortcut"), QStringLiteral("checkable"), QStringLiteral("checked"),
                                QStringLiteral("enabled")}));
        QCOMPARE(first.value(QStringLiteral("id")).toString(), QStringLiteral("command_palette"));
        QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("Command Palette..."));
        QCOMPARE(first.value(QStringLiteral("category")).toString(), QStringLiteral("Help"));
        // Portable text on every platform, not the native Cmd+K of macOS.
        QCOMPARE(first.value(QStringLiteral("shortcut")).toString(), QStringLiteral("Ctrl+K"));
        QCOMPARE(first.value(QStringLiteral("checkable")).toBool(), false);
        QVERIFY(first.value(QStringLiteral("checked")).isNull());
        QCOMPARE(first.value(QStringLiteral("enabled")).toBool(), true);

        const QJsonObject second = commands.at(1).toObject();
        QCOMPARE(second.value(QStringLiteral("id")).toString(), QStringLiteral("draw_bbox"));
        QVERIFY(second.value(QStringLiteral("shortcut")).isNull());
        QCOMPARE(second.value(QStringLiteral("checkable")).toBool(), true);
        QCOMPARE(second.value(QStringLiteral("checked")).toBool(), true);

        const QJsonObject third = commands.at(2).toObject();
        QCOMPARE(third.value(QStringLiteral("id")).toString(), QStringLiteral("zoom_to_fit"));
        QCOMPARE(third.value(QStringLiteral("enabled")).toBool(), false);
    }

    void listFollowsTheLiveActions()
    {
        CommandCatalog catalog;
        QAction sync(QStringLiteral("Sync cursor"));
        sync.setCheckable(true);
        auto* transient = new QAction(QStringLiteral("Transient"));
        catalog.add(QStringLiteral("sync_cursor"), &sync, QStringLiteral("View"));
        catalog.add(QStringLiteral("transient"), transient, QStringLiteral("View"));

        sync.setChecked(true);
        delete transient;
        const QJsonArray commands = listCatalogCommands(catalog).value(QStringLiteral("commands")).toArray();
        QCOMPARE(commands.size(), 1);
        QCOMPARE(commands.at(0).toObject().value(QStringLiteral("checked")).toBool(), true);
    }

    void emptyCatalogListsNothing()
    {
        const CommandCatalog catalog;
        QCOMPARE(listCatalogCommands(catalog),
                 (QJsonObject{{QStringLiteral("commands"), QJsonArray()}}));
    }
};

QTEST_MAIN(AgentBridgeCommandsTest)
#include "test_agent_bridge_commands.moc"
