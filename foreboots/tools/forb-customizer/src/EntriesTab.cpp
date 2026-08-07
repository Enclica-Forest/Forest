#include "EntriesTab.h"
#include "ConfigModel.h"
#include "Schema.h"

#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QLabel>
#include <QGroupBox>
#include <QShortcut>
#include <QKeySequence>
#include <functional>
#include <QShortcut>
#include <QKeySequence>

// per-item payload
static const int RoleMap = Qt::UserRole + 1;

static QVariantMap nodeToMap(const EntryNode &n) {
    QVariantMap m;
    m["isSubmenu"] = n.isSubmenu; m["type"] = n.type;
    m["kernel"]=n.kernel; m["vmlinuz"]=n.vmlinuz; m["initrd"]=n.initrd;
    m["chain"]=n.chain; m["cmdline"]=n.cmdline; m["icon"]=n.icon;
    m["background"]=n.background; m["modules"]=n.modules;
    return m;
}
static EntryNode mapToNode(const QString &title, const QVariantMap &m) {
    EntryNode n;
    n.title = title; n.isSubmenu = m.value("isSubmenu").toBool();
    n.type = m.value("type","forest").toString();
    n.kernel=m.value("kernel").toString(); n.vmlinuz=m.value("vmlinuz").toString();
    n.initrd=m.value("initrd").toString(); n.chain=m.value("chain").toString();
    n.cmdline=m.value("cmdline").toString(); n.icon=m.value("icon").toString();
    n.background=m.value("background").toString();
    n.modules=m.value("modules").toStringList();
    return n;
}

