#pragma once

#include "RegionModel.hpp"

#include <QDockWidget>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class RegionController;

// The Regions dock: the 3D Box and Edit tools, the project's regions and the
// selected region's details.
class RegionPanel : public QDockWidget
{
    Q_OBJECT

public:
    explicit RegionPanel(RegionController* regions, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void refreshAll();
    void refreshTools();
    void refreshList();
    void refreshDetails();
    void selectActiveInList();
    void applyName();
    void applyBounds();
    void pickColor();
    // Errors and lasting states only; success messages go to the status bar.
    void showError(const QString& message);
    QString selectedId() const;

    RegionController* _regions = nullptr;
    bool _populating = false;

    QLabel* _statusLabel = nullptr;
    QToolButton* _boxTool = nullptr;
    QToolButton* _editTool = nullptr;
    QSpinBox* _depth = nullptr;
    QTreeWidget* _list = nullptr;
    QLabel* _loadErrors = nullptr;

    QWidget* _details = nullptr;
    QLineEdit* _name = nullptr;
    QPushButton* _color = nullptr;
    QSpinBox* _bounds[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    QWidget* _boundsRow = nullptr;
    QLabel* _info = nullptr;

    QPushButton* _jump = nullptr;
    QPushButton* _focus = nullptr;
    QPushButton* _delete = nullptr;
};
