// EntriesTab.h - the menuentry/submenu tree editor.
//  - QTreeWidget with InternalMove drag to reorder AND re-parent (submenus).
//  - a per-entry editor (title, type, type-conditional fields, cmdline, icon).
// Any structural or field change rebuilds ConfigModel::roots and repaints.
#ifndef FORB_ENTRIESTAB_H
#define FORB_ENTRIESTAB_H

#include <QWidget>
class ConfigModel;
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QComboBox;
class QPlainTextEdit;
class QStackedWidget;
class QLabel;

class EntriesTab : public QWidget {
    Q_OBJECT
public:
    EntriesTab(ConfigModel *m, QWidget *parent = nullptr);
    void reloadFromModel();               // after file load / import / preset
    void selectFlatIndex(int i);          // preview clicked an entry row

private:
    void addEntry(bool submenu);
    void duplicateSel();
    void deleteSel();
    void rebuildModel();                  // tree -> model
    void loadEditor(QTreeWidgetItem *it);
    void saveEditor();                    // editor -> current item
    QTreeWidgetItem *makeItem(const struct EntryNode &n);
    void updateTypeFields();

    ConfigModel *model; bool syncing = false;
    QTreeWidget *tree;
    QLineEdit *title; QComboBox *type;
    QLineEdit *kernel, *vmlinuz, *initrd, *chain, *icon, *background, *modules;
    QPlainTextEdit *cmdline;
    QStackedWidget *typeStack;
    QLabel *banner;
    QWidget *editorBox;
};

#endif // FORB_ENTRIESTAB_H