EntriesTab::EntriesTab(ConfigModel *m, QWidget *parent) : QWidget(parent), model(m) {
    auto *root = new QHBoxLayout(this);

    // left: tree + toolbar
    auto *left = new QVBoxLayout;
    banner = new QLabel(this); banner->setStyleSheet("color:#b00;"); banner->setWordWrap(true);
    left->addWidget(banner);
    tree = new QTreeWidget(this);
    tree->setHeaderLabels({"Title","Type"});
    tree->setDragDropMode(QAbstractItemView::InternalMove);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setColumnWidth(0, 220);
    left->addWidget(tree, 1);
    auto *tb = new QHBoxLayout;
    auto *bAdd = new QPushButton("Add Entry"), *bSub = new QPushButton("Add Submenu");
    auto *bDup = new QPushButton("Duplicate"), *bDel = new QPushButton("Delete");
    tb->addWidget(bAdd); tb->addWidget(bSub); tb->addWidget(bDup); tb->addWidget(bDel);
    left->addLayout(tb);
    root->addLayout(left, 1);

    // right: editor
    editorBox = new QGroupBox("Selected entry", this);
    auto *form = new QFormLayout(editorBox);
    title = new QLineEdit; form->addRow("Title", title);
    type = new QComboBox; type->addItems(Schema::entryTypes()); form->addRow("Type", type);

    typeStack = new QStackedWidget;
    // page 0: forest (kernel + modules)
    { auto *w = new QWidget; auto *f = new QFormLayout(w);
      kernel = new QLineEdit; f->addRow("kernel", kernel);
      modules = new QLineEdit; modules->setPlaceholderText("comma-separated module paths");
      f->addRow("modules", modules); typeStack->addWidget(w); }
    // page 1: linux (vmlinuz + initrd)
    { auto *w = new QWidget; auto *f = new QFormLayout(w);
      vmlinuz = new QLineEdit; f->addRow("vmlinuz", vmlinuz);
      initrd = new QLineEdit; f->addRow("initrd", initrd); typeStack->addWidget(w); }
    // page 2: chainload (chain)
    { auto *w = new QWidget; auto *f = new QFormLayout(w);
      chain = new QLineEdit; chain->setPlaceholderText("blank = auto-scan all volumes");
      f->addRow("chain", chain); typeStack->addWidget(w); }
    // page 3: empty (shell/recovery/tools/setup/settings/reboot)
    { auto *w = new QWidget; new QVBoxLayout(w); typeStack->addWidget(w); }
    form->addRow(typeStack);

    cmdline = new QPlainTextEdit; cmdline->setMaximumHeight(56); form->addRow("cmdline", cmdline);
    icon = new QLineEdit; icon->setPlaceholderText("name (arch, tux, gear…) or /forebo/icons/x.tga");
    form->addRow("icon", icon);
    background = new QLineEdit; form->addRow("background", background);
    root->addWidget(editorBox, 1);

    connect(bAdd,&QPushButton::clicked,this,[this]{ addEntry(false); });
    connect(bSub,&QPushButton::clicked,this,[this]{ addEntry(true); });
    connect(bDup,&QPushButton::clicked,this,&EntriesTab::duplicateSel);
    connect(bDel,&QPushButton::clicked,this,&EntriesTab::deleteSel);
    connect(tree,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem*it){ loadEditor(it); });
    // drag reorder finishes with a model change signal
    connect(tree->model(),&QAbstractItemModel::rowsMoved,this,[this]{ if(!syncing) rebuildModel(); });

    auto onEdit = [this]{ if(!syncing){ saveEditor(); validateCurrentEntry(); } };
    connect(title,&QLineEdit::textEdited,this,onEdit);
    connect(type,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{ if(!syncing){ updateTypeFields(); saveEditor(); validateCurrentEntry(); } });
    for (QLineEdit *e : {kernel,vmlinuz,initrd,chain,icon,background,modules})
        connect(e,&QLineEdit::textEdited,this,onEdit);
    connect(cmdline,&QPlainTextEdit::textChanged,this,onEdit);

    reloadFromModel();

    // ---- keyboard shortcuts ----
    auto *scDel = new QShortcut(QKeySequence(Qt::Key_Delete), this);
    connect(scDel, &QShortcut::activated, this, &EntriesTab::deleteSel);
    auto *scBackspace = new QShortcut(QKeySequence(Qt::Key_Backspace), this);
    connect(scBackspace, &QShortcut::activated, this, &EntriesTab::deleteSel);
    auto *scAdd = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_N), this);
    connect(scAdd, &QShortcut::activated, this, [this]{ addEntry(false); });
    auto *scUp = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up), this);
    connect(scUp, &QShortcut::activated, this, &EntriesTab::moveUp);
    auto *scDown = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down), this);
    connect(scDown, &QShortcut::activated, this, &EntriesTab::moveDown);
    auto *scRename = new QShortcut(QKeySequence(Qt::Key_F2), this);
    connect(scRename, &QShortcut::activated, this, [this]{ if(tree->currentItem()) title->setFocus(); });
    auto *scEnter = new QShortcut(QKeySequence(Qt::Key_Return), this);
    connect(scEnter, &QShortcut::activated, this, [this]{ if(tree->currentItem()) title->setFocus(); });
    auto *scEscape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(scEscape, &QShortcut::activated, this, [this]{ tree->clearSelection(); });

    // ---- accessibility ----
    tree->setAccessibleName("Menu entries");
    tree->setAccessibleDescription("Boot menu entries tree. Use Delete to remove, Ctrl+Up/Down to reorder.");
    bAdd->setAccessibleName("Add entry");
    bAdd->setToolTip("Add a new menu entry (Ctrl+N)");
    bSub->setAccessibleName("Add submenu");
    bSub->setToolTip("Add a new submenu container");
    bDup->setAccessibleName("Duplicate entry");
    bDup->setToolTip("Duplicate the selected entry");
    bDel->setAccessibleName("Delete entry");
    bDel->setToolTip("Delete the selected entry (Del)");
    editorBox->setAccessibleName("Entry editor");
    title->setAccessibleName("Entry title");
    title->setToolTip("Entry display title (F2 or Enter to edit)");
    type->setAccessibleName("Entry type");
    type->setToolTip("Boot entry type: forest, linux, chainload, etc.");
    kernel->setAccessibleName("Kernel path");
    vmlinuz->setAccessibleName("Vmlinuz path");
    initrd->setAccessibleName("Initrd path");
    chain->setAccessibleName("Chainload path");
    cmdline->setAccessibleName("Kernel command line");
    cmdline->setToolTip("Extra kernel command-line arguments");
    icon->setAccessibleName("Icon name");
    icon->setToolTip("Icon name (arch, tux, gear) or path to a TGA");
    background->setAccessibleName("Background image");
    modules->setAccessibleName("Kernel modules");
    modules->setToolTip("Comma-separated module paths to load");
    setTabOrder(tree, bAdd);
    setTabOrder(bAdd, bSub);
    setTabOrder(bSub, bDup);
    setTabOrder(bDup, bDel);
    setTabOrder(bDel, title);
    setTabOrder(title, type);
}

