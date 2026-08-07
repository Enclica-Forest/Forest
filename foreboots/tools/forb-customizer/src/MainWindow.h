// MainWindow.h - the shell: menubar/toolbar, the QTabWidget of editor tabs, a
// persistent live PreviewWidget on the right, an Inspector dock, and file ops
// (New / Open / Import Limine / Save / Save As / Export). Owns the single
// ConfigModel and hands a pointer to every tab, the preview and the inspector.
#ifndef FORB_MAINWINDOW_H
#define FORB_MAINWINDOW_H

#include <QMainWindow>
#include <QVector>
#include <QPair>
class QTabWidget;
class ConfigModel;
class PreviewWidget;
class EntriesTab;
class PresetGallery;
class Inspector;
class QComboBox;
class QFileSystemWatcher;
class QFrame;
template<typename T> struct Opt;   // defined in ConfigModel.h (pointer use only)

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void newFile();
    void openFile();
    void importLimine();
    void importSyslinux();
    void importRefind();
    void saveFile();
    void saveFileAs();
    void applyToBoot();
    void importFromBoot();
    void lintConfig();
    void saveAsTemplate();
    void loadTemplate();
    void loadTemplateByName(const QString &name);
    void deleteTemplate();
    void onFileChanged(const QString &path);
    void compareWith();      // open diff dialog to compare current config vs another file

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
    void buildTemplateMenu();                          // rebuild the Templates submenu
    void loadTemplateFromPath(const QString &path);    // load a template by file path
    QString templateDir() const;                       // ~/.config/forb-customizer/templates/
    QMenu *templateMenu = nullptr;                     // owned by menuBar
    // Convert every set image field to a ForeB TGA staged in `dir`. Returns
    // (destName, stagedHostPath) per file and queues the /forebo/<name>.tga
    // field rewrites into `pending` - the caller applies them ONLY after the
    // save/install succeeds, so a cancelled apply keeps the original paths.
    QVector<QPair<QString,QString>> stageImagesTGA(const QString &dir,
                                                   QVector<QPair<Opt<QString>*,QString>> *pending);
    bool installElevated(const QVector<QPair<QString,QString>> &destSrc, QString *err); // pkexec cp
    QString detectEspMount();                          // ESP mount from /proc/mounts ("" if none)

    void updateWindowTitle();
    void startWatching();
    void stopWatching();
    void reloadConfig();
    bool promptSaveIfDirty();                          // returns false if user cancels

    ConfigModel   *model;
    PreviewWidget *preview;
    EntriesTab    *entries;
    PresetGallery *gallery;
    Inspector     *inspector;
    QTabWidget    *tabs;
    QComboBox     *defaultCombo = nullptr;
    QString        currentPath;
    bool           defaultSyncing = false;

    QFileSystemWatcher *fileWatcher = nullptr;
    QFrame             *notificationBar = nullptr;
    bool               isDirty = false;
};

#endif // FORB_MAINWINDOW_H
