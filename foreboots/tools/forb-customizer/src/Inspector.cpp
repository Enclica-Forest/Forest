#include "Inspector.h"
#include "ConfigModel.h"
#include "Schema.h"
#include "Bound.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>

Inspector::Inspector(ConfigModel *m, QWidget *parent) : QWidget(parent), model(m) {
    lay = new QVBoxLayout(this);
    header = new QLabel("Click an element in the preview", this);
    header->setStyleSheet("font-weight:bold;");
    header->setWordWrap(true);
    lay->addWidget(header);
    lay->addStretch(1);
}

void Inspector::showElement(const QString &kind, int index) {
    if (body) { body->deleteLater(); body = nullptr; }
    body = new QWidget(this);
    auto *form = new QFormLayout(body);
    Theme &t = model->th;

    if (kind == "panel") {
        header->setText("Panel / menu style");
        form->addRow("menu_pos", new EnumComboWidget(&t.menuPos, model, Schema::menuPos(), body));
        form->addRow("menu_border", new EnumComboWidget(&t.menuBorder, model, Schema::borderStyles(), body));
        form->addRow("menu_corner", new EnumComboWidget(&t.menuCorner, model, Schema::corners(), body));
        form->addRow("menu_gradient", new OptBoolWidget(&t.menuGradient, model, body));
        form->addRow("menu_shadow", new OptBoolWidget(&t.menuShadow, model, body));
        form->addRow("menu_accent_strip", new OptBoolWidget(&t.menuAccentStrip, model, body));
        form->addRow("color_window", new OptColorWidget(&t.colorWindow, model, body));
        form->addRow("ui_panel_alpha", new OptIntWidget(&t.uiPanelAlpha, model, 0, 255, "", true, body));
    } else if (kind == "entry") {
        header->setText(QString("Entry row #%1 - selection & colours").arg(index));
        form->addRow("menu_selection", new EnumComboWidget(&t.menuSelection, model, Schema::selStyles(), body));
        form->addRow("menu_align", new EnumComboWidget(&t.menuAlign, model, Schema::menuAlign(), body));
        form->addRow("color_fg", new OptColorWidget(&t.colorFg, model, body));
        form->addRow("color_sel_bg", new OptColorWidget(&t.colorSelBg, model, body));
        form->addRow("color_sel_fg", new OptColorWidget(&t.colorSelFg, model, body));
        form->addRow("color_accent", new OptColorWidget(&t.colorAccent, model, body));
        form->addRow("menu_show_icons", new OptBoolWidget(&t.menuShowIcons, model, body));
    } else if (kind == "button") {
        header->setText("Button style (btn_*)");
        form->addRow("btn_style", new EnumComboWidget(&t.btnStyle, model, Schema::btnStyles(), body));
        form->addRow("btn_corner", new EnumComboWidget(&t.btnCorner, model, Schema::corners(), body));
        form->addRow("btn_gradient", new OptBoolWidget(&t.btnGradient, model, body));
        form->addRow("btn_shadow", new OptBoolWidget(&t.btnShadow, model, body));
        form->addRow("btn_fill", new OptColorWidget(&t.btnFill, model, body));
        form->addRow("btn_text", new OptColorWidget(&t.btnText, model, body));
        form->addRow("btn_border_color", new OptColorWidget(&t.btnBorderColor, model, body));
    } else if (kind == "window") {
        header->setText("Window skin (window_skin / win_*)");
        form->addRow("window_skin", new EnumComboWidget(&t.windowSkin, model, Schema::windowSkins(), body));
        form->addRow("win_corner", new EnumComboWidget(&t.winCorner, model, Schema::corners(), body));
        form->addRow("win_title_h", new OptIntWidget(&t.winTitleH, model, -1, 128, "px", false, body));
        form->addRow("win_border_w", new OptIntWidget(&t.winBorderW, model, 0, 16, "px", false, body));
        form->addRow("win_title_fill", new OptColorWidget(&t.winTitleFill, model, body));
        form->addRow("win_title_fg", new OptColorWidget(&t.winTitleFg, model, body));
        form->addRow("win_border_color", new OptColorWidget(&t.winBorderColor, model, body));
        form->addRow("win_close_color", new OptColorWidget(&t.winCloseColor, model, body));
        form->addRow("win_shadow", new OptBoolWidget(&t.winShadow, model, body));
        form->addRow("color_titlebar", new OptColorWidget(&t.colorTitlebar, model, body));
    } else {
        header->setText("Click an element in the preview");
    }
    lay->insertWidget(1, body);
}