void EntriesTab::updateTypeFields() {
    QString t = type->currentText();
    if (t=="forest") typeStack->setCurrentIndex(0);
    else if (t=="linux") typeStack->setCurrentIndex(1);
    else if (t=="chainload") typeStack->setCurrentIndex(2);
    else typeStack->setCurrentIndex(3);
}

QTreeWidgetItem *EntriesTab::makeItem(const EntryNode &n) {
    auto *it = new QTreeWidgetItem;
    it->setText(0, n.title);
    it->setText(1, n.isSubmenu ? "submenu" : n.type);
    it->setData(0, RoleMap, nodeToMap(n));
    Qt::ItemFlags f = Qt::ItemIsSelectable|Qt::ItemIsEnabled|Qt::ItemIsDragEnabled;
    if (n.isSubmenu) f |= Qt::ItemIsDropEnabled;
    it->setFlags(f);
    for (const auto &c : n.children) it->addChild(makeItem(c));
    return it;
}

void EntriesTab::reloadFromModel() {
    syncing = true;
    tree->clear();
    for (const auto &n : model->roots) tree->addTopLevelItem(makeItem(n));
    tree->expandAll();
    syncing = false;
    if (tree->topLevelItemCount() > 0) tree->setCurrentItem(tree->topLevelItem(0));
    else loadEditor(nullptr);
    highlightDuplicates();
    int rows = model->flatten().size();
    banner->setText(rows > 64 ? QString("Warning: %1 rows exceeds the firmware cap of 64.").arg(rows) : QString());
}

static EntryNode buildNode(QTreeWidgetItem *it) {
    EntryNode n = mapToNode(it->text(0), it->data(0,RoleMap).toMap());
    n.title = it->text(0);
    if (n.isSubmenu) { n.children.clear(); for (int i=0;i<it->childCount();++i) n.children.append(buildNode(it->child(i))); }
    return n;
}

void EntriesTab::rebuildModel() {
    model->roots.clear();
    for (int i=0;i<tree->topLevelItemCount();++i)
        model->roots.append(buildNode(tree->topLevelItem(i)));
    model->touchStructure();
    highlightDuplicates();
    int rows = model->flatten().size();
    banner->setText(rows > 64 ? QString("Warning: %1 rows exceeds the firmware cap of 64.").arg(rows) : QString());
}

void EntriesTab::addEntry(bool submenu) {
    EntryNode n; n.isSubmenu = submenu; n.title = submenu ? "New Submenu" : "New Entry";
    if (!submenu) n.type = "forest";
    auto *it = makeItem(n);
    QTreeWidgetItem *cur = tree->currentItem();
    if (cur && cur->data(0,RoleMap).toMap().value("isSubmenu").toBool())
        cur->addChild(it);
    else tree->addTopLevelItem(it);
    tree->setCurrentItem(it);
    rebuildModel();
}

void EntriesTab::duplicateSel() {
    QTreeWidgetItem *cur = tree->currentItem();
    if (!cur) return;
    EntryNode n = buildNode(cur);
    n.title += " copy";
    auto *it = makeItem(n);
    if (cur->parent()) cur->parent()->insertChild(cur->parent()->indexOfChild(cur)+1, it);
    else tree->insertTopLevelItem(tree->indexOfTopLevelItem(cur)+1, it);
    tree->setCurrentItem(it);
    rebuildModel();
}

void EntriesTab::deleteSel() {
    QTreeWidgetItem *cur = tree->currentItem();
    if (!cur) return;
    delete cur;
    rebuildModel();
}

