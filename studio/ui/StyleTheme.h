#ifndef STYLE_THEME_H
#define STYLE_THEME_H

#include <QColor>
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
                font-family: -apple-system, BlinkMacSystemFont, "SF Pro Text", "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
                font-size: 13px;
                color: #000000;
            }
            QLabel {
                color: #000000;
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
                background-color: #34c759;
                color: #ffffff;
                border: none;
                border-radius: 6px;
                padding: 6px 14px;
                font-weight: 600;
            }
            QPushButton:hover {
                background-color: #30b753;
            }
            QPushButton:pressed {
                background-color: #289a46;
            }
            QPushButton:disabled {
                background-color: #e5e5ea;
                color: #8e8e93;
            }

            /* Form Input Controls */
            QLineEdit, QSpinBox, QDoubleSpinBox, QTextEdit, QPlainTextEdit {
                background-color: #ffffff;
                border: 1px solid #c6c6c8;
                border-radius: 6px;
                padding: 5px 8px;
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
                height: 4px;
                background: #e5e5ea;
                border-radius: 2px;
            }
            QSlider::sub-page:horizontal {
                background: #007aff;
                border-radius: 2px;
            }
            QSlider::handle:horizontal {
                background: #ffffff;
                border: 1px solid #c6c6c8;
                width: 14px;
                margin-top: -5px;
                margin-bottom: -5px;
                border-radius: 7px;
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
                font-family: -apple-system, BlinkMacSystemFont, "SF Pro Text", "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
                font-size: 13px;
                color: #ffffff;
            }
            QLabel {
                color: #ffffff;
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
                color: #a0a5b5;
            }

            QPushButton {
                background-color: #2cb67d;
                color: #ffffff;
                border: none;
                border-radius: 6px;
                padding: 6px 14px;
                font-weight: 600;
            }
            QPushButton:hover {
                background-color: #35cd8c;
            }
            QPushButton:pressed {
                background-color: #249969;
            }
            QPushButton:disabled {
                background-color: #282a36;
                color: #5a5f73;
            }

            QLineEdit, QSpinBox, QDoubleSpinBox, QTextEdit, QPlainTextEdit {
                background-color: #121318;
                border: 1px solid #2c2d3a;
                border-radius: 6px;
                padding: 5px 8px;
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
                height: 4px;
                background: #2c2d3a;
                border-radius: 2px;
            }
            QSlider::sub-page:horizontal {
                background: #007af5;
                border-radius: 2px;
            }
            QSlider::handle:horizontal {
                background: #ffffff;
                width: 14px;
                margin-top: -5px;
                margin-bottom: -5px;
                border-radius: 7px;
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
        )";
    }

    inline static AppTheme s_theme = AppTheme::Light;

    static void setTheme(AppTheme theme) { s_theme = theme; }

    static AppTheme theme() { return s_theme; }

    static bool isDark() { return s_theme == AppTheme::Dark; }

    static QString currentStylesheet() { return (s_theme == AppTheme::Dark) ? darkStylesheet() : lightStylesheet(); }

    static QColor windowBg() { return isDark() ? QColor("#121318") : QColor("#ffffff"); }
    static QColor cardBg() { return isDark() ? QColor("#1a1b22") : QColor("#f5f5f7"); }
    static QColor border() { return isDark() ? QColor("#2c2d3a") : QColor("#d1d1d6"); }
    static QColor accent() { return isDark() ? QColor("#007af5") : QColor("#007aff"); }
    static QColor accentGreen() { return isDark() ? QColor("#2cb67d") : QColor("#34c759"); }
    static QColor textPrimary() { return isDark() ? QColor("#ffffff") : QColor("#000000"); }
    static QColor textSecondary() { return isDark() ? QColor("#a0a5b5") : QColor("#6c6c70"); }
    static QColor gridPenColor() { return isDark() ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 35); }
    static QColor axisLabelPenColor() { return isDark() ? QColor("#a0a5b5") : QColor("#444446"); }
    static QColor bgDark() { return windowBg(); }
};

#endif // STYLE_THEME_H
