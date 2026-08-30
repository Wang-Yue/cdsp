#ifndef THEME_MANAGER_H
#define THEME_MANAGER_H

#include <QColor>  // for QColor
#include <QObject> // for QObject, Q_OBJECT
#include <QWidget> // for QWidget

class ThemeManager : public QObject {
    Q_OBJECT

public:
    static ThemeManager* instance();

    static void init();

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

private:
    explicit ThemeManager(QObject* parent = nullptr);
};

#endif // THEME_MANAGER_H