void EntriesTab::loadEditor(QTreeWidgetItem *it) {
    editorBox->setEnabled(it != nullptr);
    if (!it) return;
    syncing = true;
    QVariantMap m = it->data(0,RoleMap).toMap();
    EntryNode n = mapToNode(it->text(0), m);
    title->setText(n.title);
    if (n.isSubmenu) { type->setEnabled(false); type->setCurrentText("forest"); }
    else { type->setEnabled(true); type->setCurrentText(n.type); }
    kernel->setText(n.kernel); vmlinuz->setText(n.vmlinuz); initrd->setText(n.initrd);
    chain->setText(n.chain); icon->setText(n.icon); background->setText(n.background);
    modules->setText(n.modules.join(", "));
    cmdline->setPlainText(n.cmdline);
    updateTypeFields();
    syncing = false;
    validateCurrentEntry();
}

void EntriesTab::saveEditor() {
    QTreeWidgetItem *it = tree->currentItem();
    if (!it) return;
    QVariantMap m = it->data(0,RoleMap).toMap();
    bool submenu = m.value("isSubmenu").toBool();
    it->setText(0, title->text());
    if (!submenu) m["type"] = type->currentText();
    m["kernel"]=kernel->text(); m["vmlinuz"]=vmlinuz->text(); m["initrd"]=initrd->text();
    m["chain"]=chain->text(); m["icon"]=icon->text(); m["background"]=background->text();
    QStringList mods; for (const QString &s : modules->text().split(',', Qt::SkipEmptyParts)) mods << s.trimmed();
    m["modules"]=mods;
    m["cmdline"]=cmdline->toPlainText();
    it->setData(0,RoleMap,m);
    it->setText(1, submenu ? "submenu" : type->currentText());
    rebuildModel();
}

void EntriesTab::selectFlatIndex(int i) {
    // walk visible items in flat order
    QVector<QTreeWidgetItem*> flat;
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem*it){
        flat.append(it);
        for (int k=0;k<it->childCount();++k) rec(it->child(k));
    };
    for (int k=0;k<tree->topLevelItemCount();++k) rec(tree->topLevelItem(k));
    if (i>=0 && i<flat.size()) tree->setCurrentItem(flat[i]);
}

void EntriesTab::moveUp() {
    QTreeWidgetItem *cur = tree->currentItem();
    if (!cur) return;
    QTreeWidgetItem *par = cur->parent();
    if (par) {
        int idx = par->indexOfChild(cur);
        if (idx <= 0) return;
        par->removeChild(cur);
        par->insertChild(idx - 1, cur);
    } else {
        int idx = tree->indexOfTopLevelItem(cur);
        if (idx <= 0) return;
        tree->takeTopLevelItem(idx);
        tree->insertTopLevelItem(idx - 1, cur);
    }
    tree->setCurrentItem(cur);
    rebuildModel();
}

void EntriesTab::moveDown() {
    QTreeWidgetItem *cur = tree->currentItem();
    if (!cur) return;
    QTreeWidgetItem *par = cur->parent();
    if (par) {
        int idx = par->indexOfChild(cur);
        if (idx >= par->childCount() - 1) return;
        par->removeChild(cur);
        par->insertChild(idx + 1, cur);
    } else {
        int idx = tree->indexOfTopLevelItem(cur);
        if (idx >= tree->topLevelItemCount() - 1) return;
        tree->takeTopLevelItem(idx);
        tree->insertTopLevelItem(idx + 1, cur);
    }
    tree->setCurrentItem(cur);
    rebuildModel();
}

