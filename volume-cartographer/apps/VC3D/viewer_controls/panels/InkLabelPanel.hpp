#pragma once

#include <QPointer>
#include <QWidget>

class CollapsibleSettingsGroup;
class InkLabelController;
class QCheckBox;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;

// The ink label section of the Ink Detection dock: label mode, class, tool,
// display and file controls for the InkLabelController. Every control is
// Qt::NoFocus, so a click leaves keyboard focus in the view.
class InkLabelPanel : public QWidget
{
    Q_OBJECT

public:
    explicit InkLabelPanel(InkLabelController* labels, QWidget* parent = nullptr);

private:
    void refreshSession();
    void refreshTools();
    void refreshDisplay();
    void refreshHistory();
    void refreshCounts();
    void pushDisplay();

    QPointer<InkLabelController> _labels;
    CollapsibleSettingsGroup* _labelsGroup{nullptr};
    QPushButton* _modeButton{nullptr};
    QLabel* _folderLabel{nullptr};
    QLabel* _statusLabel{nullptr};
    QLabel* _bannerLabel{nullptr};
    QPushButton* _inkButton{nullptr};
    QPushButton* _backgroundButton{nullptr};
    QPushButton* _validationButton{nullptr};
    QPushButton* _eraserButton{nullptr};
    QCheckBox* _keepsInkCheck{nullptr};
    QPushButton* _brushButton{nullptr};
    QPushButton* _polygonButton{nullptr};
    QSlider* _radiusSlider{nullptr};
    QLabel* _radiusLabel{nullptr};
    QCheckBox* _showCheck{nullptr};
    QCheckBox* _showInkCheck{nullptr};
    QCheckBox* _showSupervisionCheck{nullptr};
    QCheckBox* _showValidationCheck{nullptr};
    QSlider* _opacitySlider{nullptr};
    QPushButton* _undoButton{nullptr};
    QPushButton* _redoButton{nullptr};
    QPushButton* _saveButton{nullptr};
    QLabel* _countsLabel{nullptr};
    QTimer* _countsTimer{nullptr};
};
