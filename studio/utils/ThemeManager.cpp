#include "utils/ThemeManager.h"

#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QProcess>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
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
        qApp->setStyleSheet("QScrollArea { background: transparent; } "
                            "QScrollArea > QWidget > QWidget { background: transparent; }");
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
    if (QGuiApplication::styleHints() &&
        QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark) {
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
    if (dark) {
        QApplication::setPalette(createDarkPalette());
    } else {
        QApplication::setPalette(createLightPalette());
    }
    emit instance()->themeChanged(s_currentTheme, dark);
}

QPalette ThemeManager::createDarkPalette() {
    QPalette p;
    const QColor windowColor(32, 32, 34);
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