// ---------------------------------------------------------------------------
//  Inline entry validation
// ---------------------------------------------------------------------------
void EntriesTab::validateCurrentEntry() {
    QTreeWidgetItem *it = tree->currentItem();
    if (!it || syncing) return;

    QVariantMap m = it->data(0, RoleMap).toMap();
    EntryNode n = mapToNode(it->text(0), m);

    // Build an EntryNode from current editor state (unsaved title)
    n.title = title->text();
    if (!m.value("isSubmenu").toBool()) n.type = type->currentText();
    n.kernel = kernel->text(); n.vmlinuz = vmlinuz->text();
    n.initrd = initrd->text(); n.chain = chain->text();
    n.icon = icon->text(); n.background = background->text();
    QStringList mods; for (const QString &s : modules->text().split(',', Qt::SkipEmptyParts)) mods << s.trimmed();
    n.modules = mods;
    n.cmdline = cmdline->toPlainText();

    QVector<EntryValidation> issues = ConfigModel::validateEntry(n);

    // 1. Title field: red border if > 63 chars
    bool titleLong = n.title.size() > 63;
    title->setStyleSheet(titleLong ? "border: 2px solid red;" : QString());

    // 2. Kernel field: warning background if empty for forest/linux
    bool kernelMissing = (n.type == "forest" && n.kernel.isEmpty()) ||
                         (n.type == "linux" && n.vmlinuz.isEmpty());
    kernel->setStyleSheet((n.type == "forest" && kernelMissing) ? "background: #fff0f0;" : QString());
    vmlinuz->setStyleSheet((n.type == "linux" && kernelMissing) ? "background: #fff0f0;" : QString());

    // 3. Cmdline: warning background if > 255 chars
    bool cmdlineLong = n.cmdline.size() > 255;
    cmdline->setStyleSheet(cmdlineLong ? "background: #fff0f0;" : QString());

    // 4. Build status text for banner
    int errors = 0, warnings = 0, infos = 0;
    for (const auto &v : issues) {
        if (v.level == EntryValidation::Error) ++errors;
        else if (v.level == EntryValidation::Warning) ++warnings;
        else ++infos;
    }

    // Also run full model validation for duplicate check
    QVector<EntryValidation> allIssues = model->validateAll();
    int dupCount = 0;
    for (const auto &v : allIssues)
        if (v.message.contains("duplicate")) ++dupCount;

    QStringList parts;
    if (errors)   parts << QString("%1 error(s)").arg(errors);
    if (warnings) parts << QString("%1 warning(s)").arg(warnings);
    if (dupCount) parts << QString("%1 duplicate(s)").arg(dupCount);
    if (infos)    parts << QString("%1 info(s)").arg(infos);

    banner->setText(parts.isEmpty() ? QString() : "Validation: " + parts.join(", "));

    // Highlight duplicates in tree
    highlightDuplicates();
}

void EntriesTab::highlightDuplicates() {
    // Collect all entries with their (title, kernel) keys
    struct DupKey { QString title, kernel;
        bool operator<(const DupKey &o) const { return title < o.title || (title == o.title && kernel < o.kernel); }
    };
    auto getKey = [](QTreeWidgetItem *it) -> DupKey {
        QVariantMap m = it->data(0, RoleMap).toMap();
        bool sub = m.value("isSubmenu").toBool();
        QString k = sub ? QString() : (m.value("type").toString() == "linux"
                      ? m.value("vmlinuz").toString()
                      : m.value("kernel").toString());
        return {it->text(0), k};
    };

    // Count occurrences
    QMap<DupKey, int> counts;
    std::function<void(QTreeWidgetItem*)> countAll = [&](QTreeWidgetItem *it) {
        DupKey key = getKey(it);
        if (!it->data(0, RoleMap).toMap().value("isSubmenu").toBool())
            counts[key]++;
        for (int i = 0; i < it->childCount(); ++i) countAll(it->child(i));
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        countAll(tree->topLevelItem(i));

    // Apply yellow background to duplicates
    std::function<void(QTreeWidgetItem*)> markDups = [&](QTreeWidgetItem *it) {
        DupKey key = getKey(it);
        bool isDup = !it->data(0, RoleMap).toMap().value("isSubmenu").toBool() &&
                     counts.value(key, 0) > 1;
        it->setBackground(0, isDup ? QBrush(QColor(255, 255, 180)) : QBrush());
        for (int i = 0; i < it->childCount(); ++i) markDups(it->child(i));
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        markDups(tree->topLevelItem(i));
}
