#ifndef THEME_MANAGER_H
#define THEME_MANAGER_H

#include <QObject>
#include <QPalette>

enum class AppTheme { System = 0, Light = 1, Dark = 2 };

class ThemeManager : public QObject {
    Q_OBJECT

public:
    static ThemeManager* instance();

    static void init();
    static void setTheme(AppTheme theme);
    static AppTheme currentTheme();
    static bool isDarkMode();

    static QPalette createDarkPalette();
    static QPalette createLightPalette();

signals:
    void themeChanged(AppTheme theme, bool isDark);

private:
    explicit ThemeManager(QObject* parent = nullptr);
    static void applyTheme(bool dark);
    static bool detectSystemDark();

    static AppTheme s_currentTheme;
    static bool s_isDarkActive;
};

#endif // THEME_MANAGER_H
