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

    auto onEdit = [this]{ if(!syncing){ saveEditor(); } };
    connect(title,&QLineEdit::textEdited,this,onEdit);
    connect(type,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{ if(!syncing){ updateTypeFields(); saveEditor(); } });
    for (QLineEdit *e : {kernel,vmlinuz,initrd,chain,icon,background,modules})
        connect(e,&QLineEdit::textEdited,this,onEdit);
    connect(cmdline,&QPlainTextEdit::textChanged,this,onEdit);

    reloadFromModel();
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
