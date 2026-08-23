#ifndef THEME_MANAGER_H
#define THEME_MANAGER_H

#include <QColor>
#include <QObject>

class QWidget;

class ThemeManager : public QObject {
    Q_OBJECT

public:
    static ThemeManager* instance();

    static void init();
    static bool isDarkMode();

    // MiniPlayer helper & colors
    static bool isMiniPlayer(const QWidget* widget);
    static QColor miniPlayerPrimaryTextColor();
    static QColor miniPlayerSubtextColor();
    static QColor miniPlayerSecondaryTextColor();
    static QColor miniPlayerGridColor();
    static QColor miniPlayerTrackColor();

    // Context-aware color resolvers (MiniPlayer vs standard window)
    static QColor textColor(const QWidget* widget);
    static QColor subtextColor(const QWidget* widget);
    static QColor gridColor(const QWidget* widget);

signals:
    void themeChanged(bool isDark);

private:
    explicit ThemeManager(QObject* parent = nullptr);
};

#endif // THEME_MANAGER_H
