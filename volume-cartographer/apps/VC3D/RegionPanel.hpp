#pragma once

#include "RegionModel.hpp"

#include <QDockWidget>
#include <QString>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QProgressBar;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class RegionController;

// The Regions dock: the 3D Box and Edit tools, the project's regions, the
// selected region's details, its statistics and its crop export.
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
    void applyArc();
    void pickColor();
    // Errors and lasting states only; success messages go to the status bar.
    void showError(const QString& message);
    QString selectedId() const;
    void refreshMeasure();
    void refreshLevels();
    void refreshJobControls();
    void refreshSavedStats();
    void showStats(const vc3d::regions::Json& doc, bool stale);
    void runStats(bool fullResolution);
    void runExport();

    RegionController* _regions = nullptr;
    bool _populating = false;

    QLabel* _statusLabel = nullptr;
    QToolButton* _boxTool = nullptr;
    QToolButton* _editTool = nullptr;
    QToolButton* _arcTool = nullptr;
    QWidget* _arcOptions = nullptr;
    QComboBox* _arcDirection = nullptr;
    QComboBox* _arcSeam = nullptr;
    QLabel* _umbilicusLabel = nullptr;
    QSpinBox* _depth = nullptr;
    QTreeWidget* _list = nullptr;
    QLabel* _loadErrors = nullptr;

    QWidget* _details = nullptr;
    QFormLayout* _detailsForm = nullptr;
    QLineEdit* _name = nullptr;
    QPushButton* _color = nullptr;
    QSpinBox* _bounds[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    QWidget* _boundsRow = nullptr;
    QWidget* _arcRow = nullptr;
    QDoubleSpinBox* _arcStart = nullptr;
    QDoubleSpinBox* _arcSweep = nullptr;
    QDoubleSpinBox* _arcRadiusMin = nullptr;
    QDoubleSpinBox* _arcRadiusMax = nullptr;
    QSpinBox* _arcZMin = nullptr;
    QSpinBox* _arcZMax = nullptr;
    QPushButton* _refreshUmbilicus = nullptr;
    QLabel* _info = nullptr;

    QPushButton* _jump = nullptr;
    QPushButton* _focus = nullptr;
    QPushButton* _delete = nullptr;

    QGroupBox* _measure = nullptr;
    QComboBox* _volume = nullptr;
    QComboBox* _statsLevel = nullptr;
    QCheckBox* _thresholdOn = nullptr;
    QDoubleSpinBox* _threshold = nullptr;
    QPushButton* _runStats = nullptr;
    QPushButton* _fullResolution = nullptr;
    QLabel* _statsSummary = nullptr;
    QLabel* _histogram = nullptr;
    QPushButton* _copyJson = nullptr;
    QLineEdit* _exportDir = nullptr;
    QToolButton* _browseExport = nullptr;
    QComboBox* _exportLevel = nullptr;
    QPushButton* _runExport = nullptr;
    QPushButton* _cancelJob = nullptr;
    QProgressBar* _jobProgress = nullptr;
    QLabel* _jobStatus = nullptr;
    QString _statsJson;
};
