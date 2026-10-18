#include "viewer_controls/panels/InkLabelPanel.hpp"

#include "elements/CollapsibleSettingsGroup.hpp"
#include "elements/LabeledControlRow.hpp"
#include "ink_labels/InkLabelController.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QFontMetrics>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <initializer_list>

using namespace vc3d::ink_labels;

namespace {

// The radius slider is logarithmic: 0 is kMinRadius, kRadiusSteps is kMaxRadius.
constexpr int kRadiusSteps = 1000;

int radiusToSlider(double r)
{
    const double t = std::log(r / kMinRadius) / std::log(kMaxRadius / kMinRadius);
    return int(std::lround(std::clamp(t, 0.0, 1.0) * kRadiusSteps));
}

double sliderToRadius(int value)
{
    return kMinRadius * std::pow(kMaxRadius / kMinRadius, double(value) / kRadiusSteps);
}

// A small square of a label color for the class buttons.
QIcon swatch(const QColor& color)
{
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setPen(color.darker(160));
    painter.setBrush(color);
    painter.drawRect(0, 0, 11, 11);
    return QIcon(pixmap);
}

QPushButton* makeButton(const QString& text, QWidget* parent, bool checkable)
{
    auto* button = new QPushButton(text, parent);
    button->setCheckable(checkable);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QCheckBox* makeCheck(const QString& text, QWidget* parent)
{
    auto* check = new QCheckBox(text, parent);
    check->setFocusPolicy(Qt::NoFocus);
    return check;
}

}  // namespace

InkLabelPanel::InkLabelPanel(InkLabelController* labels, QWidget* parent)
    : QWidget(parent)
    , _labels(labels)
{
    setObjectName(QStringLiteral("inkLabelPanel"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 8, 0, 0);
    outer->setSpacing(6);
    _labelsGroup = new CollapsibleSettingsGroup(tr("Ink labels"), this);
    _labelsGroup->setObjectName(QStringLiteral("inkLabelGroup"));
    _labelsGroup->setToolTip(tr("Draw ink, background and validation labels on the segment view and save them as the ink "
                                "detection trainer's label TIFFs."));
    outer->addWidget(_labelsGroup);
    auto* layout = _labelsGroup->contentLayout();
    layout->setSpacing(6);

    auto* modeRow = new LabeledControlRow(tr("Labels"), this);
    _modeButton = makeButton(tr("Label mode (I)"), modeRow, true);
    _modeButton->setObjectName(QStringLiteral("inkLabelModeButton"));
    _modeButton->setToolTip(tr("Paint labels on the segment view with the left mouse button. Right drag still pans."));
    modeRow->addControl(_modeButton);
    modeRow->addStretch(1);
    layout->addWidget(modeRow);

    _folderLabel = new QLabel(this);
    _folderLabel->setObjectName(QStringLiteral("inkLabelFolder"));
    _folderLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    _folderLabel->setMinimumWidth(40);
    layout->addWidget(_folderLabel);

    _statusLabel = new QLabel(this);
    _statusLabel->setObjectName(QStringLiteral("inkLabelStatus"));
    layout->addWidget(_statusLabel);

    _bannerLabel = new QLabel(this);
    _bannerLabel->setObjectName(QStringLiteral("inkLabelBanner"));
    _bannerLabel->setWordWrap(true);
    // Palette colors, so the banner reads in light and dark themes.
    _bannerLabel->setFrameShape(QFrame::StyledPanel);
    _bannerLabel->setMargin(4);
    _bannerLabel->setAutoFillBackground(true);
    _bannerLabel->setBackgroundRole(QPalette::ToolTipBase);
    _bannerLabel->setForegroundRole(QPalette::ToolTipText);
    _bannerLabel->hide();
    layout->addWidget(_bannerLabel);

    auto* classRow = new LabeledControlRow(tr("Class"), this);
    _inkButton = makeButton(tr("Ink (W)"), classRow, true);
    _inkButton->setToolTip(tr("Ink, drawn red: the trainer learns these pixels as ink."));
    _inkButton->setIcon(swatch(InkLabelController::colorFor(PaintClass::Ink, false)));
    _backgroundButton = makeButton(tr("Background (F)"), classRow, true);
    _backgroundButton->setToolTip(tr("Background, drawn blue: supervised pixels that are not ink."));
    _backgroundButton->setIcon(swatch(InkLabelController::colorFor(PaintClass::Background, false)));
    _validationButton = makeButton(tr("Validation (V)"), classRow, true);
    _validationButton->setToolTip(tr("Validation area, drawn yellow: written to the validation mask."));
    _validationButton->setIcon(swatch(InkLabelController::colorFor(PaintClass::Validation, false)));
    auto* classGroup = new QButtonGroup(this);
    classGroup->setExclusive(true);
    classGroup->addButton(_inkButton, int(PaintClass::Ink));
    classGroup->addButton(_backgroundButton, int(PaintClass::Background));
    classGroup->addButton(_validationButton, int(PaintClass::Validation));
    classRow->addControl(_inkButton);
    classRow->addControl(_backgroundButton);
    classRow->addControl(_validationButton);
    classRow->addStretch(1);
    layout->addWidget(classRow);

    auto* eraseRow = new LabeledControlRow(QString(), this);
    _eraserButton = makeButton(tr("Eraser (Z)"), eraseRow, true);
    _eraserButton->setToolTip(tr("Ink and background go back to unknown (not supervised); validation is cleared."));
    _eraserButton->setIcon(swatch(InkLabelController::colorFor(PaintClass::Ink, true)));
    _keepsInkCheck = makeCheck(tr("Background keeps ink"), eraseRow);
    _keepsInkCheck->setToolTip(tr("Painting background over ink marks it supervised without clearing the ink."));
    eraseRow->addControl(_eraserButton);
    eraseRow->addControl(_keepsInkCheck);
    eraseRow->addStretch(1);
    layout->addWidget(eraseRow);

    auto* toolRow = new LabeledControlRow(tr("Tool"), this);
    _brushButton = makeButton(tr("Brush"), toolRow, true);
    _brushButton->setToolTip(tr("Paint with a round brush; Shift+Click draws a straight stroke. L switches tools."));
    _polygonButton = makeButton(tr("Polygon"), toolRow, true);
    _polygonButton->setToolTip(tr("Click points and press Enter, or drag a lasso. Backspace removes a point, Esc cancels. L "
                                  "switches tools."));
    auto* toolGroup = new QButtonGroup(this);
    toolGroup->setExclusive(true);
    toolGroup->addButton(_brushButton, int(InkLabelController::Tool::Brush));
    toolGroup->addButton(_polygonButton, int(InkLabelController::Tool::Polygon));
    toolRow->addControl(_brushButton);
    toolRow->addControl(_polygonButton);
    toolRow->addStretch(1);
    layout->addWidget(toolRow);

    auto* radiusRow = new LabeledControlRow(tr("Radius"), this);
    _radiusSlider = new QSlider(Qt::Horizontal, radiusRow);
    _radiusSlider->setObjectName(QStringLiteral("inkLabelRadius"));
    _radiusSlider->setRange(0, kRadiusSteps);
    _radiusSlider->setFocusPolicy(Qt::NoFocus);
    _radiusSlider->setToolTip(tr("Brush radius in label pixels (voxels at full resolution). Ctrl+Mouse Wheel in the view."));
    _radiusLabel = new QLabel(radiusRow);
    _radiusLabel->setToolTip(_radiusSlider->toolTip());
    _radiusLabel->setMinimumWidth(QFontMetrics(_radiusLabel->font()).horizontalAdvance(QStringLiteral("512 px")));
    radiusRow->addControl(_radiusSlider, 1);
    radiusRow->addControl(_radiusLabel);
    layout->addWidget(radiusRow);

    auto* showRow = new LabeledControlRow(tr("Show"), this);
    _showCheck = makeCheck(tr("Labels (H)"), showRow);
    _showInkCheck = makeCheck(tr("Ink"), showRow);
    _showSupervisionCheck = makeCheck(tr("Supervision"), showRow);
    _showValidationCheck = makeCheck(tr("Validation"), showRow);
    showRow->addControl(_showCheck);
    showRow->addControl(_showInkCheck);
    showRow->addControl(_showSupervisionCheck);
    showRow->addControl(_showValidationCheck);
    showRow->addStretch(1);
    layout->addWidget(showRow);

    auto* opacityRow = new LabeledControlRow(tr("Opacity"), this);
    _opacitySlider = new QSlider(Qt::Horizontal, opacityRow);
    _opacitySlider->setRange(0, 100);
    _opacitySlider->setFocusPolicy(Qt::NoFocus);
    opacityRow->addControl(_opacitySlider, 1);
    layout->addWidget(opacityRow);

    auto* historyRow = new LabeledControlRow(tr("History"), this);
    _undoButton = makeButton(tr("Undo"), historyRow, false);
    _redoButton = makeButton(tr("Redo"), historyRow, false);
    historyRow->addControl(_undoButton);
    historyRow->addControl(_redoButton);
    historyRow->addStretch(1);
    layout->addWidget(historyRow);

    auto* fileRow = new LabeledControlRow(tr("File"), this);
    _saveButton = makeButton(tr("Save (Ctrl+S)"), fileRow, false);
    _saveButton->setObjectName(QStringLiteral("inkLabelSave"));
    _saveButton->setToolTip(tr("Write <segment>_inklabels.tif, <segment>_supervision_mask.tif and, if used, "
                               "<segment>_validation_mask.tif."));
    fileRow->addControl(_saveButton);
    fileRow->addStretch(1);
    layout->addWidget(fileRow);

    _countsLabel = new QLabel(this);
    _countsLabel->setObjectName(QStringLiteral("inkLabelCounts"));
    _countsLabel->setWordWrap(true);
    layout->addWidget(_countsLabel);

    if (!_labels) {
        setEnabled(false);
        return;
    }

    _countsTimer = new QTimer(this);
    _countsTimer->setSingleShot(true);
    _countsTimer->setInterval(150);
    connect(_countsTimer, &QTimer::timeout, this, &InkLabelPanel::refreshCounts);

    connect(_modeButton, &QPushButton::clicked, this, [this](bool checked) {
        _labels->setLabelModeEnabled(checked);
        refreshSession();
    });
    connect(classGroup, &QButtonGroup::idClicked, this, [this](int id) {
        _labels->setErase(false);
        _labels->setPaintClass(PaintClass(id));
    });
    connect(_eraserButton, &QPushButton::clicked, this, [this](bool checked) { _labels->setErase(checked); });
    connect(_keepsInkCheck, &QCheckBox::toggled, this, [this](bool checked) { _labels->setBackgroundKeepsInk(checked); });
    connect(toolGroup, &QButtonGroup::idClicked, this, [this](int id) { _labels->setTool(InkLabelController::Tool(id)); });
    connect(_radiusSlider, &QSlider::valueChanged, this, [this](int value) { _labels->setRadius(sliderToRadius(value)); });
    for (QCheckBox* check : {_showCheck, _showInkCheck, _showSupervisionCheck, _showValidationCheck})
        connect(check, &QCheckBox::toggled, this, &InkLabelPanel::pushDisplay);
    connect(_opacitySlider, &QSlider::valueChanged, this, &InkLabelPanel::pushDisplay);
    connect(_undoButton, &QPushButton::clicked, this, [this] { _labels->undo(Actor::User, true); });
    connect(_redoButton, &QPushButton::clicked, this, [this] { _labels->redo(Actor::User); });
    connect(_saveButton, &QPushButton::clicked, this, [this] { _labels->saveInteractive(); });

    connect(_labels, &InkLabelController::sessionChanged, this, &InkLabelPanel::refreshSession);
    connect(_labels, &InkLabelController::labelModeChanged, this, &InkLabelPanel::refreshSession);
    connect(_labels, &InkLabelController::dirtyChanged, this, &InkLabelPanel::refreshSession);
    connect(_labels, &InkLabelController::saveFinished, this, &InkLabelPanel::refreshSession);
    connect(_labels, &InkLabelController::toolStateChanged, this, &InkLabelPanel::refreshTools);
    connect(_labels, &InkLabelController::displayChanged, this, &InkLabelPanel::refreshDisplay);
    connect(_labels, &InkLabelController::historyChanged, this, &InkLabelPanel::refreshHistory);
    connect(_labels, &InkLabelController::rasterChanged, this, [this](QRect) { _countsTimer->start(); });

    refreshSession();
    refreshTools();
    refreshDisplay();
    refreshHistory();
}

void InkLabelPanel::refreshSession()
{
    if (!_labels)
        return;
    const InkLabelController::SessionInfo* session = _labels->session();
    {
        const QSignalBlocker blocker(_modeButton);
        _modeButton->setChecked(_labels->labelModeEnabled());
    }
    if (!session) {
        _folderLabel->setText(tr("No labels open. Select a segment and press I."));
        _folderLabel->setToolTip(QString());
        _statusLabel->clear();
        _bannerLabel->hide();
    } else {
        const QString dir = QString::fromStdString(session->paths.dir.string());
        const QFontMetrics metrics(_folderLabel->font());
        _folderLabel->setText(metrics.elidedText(dir, Qt::ElideMiddle, std::max(120, _folderLabel->width())));
        _folderLabel->setToolTip(dir);

        QStringList parts;
        if (session->locked)
            parts << tr("locked");
        else if (session->readOnly)
            parts << tr("read-only");
        else
            parts << (session->dirty ? tr("unsaved changes") : tr("no unsaved changes"));
        if (const auto& last = _labels->lastSave(); last && last->atUtc.isValid())
            parts << tr("saved %1").arg(last->atUtc.toLocalTime().toString(QStringLiteral("hh:mm")));
        if (_labels->lastAutosave().isValid())
            parts << tr("autosaved %1").arg(_labels->lastAutosave().toLocalTime().toString(QStringLiteral("hh:mm")));
        if (_labels->saveInFlight())
            parts << tr("saving...");
        _statusLabel->setText(parts.join(QStringLiteral(", ")));

        _bannerLabel->setVisible(!session->lockReason.isEmpty());
        _bannerLabel->setText(session->lockReason);
    }

    const bool editable = session && !session->locked && !session->readOnly;
    for (QWidget* w : std::initializer_list<QWidget*>{_inkButton,
                                                      _backgroundButton,
                                                      _validationButton,
                                                      _eraserButton,
                                                      _keepsInkCheck,
                                                      _brushButton,
                                                      _polygonButton,
                                                      _radiusSlider})
        w->setEnabled(editable || !session);
    _saveButton->setEnabled(editable);
    refreshHistory();
    refreshCounts();
}

void InkLabelPanel::refreshTools()
{
    if (!_labels)
        return;
    const QSignalBlocker b1(_inkButton), b2(_backgroundButton), b3(_validationButton), b4(_eraserButton), b5(_keepsInkCheck),
        b6(_brushButton), b7(_polygonButton), b8(_radiusSlider);
    _inkButton->setChecked(_labels->paintClass() == PaintClass::Ink);
    _backgroundButton->setChecked(_labels->paintClass() == PaintClass::Background);
    _validationButton->setChecked(_labels->paintClass() == PaintClass::Validation);
    _eraserButton->setChecked(_labels->erase());
    _keepsInkCheck->setChecked(_labels->backgroundKeepsInk());
    _brushButton->setChecked(_labels->tool() == InkLabelController::Tool::Brush);
    _polygonButton->setChecked(_labels->tool() == InkLabelController::Tool::Polygon);
    _radiusSlider->setValue(radiusToSlider(_labels->radius()));
    _radiusLabel->setText(tr("%1 px").arg(QString::number(_labels->radius(), 'g', 3)));
}

void InkLabelPanel::refreshDisplay()
{
    if (!_labels)
        return;
    const QSignalBlocker b1(_showCheck), b2(_showInkCheck), b3(_showSupervisionCheck), b4(_showValidationCheck),
        b5(_opacitySlider);
    _showCheck->setChecked(_labels->showLabels());
    _showInkCheck->setChecked(_labels->showInk());
    _showSupervisionCheck->setChecked(_labels->showSupervision());
    _showValidationCheck->setChecked(_labels->showValidation());
    _opacitySlider->setValue(int(std::lround(_labels->opacity() * 100.0)));
}

void InkLabelPanel::refreshHistory()
{
    if (!_labels)
        return;
    const History* history = _labels->history();
    const InkLabelController::SessionInfo* session = _labels->session();
    const bool editable = session && !session->locked && !session->readOnly;
    const std::size_t undoDepth = history ? history->undoDepth() : 0;
    const std::size_t redoDepth = history ? history->redoDepth() : 0;
    _undoButton->setText(tr("Undo (%1)").arg(undoDepth));
    _redoButton->setText(tr("Redo (%1)").arg(redoDepth));
    _undoButton->setEnabled(editable && undoDepth > 0);
    _redoButton->setEnabled(editable && redoDepth > 0);
}

void InkLabelPanel::refreshCounts()
{
    if (!_labels)
        return;
    const InkLabelController::SessionInfo* session = _labels->session();
    if (!session || session->locked) {
        _countsLabel->clear();
        return;
    }
    const LabelCounts c = LabelCounts::from(session->counts);
    const QLocale locale;
    _countsLabel->setText(tr("ink %1 px, background %2 px, validation %3 px")
                              .arg(locale.toString(qulonglong(c.ink)),
                                   locale.toString(qulonglong(c.background)),
                                   locale.toString(qulonglong(c.validation))));
}

void InkLabelPanel::pushDisplay()
{
    if (!_labels)
        return;
    _labels->setDisplay(_showCheck->isChecked(),
                        _showInkCheck->isChecked(),
                        _showSupervisionCheck->isChecked(),
                        _showValidationCheck->isChecked(),
                        _opacitySlider->value() / 100.0);
}
