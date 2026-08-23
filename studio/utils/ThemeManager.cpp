#include "utils/ThemeManager.h"

#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QProcess>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QWidget>
#include <memory>

AppTheme ThemeManager::s_currentTheme = AppTheme::System;
bool ThemeManager::s_isDarkActive = false;

ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {}

ThemeManager* ThemeManager::instance() {
    static ThemeManager s_instance;
    return &s_instance;
}

void ThemeManager::init() {
    // Set consistent cross-platform widget style
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    if (qApp) {
        QString secondaryColor = s_isDarkActive ? "#a0a0a5" : "#6c6c70";
        qApp->setStyleSheet(QString("QScrollArea { background: transparent; } "
                                    "QScrollArea > QWidget > QWidget { background: transparent; } "
                                    "QTreeWidget#SidebarTree { background-color: palette(window); border: none; }"
                                    "QLabel#secondaryLabel { color: %1; }")
                                .arg(secondaryColor));
    }

    QSettings s("DSPMonitor", "MonitorQt");
    s_currentTheme = static_cast<AppTheme>(s.value("appTheme", static_cast<int>(AppTheme::System)).toInt());

    // Listen to system color scheme changes dynamically
    if (QGuiApplication::styleHints()) {
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, [](Qt::ColorScheme scheme) {
            if (s_currentTheme == AppTheme::System) {
                applyTheme(scheme == Qt::ColorScheme::Dark);
            }
        });
    }

    bool dark = false;
    if (s_currentTheme == AppTheme::Dark) {
        dark = true;
    } else if (s_currentTheme == AppTheme::Light) {
        dark = false;
    } else {
        dark = detectSystemDark();
    }
    applyTheme(dark);
}

void ThemeManager::setTheme(AppTheme theme) {
    s_currentTheme = theme;
    QSettings s("DSPMonitor", "MonitorQt");
    s.setValue("appTheme", static_cast<int>(theme));

    bool dark = false;
    if (theme == AppTheme::Dark) {
        dark = true;
    } else if (theme == AppTheme::Light) {
        dark = false;
    } else {
        dark = detectSystemDark();
    }
    applyTheme(dark);
}

AppTheme ThemeManager::currentTheme() {
    return s_currentTheme;
}

bool ThemeManager::isDarkMode() {
    return s_isDarkActive;
}

bool ThemeManager::detectSystemDark() {
    if (QGuiApplication::styleHints() && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark) {
        return true;
    }

#if defined(Q_OS_LINUX)
    // On Linux desktop environments (e.g. GNOME), Qt's asynchronous portal
    // query may not have settled yet at early launch. Fast-check gsettings:
    QProcess proc;
    proc.start("gsettings", {"get", "org.gnome.desktop.interface", "color-scheme"});
    if (proc.waitForFinished(100)) {
        QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        if (out.contains("prefer-dark")) {
            return true;
        }
    }
#endif

    return false;
}

void ThemeManager::applyTheme(bool dark) {
    s_isDarkActive = dark;
    QPalette pal = dark ? createDarkPalette() : createLightPalette();
    QApplication::setPalette(pal);
    if (QStyle* style = QStyleFactory::create("Fusion")) {
        QApplication::setStyle(style);
    }
    if (qApp) {
        QString secondaryColor = dark ? "#a0a0a5" : "#6c6c70";
        qApp->setStyleSheet(QString("QScrollArea { background: transparent; } "
                                    "QScrollArea > QWidget > QWidget { background: transparent; } "
                                    "QTreeWidget#SidebarTree { background-color: palette(window); border: none; }"
                                    "QLabel#secondaryLabel { color: %1; }")
                                .arg(secondaryColor));
    }
    for (QWidget* top : QApplication::topLevelWidgets()) {
        top->setPalette(pal);
        top->update();
    }
    emit instance() -> themeChanged(s_currentTheme, dark);
}

QPalette ThemeManager::createDarkPalette() {
    QPalette p;
    const QColor windowColor(35, 35, 45);
    const QColor baseColor(24, 24, 26);
    const QColor altBaseColor(38, 38, 40);
    const QColor textColor(240, 240, 240);
    const QColor placeholderColor(140, 140, 145);
    const QColor disabledTextColor(120, 120, 125);
    const QColor buttonColor(44, 44, 46);
    const QColor highlightColor(10, 132, 255);
    const QColor highlightedTextColor(255, 255, 255);
    const QColor midColor(60, 60, 64);
    const QColor lightColor(70, 70, 75);
    const QColor darkColor(18, 18, 20);

    p.setColor(QPalette::Window, windowColor);
    p.setColor(QPalette::WindowText, textColor);
    p.setColor(QPalette::Base, baseColor);
    p.setColor(QPalette::AlternateBase, altBaseColor);
    p.setColor(QPalette::ToolTipBase, windowColor);
    p.setColor(QPalette::ToolTipText, textColor);
    p.setColor(QPalette::Text, textColor);
    p.setColor(QPalette::Button, buttonColor);
    p.setColor(QPalette::ButtonText, textColor);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Link, highlightColor);
    p.setColor(QPalette::Highlight, highlightColor);
    p.setColor(QPalette::HighlightedText, highlightedTextColor);
    p.setColor(QPalette::Light, lightColor);
    p.setColor(QPalette::Midlight, buttonColor);
    p.setColor(QPalette::Dark, darkColor);
    p.setColor(QPalette::Mid, midColor);
    p.setColor(QPalette::Shadow, QColor(0, 0, 0, 180));
    p.setColor(QPalette::PlaceholderText, placeholderColor);

    p.setColor(QPalette::Disabled, QPalette::WindowText, disabledTextColor);
    p.setColor(QPalette::Disabled, QPalette::Text, disabledTextColor);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabledTextColor);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(60, 60, 65));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledTextColor);

    return p;
}

QPalette ThemeManager::createLightPalette() {
    std::unique_ptr<QStyle> style(QStyleFactory::create("Fusion"));
    if (style) {
        return style->standardPalette();
    }
    return QPalette();
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
