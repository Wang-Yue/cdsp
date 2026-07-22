#ifndef STYLE_THEME_H
#define STYLE_THEME_H

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QString>

enum class AppTheme { Light, Dark };

class StyleTheme {
public:
    static QString lightStylesheet() {
        return R"(
            /* macOS Apple Light Theme */
            QMainWindow, QDialog, QMessageBox {
                background-color: #ffffff;
                color: #000000;
            }
            QWidget {
                font-family: -apple-system, BlinkMacSystemFont, "SF Pro Text", "Helvetica Neue", "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
                font-size: 13px;
                color: #000000;
            }
            QLabel {
                color: #000000;
            }

            /* Tooltips */
            QToolTip {
                background-color: #ffffff;
                color: #000000;
                border: 1px solid #d1d1d6;
                border-radius: 6px;
                padding: 4px 8px;
            }

            /* Sidebar, Trees, Lists, Tables */
            QTreeWidget, QListWidget, QTableWidget {
                background-color: #f5f5f7;
                border: 1px solid #d1d1d6;
                border-radius: 8px;
                padding: 4px;
                color: #000000;
                gridline-color: #e5e5ea;
            }
            QTreeWidget::item, QListWidget::item, QTableWidget::item {
                padding: 6px 8px;
                border-radius: 4px;
                color: #000000;
            }
            QTreeWidget::item:hover, QListWidget::item:hover, QTableWidget::item:hover {
                background-color: #e5e5ea;
            }
            QTreeWidget::item:selected, QListWidget::item:selected, QTableWidget::item:selected {
                background-color: #007aff;
                color: #ffffff;
            }
            QHeaderView::section {
                background-color: #e5e5ea;
                color: #3a3a3c;
                padding: 6px;
                border: none;
                border-bottom: 1px solid #d1d1d6;
                font-weight: 600;
            }

            /* Context Popup & Dropdown Menus */
            QMenu {
                background-color: #ffffff;
                color: #000000;
                border: 1px solid #d1d1d6;
                border-radius: 8px;
                padding: 6px 0px;
            }
            QMenu::item {
                background-color: transparent;
                padding: 6px 24px 6px 14px;
                color: #000000;
                font-size: 13px;
            }
            QMenu::item:selected {
                background-color: #007aff;
                color: #ffffff;
            }
            QMenu::item:disabled {
                color: #8e8e93;
            }
            QMenu::separator {
                height: 1px;
                background-color: #e5e5ea;
                margin: 4px 0px;
            }

            /* Group Box */
            QGroupBox {
                background-color: #f5f5f7;
                border: 1px solid #d1d1d6;
                border-radius: 8px;
                margin-top: 14px;
                padding-top: 16px;
                font-weight: bold;
                color: #000000;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                subcontrol-position: top left;
                padding: 0 8px;
                color: #6c6c70;
            }

            /* Buttons */
            QPushButton {
                background-color: #e5e5ea;
                color: #000000;
                border: 1px solid #c6c6c8;
                border-radius: 6px;
                padding: 6px 14px;
                font-weight: 600;
            }
            QPushButton:hover {
                background-color: #d1d1d6;
            }
            QPushButton:pressed {
                background-color: #b0b0b8;
            }
            QPushButton:disabled {
                background-color: #f2f2f7;
                color: #8e8e93;
                border: 1px solid #e5e5ea;
            }

            /* Form Input Controls */
            QLineEdit, QTextEdit, QPlainTextEdit {
                background-color: #ffffff;
                border: 1px solid #c6c6c8;
                border-radius: 6px;
                padding: 4px 8px;
                color: #000000;
            }
            QSpinBox, QDoubleSpinBox {
                background-color: #ffffff;
                border: 1px solid #c6c6c8;
                border-radius: 6px;
                padding: 4px 20px 4px 6px;
                color: #000000;
            }
            QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QTextEdit:focus, QPlainTextEdit:focus {
                border: 1px solid #007aff;
            }

            /* ComboBox & Dropdown View */
            QComboBox {
                background-color: #ffffff;
                border: 1px solid #c6c6c8;
                border-radius: 6px;
                padding: 5px 8px;
                color: #000000;
            }
            QComboBox:focus {
                border: 1px solid #007aff;
            }
            QComboBox::drop-down {
                border: none;
                width: 20px;
            }
            QComboBox QAbstractItemView {
                background-color: #ffffff;
                color: #000000;
                border: 1px solid #d1d1d6;
                border-radius: 6px;
                selection-background-color: #007aff;
                selection-color: #ffffff;
                padding: 4px;
            }

            /* Checkboxes & Radio Buttons */
            QCheckBox, QRadioButton {
                color: #000000;
                spacing: 6px;
            }
            QCheckBox::indicator, QRadioButton::indicator {
                width: 16px;
                height: 16px;
                border-radius: 3px;
                border: 1px solid #c6c6c8;
                background-color: #ffffff;
            }
            QCheckBox::indicator:checked, QRadioButton::indicator:checked {
                background-color: #007aff;
                border: 1px solid #007aff;
            }

            /* Sliders */
            QSlider::groove:horizontal {
                height: 6px;
                background: #e5e5ea;
                border-radius: 3px;
            }
            QSlider::sub-page:horizontal {
                background: #007aff;
                border-radius: 3px;
            }
            QSlider::handle:horizontal {
                background: #ffffff;
                border: 1px solid #c6c6c8;
                width: 18px;
                height: 18px;
                margin-top: -6px;
                margin-bottom: -6px;
                border-radius: 9px;
            }
            QSlider::groove:vertical {
                width: 6px;
                background: #e5e5ea;
                border-radius: 3px;
            }
            QSlider::add-page:vertical {
                background: #007aff;
                border-radius: 3px;
            }
            QSlider::handle:vertical {
                background: #ffffff;
                border: 1px solid #c6c6c8;
                width: 18px;
                height: 18px;
                margin-left: -6px;
                margin-right: -6px;
                border-radius: 9px;
            }

            /* Tabs */
            QTabWidget::pane {
                border: 1px solid #d1d1d6;
                border-radius: 8px;
                background-color: #f5f5f7;
            }
            QTabBar::tab {
                background: #e5e5ea;
                color: #6c6c70;
                padding: 8px 16px;
                border-top-left-radius: 6px;
                border-top-right-radius: 6px;
                font-weight: 600;
            }
            QTabBar::tab:selected {
                background: #f5f5f7;
                color: #000000;
                border-bottom: 2px solid #007aff;
            }

            /* Scrollbars */
            QScrollBar:vertical, QScrollBar:horizontal {
                background: #ffffff;
                width: 10px;
                height: 10px;
                border-radius: 5px;
            }
            QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
                background: #d1d1d6;
                border-radius: 5px;
            }
            QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover {
                background: #8e8e93;
            }

