// DiffDialog.h - side-by-side config comparison view.
// Shows the current config vs a reference file with entry-level and
// theme-setting-level diffing. Users can selectively apply changes.
#ifndef FORB_DIFFDIALOG_H
#define FORB_DIFFDIALOG_H

#include <QDialog>
#include <QVector>
class ConfigModel;
class QTreeWidget;
class QTreeWidgetItem;
class QCheckBox;

class DiffDialog : public QDialog {
    Q_OBJECT
public:
    DiffDialog(ConfigModel *current, const QString &refPath, QWidget *parent = nullptr);

private slots:
    void applySelected();
    void applyAll();
    void toggleCheckState(QTreeWidgetItem *item, int col);

private:
    struct DiffItem {
        enum Kind { Added, Removed, Modified };
        Kind kind;
        QString label;       // entry title or setting name
        QString category;    // "Entry" / "Global" / "Theme"
        QString detail;      // field-level info for Modified
        QString leftValue;   // current value text
        QString rightValue;  // reference value text
        bool checked = true; // apply this change?
    };

    void computeDiff();
    void computeEntryDiff();
    void computeGlobalDiff();
    void computeThemeDiff();
    void populateTree();
    void highlightItem(QTreeWidgetItem *item, DiffItem::Kind kind);
    QColor colorForKind(DiffItem::Kind kind) const;

    ConfigModel *m_current;
    ConfigModel *m_reference;   // owned, loaded from refPath
    QVector<DiffItem> m_diffs;
    QTreeWidget *m_tree;
};

#endif // FORB_DIFFDIALOG_H
