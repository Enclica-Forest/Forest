// tst_roundtrip.cpp - verify lossless parse -> emit -> reparse of the shipped
// forebo.cfg, plus a Limine-import smoke test. Uses QtTest (headless).
#include <QtTest>
#include <QFileInfo>
#include "ConfigModel.h"

class TestRoundtrip : public QObject {
    Q_OBJECT
private slots:
    void roundtripSample();
    void limineTree();
    void optOmitsUnset();
};

// Locate the repo's sample forebo.cfg relative to the source tree.
static QString sampleCfg() {
    // tests/ -> tools/forb-customizer/ -> tools/ -> repo root
    QString here = QFileInfo(__FILE__).absolutePath();
    return QFileInfo(here + "/../../../forebo.cfg").absoluteFilePath();
}
static QString limineFixture() {
    QString here = QFileInfo(__FILE__).absolutePath();
    return QFileInfo(here + "/../../tests/limine.conf").absoluteFilePath();
}

void TestRoundtrip::roundtripSample() {
    QString path = sampleCfg();
    QVERIFY2(QFileInfo::exists(path), qPrintable("missing " + path));
    ConfigModel a;
    QVERIFY(a.loadFile(path));
    QString emitted = a.serialize();

    ConfigModel b;
    b.parseText(emitted);
    QString emitted2 = b.serialize();

    // model equality: globals, theme, entry tree all preserved
    QCOMPARE(a.g, b.g);
    QVERIFY(a.th == b.th);
    QCOMPARE(a.roots.size(), b.roots.size());
    QVERIFY(a.roots == b.roots);
    // and the second emit is byte-stable
    QCOMPARE(emitted, emitted2);
}

void TestRoundtrip::limineTree() {
    QString path = limineFixture();
    QVERIFY2(QFileInfo::exists(path), qPrintable("missing " + path));
    ConfigModel m;
    QVERIFY(m.importLimine(path));
    // top level: CachyOS submenu, "Other systems and bootloaders" submenu,
    // and the EFI fallback entry.
    QVERIFY(m.roots.size() >= 2);
    // the CachyOS group should be a submenu with linux children
    const EntryNode &g = m.roots.first();
    QVERIFY(g.isSubmenu);
    QVERIFY(g.children.size() >= 3);
    QCOMPARE(g.children.first().type, QString("linux"));
    QVERIFY(!g.children.first().vmlinuz.isEmpty());
    QVERIFY(m.g.timeout.isSet());
    QCOMPARE(m.g.timeout.v, 5);
    QVERIFY(m.g.rememberLast.isSet() && m.g.rememberLast.v);
}

void TestRoundtrip::optOmitsUnset() {
    ConfigModel m;
    m.resetDefaults();
    // nothing set -> serialize must not contain a color_bg= line
    QString s = m.serialize();
    QVERIFY(!s.contains("color_bg="));
    m.th.colorBg.assign(0x123456);
    QVERIFY(m.serialize().contains("color_bg=0x123456"));
}

QTEST_MAIN(TestRoundtrip)
#include "tst_roundtrip.moc"