            QToolBar {
                background-color: #f5f5f7;
                border-bottom: 1px solid #d1d1d6;
                spacing: 6px;
                padding: 4px;
            }
            QToolBar QLabel {
                color: #000000;
            }
            QStatusBar {
                background-color: #f5f5f7;
                color: #000000;
                border-top: 1px solid #d1d1d6;
            }
            QStatusBar QLabel {
                color: #000000;
            }
            QSplitter::handle {
                background-color: #d1d1d6;
            }
            QSplitter::handle:horizontal {
                width: 1px;
            }
            QSplitter::handle:vertical {
                height: 1px;
            }

            /* Custom Dynamic Properties - Light */
            QFrame[themeBorder="true"] {
                color: #d1d1d6;
            }
            QLabel[secondary="true"] {
                color: #6c6c70;
            }
            QToolBar QLabel[secondary="true"] {
                padding: 0 8px;
            }
            QStatusBar QLabel {
                padding: 0 8px;
            }
            QLabel[badge="sample-rate"] {
                color: #007aff;
                background-color: rgba(0, 122, 255, 0.15);
                border-radius: 4px;
                font-weight: bold;
                font-family: monospace;
                padding: 2px 8px;
            }
            QLabel[banner="error"] {
                padding: 2px 8px;
                color: #ffffff;
                background-color: #ff3b30;
                border-radius: 4px;
                font-weight: bold;
            }
            QLabel[muteState="muted"] {
                color: #ff3b30;
                font-weight: bold;
            }
            QLabel[muteState="unmuted"] {
                color: #34c759;
                font-weight: bold;
            }
            QPushButton[muteState="muted"] {
                background-color: transparent;
                color: #ff3b30;
                font-size: 14px;
                padding: 2px 4px;
                border: none;
            }
            QPushButton[muteState="muted"]:hover {
                background-color: rgba(255, 59, 48, 0.1);
                border-radius: 4px;
            }
            QPushButton[muteState="unmuted"] {
                background-color: transparent;
                color: #000000;
                font-size: 14px;
                padding: 2px 4px;
                border: none;
            }
            QPushButton[muteState="unmuted"]:hover {
                background-color: rgba(0, 0, 0, 0.05);
                border-radius: 4px;
            }
            QLabel[state="running"] {
                color: #34c759;
                font-weight: bold;
            }
            QLabel[state="warning"] {
                color: #ff9500;
                font-weight: bold;
            }
            QLabel[state="inactive"] {
                color: #ff3b30;
                font-weight: bold;
            }
            QPushButton[state="running"] {
                background-color: transparent;
                color: #ff3b30;
                font-weight: bold;
                padding: 4px 8px;
                border: none;
            }
            QPushButton[state="running"]:hover {
                background-color: rgba(255, 59, 48, 0.1);
                border-radius: 4px;
            }
            QPushButton[state="warning"] {
                background-color: transparent;
                color: #ff9500;
                font-weight: bold;
                padding: 4px 8px;
                border: none;
            }
            QPushButton[state="inactive"] {
                background-color: transparent;
                color: #34c759;
                font-weight: bold;
                padding: 4px 8px;
                border: none;
            }
            QPushButton[state="inactive"]:hover {
                background-color: rgba(52, 199, 89, 0.1);
                border-radius: 4px;
            }
            QLabel[clipping="true"] {
                font-family: monospace;
                font-size: 10pt;
                color: #ff3b30;
                padding-right: 10px;
            }
            QLabel[clipping="false"] {
                font-family: monospace;
                font-size: 10pt;
                color: #000000;
                padding-right: 10px;
            }
        )";
    }

    static QString darkStylesheet() {
        return R"(
            /* Modern High-Contrast Dark Theme */
            QMainWindow, QDialog, QMessageBox {
                background-color: #121318;
                color: #ffffff;
            }
            QWidget {
                font-family: -apple-system, BlinkMacSystemFont, "SF Pro Text", "Helvetica Neue", "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
                font-size: 13px;
                color: #ffffff;
            }
            QLabel {
                color: #ffffff;
            }

            /* Tooltips */
            QToolTip {
                background-color: #1e2029;
                color: #ffffff;
                border: 1px solid #343746;
                border-radius: 6px;
                padding: 4px 8px;
            }

            QTreeWidget, QListWidget, QTableWidget {
                background-color: #1a1b22;
                border: 1px solid #2c2d3a;
                border-radius: 8px;
                padding: 4px;
                color: #ffffff;
                gridline-color: #2c2d3a;
            }
            QTreeWidget::item, QListWidget::item, QTableWidget::item {
                padding: 6px 8px;
                border-radius: 4px;
                color: #ffffff;
            }
            QTreeWidget::item:hover, QListWidget::item:hover, QTableWidget::item:hover {
                background-color: #282a36;
            }
            QTreeWidget::item:selected, QListWidget::item:selected, QTableWidget::item:selected {
                background-color: #007af5;
                color: #ffffff;
            }
            QHeaderView::section {
                background-color: #121318;
                color: #a0a5b5;
                padding: 6px;
                border: none;
                border-bottom: 1px solid #2c2d3a;
                font-weight: 600;
            }

            QMenu {
                background-color: #1e2029;
                color: #ffffff;
                border: 1px solid #343746;
                border-radius: 8px;
                padding: 6px 0px;
            }
            QMenu::item {
                background-color: transparent;
                padding: 6px 24px 6px 14px;
                color: #ffffff;
                font-size: 13px;
            }
            QMenu::item:selected {
                background-color: #007af5;
                color: #ffffff;
            }
            QMenu::item:disabled {
                color: #5a5f73;
            }
            QMenu::separator {
                height: 1px;
                background-color: #2c2d3a;
                margin: 4px 0px;
            }

            QGroupBox {
                background-color: #1a1b22;
                border: 1px solid #2c2d3a;
                border-radius: 8px;
                margin-top: 14px;
                padding-top: 16px;
                font-weight: bold;
                color: #ffffff;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                subcontrol-position: top left;
                padding: 0 8px;
                color: #ffffff;
            }

            QPushButton {
                background-color: #2c2d3a;
                color: #ffffff;
                border: 1px solid #3a3d4e;
                border-radius: 6px;
                padding: 6px 14px;
                font-weight: 600;
            }
            QPushButton:hover {
                background-color: #3a3d4e;
            }
            QPushButton:pressed {
                background-color: #20212b;
            }
            QPushButton:disabled {
                background-color: #1a1b22;
                color: #5a5f73;
                border: 1px solid #282a36;
            }

            QLineEdit, QTextEdit, QPlainTextEdit {
                background-color: #121318;
                border: 1px solid #2c2d3a;
                border-radius: 6px;
                padding: 4px 8px;
                color: #ffffff;
            }
            QSpinBox, QDoubleSpinBox {
                background-color: #121318;
                border: 1px solid #2c2d3a;
                border-radius: 6px;
                padding: 4px 20px 4px 6px;
                color: #ffffff;
            }
            QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QTextEdit:focus, QPlainTextEdit:focus {
                border: 1px solid #007af5;
            }

            QComboBox {
                background-color: #121318;
                border: 1px solid #2c2d3a;
                border-radius: 6px;
                padding: 5px 8px;
                color: #ffffff;
            }
            QComboBox:focus {
                border: 1px solid #007af5;
            }
            QComboBox::drop-down {
                border: none;
                width: 20px;
            }
            QComboBox QAbstractItemView {
                background-color: #1e2029;
                color: #ffffff;
                border: 1px solid #343746;
                border-radius: 6px;
                selection-background-color: #007af5;
                selection-color: #ffffff;
                padding: 4px;
            }

            QCheckBox, QRadioButton {
                color: #ffffff;
                spacing: 6px;
            }
            QCheckBox::indicator, QRadioButton::indicator {
                width: 16px;
                height: 16px;
                border-radius: 3px;
                border: 1px solid #424659;
                background-color: #121318;
            }
            QCheckBox::indicator:checked, QRadioButton::indicator:checked {
                background-color: #007af5;
                border: 1px solid #007af5;
            }

            QSlider::groove:horizontal {
                height: 6px;
                background: #2c2d3a;
                border-radius: 3px;
            }
            QSlider::sub-page:horizontal {
                background: #007af5;
                border-radius: 3px;
            }
            QSlider::handle:horizontal {
                background: #ffffff;
                width: 18px;
                height: 18px;
                margin-top: -6px;
                margin-bottom: -6px;
                border-radius: 9px;
            }
            QSlider::groove:vertical {
                width: 6px;
                background: #2c2d3a;
                border-radius: 3px;
            }
            QSlider::add-page:vertical {
                background: #007af5;
                border-radius: 3px;
            }
            QSlider::handle:vertical {
                background: #ffffff;
                width: 18px;
                height: 18px;
                margin-left: -6px;
                margin-right: -6px;
                border-radius: 9px;
            }

            QTabWidget::pane {
                border: 1px solid #2c2d3a;
                border-radius: 8px;
                background-color: #1a1b22;
            }
            QTabBar::tab {
                background: #121318;
                color: #a0a5b5;
                padding: 8px 16px;
                border-top-left-radius: 6px;
                border-top-right-radius: 6px;
                font-weight: 600;
            }
            QTabBar::tab:selected {
                background: #1a1b22;
                color: #ffffff;
                border-bottom: 2px solid #007af5;
            }

            QScrollBar:vertical, QScrollBar:horizontal {
                background: #121318;
                width: 10px;
                height: 10px;
                border-radius: 5px;
            }
            QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
                background: #2c2d3a;
                border-radius: 5px;
            }
            QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover {
                background: #424659;
            }

            QToolBar {
                background-color: #1a1b22;
                border-bottom: 1px solid #2c2d3a;
                spacing: 6px;
                padding: 4px;
            }
            QToolBar QLabel {
                color: #ffffff;
            }
            QStatusBar {
                background-color: #1a1b22;
                color: #ffffff;
                border-top: 1px solid #2c2d3a;
            }
            QStatusBar QLabel {
                color: #ffffff;
            }
            QSplitter::handle {
                background-color: #2c2d3a;
            }
            QSplitter::handle:horizontal {
                width: 1px;
            }
            QSplitter::handle:vertical {
                height: 1px;
            }

            /* Custom Dynamic Properties - Dark */
            QFrame[themeBorder="true"] {
                color: #2c2d3a;
            }
            QLabel[secondary="true"] {
                color: #a0a5b5;
            }
            QToolBar QLabel[secondary="true"] {
                padding: 0 8px;
            }
            QStatusBar QLabel {
                padding: 0 8px;
            }
            QLabel[badge="sample-rate"] {
                color: #007af5;
                background-color: rgba(0, 122, 245, 0.15);
                border-radius: 4px;
                font-weight: bold;
                font-family: monospace;
                padding: 2px 8px;
            }
            QLabel[banner="error"] {
                padding: 2px 8px;
                color: #ffffff;
                background-color: #ff453a;
                border-radius: 4px;
                font-weight: bold;
            }
            QLabel[muteState="muted"] {
                color: #ff453a;
                font-weight: bold;
            }
            QLabel[muteState="unmuted"] {
                color: #2cb67d;
                font-weight: bold;
            }
            QPushButton[muteState="muted"] {
                background-color: transparent;
                color: #ff453a;
                font-size: 14px;
                padding: 2px 4px;
                border: none;
            }
            QPushButton[muteState="muted"]:hover {
                background-color: rgba(255, 69, 58, 0.15);
                border-radius: 4px;
            }
            QPushButton[muteState="unmuted"] {
                background-color: transparent;
                color: #ffffff;
                font-size: 14px;
                padding: 2px 4px;
                border: none;
            }
            QPushButton[muteState="unmuted"]:hover {
                background-color: rgba(255, 255, 255, 0.1);
                border-radius: 4px;
            }
            QLabel[state="running"] {
                color: #2cb67d;
                font-weight: bold;
            }
            QLabel[state="warning"] {
                color: #ff9f0a;
                font-weight: bold;
            }
            QLabel[state="inactive"] {
                color: #ff453a;
                font-weight: bold;
            }
            QPushButton[state="running"] {
                background-color: transparent;
                color: #ff453a;
                font-weight: bold;
                padding: 4px 8px;
                border: none;
            }
            QPushButton[state="running"]:hover {
                background-color: rgba(255, 69, 58, 0.15);
                border-radius: 4px;
            }
            QPushButton[state="warning"] {
                background-color: transparent;
                color: #ff9f0a;
                font-weight: bold;
                padding: 4px 8px;
                border: none;
            }
            QPushButton[state="inactive"] {
                background-color: transparent;
                color: #2cb67d;
                font-weight: bold;
                padding: 4px 8px;
                border: none;
            }
            QPushButton[state="inactive"]:hover {
                background-color: rgba(44, 182, 125, 0.15);
                border-radius: 4px;
            }
            QLabel[clipping="true"] {
                font-family: monospace;
                font-size: 10pt;
                color: #ff453a;
                padding-right: 10px;
            }
            QLabel[clipping="false"] {
                font-family: monospace;
                font-size: 10pt;
                color: #ffffff;
                padding-right: 10px;
            }
        )";
    }

    inline static AppTheme s_theme = AppTheme::Light;

    static void setTheme(AppTheme theme) { s_theme = theme; }

    static AppTheme theme() { return s_theme; }

    static bool isDark() { return s_theme == AppTheme::Dark; }

    static QString currentStylesheet() { return (s_theme == AppTheme::Dark) ? darkStylesheet() : lightStylesheet(); }

    static QPalette darkPalette() {
        QPalette p;
        p.setColor(QPalette::Window, QColor("#121318"));
        p.setColor(QPalette::WindowText, QColor("#ffffff"));
        p.setColor(QPalette::Base, QColor("#1a1b22"));
        p.setColor(QPalette::AlternateBase, QColor("#282a36"));
        p.setColor(QPalette::ToolTipBase, QColor("#1e2029"));
        p.setColor(QPalette::ToolTipText, QColor("#ffffff"));
        p.setColor(QPalette::Text, QColor("#ffffff"));
        p.setColor(QPalette::Button, QColor("#1f212a"));
        p.setColor(QPalette::ButtonText, QColor("#ffffff"));
        p.setColor(QPalette::BrightText, QColor("#ff3b30"));
        p.setColor(QPalette::Link, QColor("#007af5"));
        p.setColor(QPalette::Highlight, QColor("#007af5"));
        p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
        p.setColor(QPalette::Disabled, QPalette::Text, QColor("#5a5f73"));
        p.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#5a5f73"));
        p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#5a5f73"));
        return p;
    }

    static QPalette lightPalette() {
        QPalette p;
        p.setColor(QPalette::Window, QColor("#ffffff"));
        p.setColor(QPalette::WindowText, QColor("#000000"));
        p.setColor(QPalette::Base, QColor("#f5f5f7"));
        p.setColor(QPalette::AlternateBase, QColor("#e5e5ea"));
        p.setColor(QPalette::ToolTipBase, QColor("#ffffff"));
        p.setColor(QPalette::ToolTipText, QColor("#000000"));
        p.setColor(QPalette::Text, QColor("#000000"));
        p.setColor(QPalette::Button, QColor("#f5f5f7"));
        p.setColor(QPalette::ButtonText, QColor("#000000"));
        p.setColor(QPalette::BrightText, QColor("#ff3b30"));
        p.setColor(QPalette::Link, QColor("#007aff"));
        p.setColor(QPalette::Highlight, QColor("#007aff"));
        p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
        p.setColor(QPalette::Disabled, QPalette::Text, QColor("#8e8e93"));
        p.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#8e8e93"));
        p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#8e8e93"));
        return p;
    }

    static void applyTheme(QApplication* app, AppTheme theme) {
        setTheme(theme);
        app->setPalette(isDark() ? darkPalette() : lightPalette());
        app->setStyleSheet(currentStylesheet());
    }

    static QColor windowBg() { return isDark() ? QColor("#121318") : QColor("#ffffff"); }
    static QColor cardBg() { return isDark() ? QColor("#1a1b22") : QColor("#f5f5f7"); }
    static QColor border() { return isDark() ? QColor("#2c2d3a") : QColor("#d1d1d6"); }
    static QColor accent() { return isDark() ? QColor("#007af5") : QColor("#007aff"); }
    static QColor accentGreen() { return isDark() ? QColor("#2cb67d") : QColor("#34c759"); }
    static QColor accentRed() { return isDark() ? QColor("#ff453a") : QColor("#ff3b30"); }
    static QColor accentOrange() { return isDark() ? QColor("#ff9f0a") : QColor("#ff9500"); }
    static QColor trackBg() { return isDark() ? QColor(255, 255, 255, 25) : QColor(0, 0, 0, 25); }
    static QColor textPrimary() { return isDark() ? QColor("#ffffff") : QColor("#000000"); }
    static QColor textSecondary() { return isDark() ? QColor("#a0a5b5") : QColor("#6c6c70"); }
    static QColor gridPenColor() { return isDark() ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 35); }
    static QColor axisLabelPenColor() { return isDark() ? QColor("#a0a5b5") : QColor("#444446"); }
    static QColor bgDark() { return windowBg(); }
};

#endif // STYLE_THEME_H
