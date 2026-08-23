#include "utils/ThemeManager.h"

#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QWidget>

ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {}

ThemeManager* ThemeManager::instance() {
    static ThemeManager s_instance;
    return &s_instance;
}

void ThemeManager::init() {
    // Set consistent cross-platform Fusion style
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    if (qApp) {
        qApp->setStyleSheet(QString("QScrollArea { background: transparent; } "
                                    "QScrollArea > QWidget > QWidget { background: transparent; }"));
    }

    // Listen to system color scheme changes dynamically
    if (QGuiApplication::styleHints()) {
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                         [](Qt::ColorScheme scheme) { QApplication::setStyle(QStyleFactory::create("Fusion")); });
    }
}

bool ThemeManager::isMiniPlayer(const QWidget* widget) {
    if (!widget)
        return false;
    for (const QWidget* p = widget; p != nullptr; p = p->parentWidget()) {
        if (p->inherits("MiniPlayerView") || p->objectName() == "MiniPlayerViewWindow" ||
            p->objectName() == "MiniPlayerViewStack") {
            return true;
        }
    }
    return false;
}

QColor ThemeManager::miniPlayerPrimaryTextColor() {
    return QColor(255, 255, 255, 180);
}

QColor ThemeManager::miniPlayerSubtextColor() {
    return QColor(255, 255, 255, 130);
}

QColor ThemeManager::miniPlayerSecondaryTextColor() {
    return QColor(255, 255, 255, 100);
}

QColor ThemeManager::miniPlayerGridColor() {
    return QColor(255, 255, 255, 25);
}

QColor ThemeManager::miniPlayerTrackColor() {
    return QColor(255, 255, 255, 20);
}

QColor ThemeManager::textColor(const QWidget* widget) {
    if (isMiniPlayer(widget))
        return miniPlayerPrimaryTextColor();
    return widget ? widget->palette().color(QPalette::Text) : QColor(Qt::black);
}

QColor ThemeManager::subtextColor(const QWidget* widget) {
    if (isMiniPlayer(widget))
        return miniPlayerSubtextColor();
    return widget ? widget->palette().color(QPalette::PlaceholderText) : QColor(128, 128, 128);
}

QColor ThemeManager::gridColor(const QWidget* widget) {
    if (isMiniPlayer(widget))
        return miniPlayerGridColor();
    return widget ? widget->palette().color(QPalette::Mid) : QColor(200, 200, 200);
}
