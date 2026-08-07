// MainWindow.h - the shell: menubar/toolbar, the QTabWidget of editor tabs, a
// persistent live PreviewWidget on the right, an Inspector dock, and file ops
// (New / Open / Import Limine / Save / Save As / Export). Owns the single
// ConfigModel and hands a pointer to every tab, the preview and the inspector.
#ifndef FORB_MAINWINDOW_H
#define FORB_MAINWINDOW_H

#include <QMainWindow>
#include <QVector>
#include <QPair>
class ConfigModel;
class PreviewWidget;
class EntriesTab;
class PresetGallery;
class Inspector;
class QComboBox;
template<typename T> struct Opt;   // defined in ConfigModel.h (pointer use only)

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr);

private slots:
    void newFile();
    void openFile();
    void importLimine();
    void saveFile();
    void saveFileAs();
    void applyToBoot();      // convert images -> TGA + write forebo.cfg to the ESP (pkexec)
    void importFromBoot();   // detect + load forebo.cfg or migrate limine.conf from /boot (pkexec)

private:
    QWidget *buildGeneralTab();
    QWidget *buildThemeTab();
    QWidget *buildMenuTab();
    QWidget *buildButtonsTab();
    QWidget *buildEffectsTab();
    QWidget *buildAudioTab();
    QWidget *buildWindowSkinTab();
    QWidget *buildImagesTab();
    void buildDefaultCombo(class QFormLayout *form);
    void autoLoad();                                   // find + load /boot config
    void doSave(const QString &path);                  // convert images + write (pkexec if needed)
    // Convert every set image field to a ForeB TGA staged in `dir`. Returns
    // (destName, stagedHostPath) per file and queues the /forebo/<name>.tga
    // field rewrites into `pending` - the caller applies them ONLY after the
    // save/install succeeds, so a cancelled apply keeps the original paths.
    QVector<QPair<QString,QString>> stageImagesTGA(const QString &dir,
                                                   QVector<QPair<Opt<QString>*,QString>> *pending);
    bool installElevated(const QVector<QPair<QString,QString>> &destSrc, QString *err); // pkexec cp
    QString detectEspMount();                          // ESP mount from /proc/mounts ("" if none)

    ConfigModel   *model;
    PreviewWidget *preview;
    EntriesTab    *entries;
    PresetGallery *gallery;
    Inspector     *inspector;
    QComboBox     *defaultCombo = nullptr;
    QString        currentPath;
    bool           defaultSyncing = false;
};

#endif // FORB_MAINWINDOW_H
