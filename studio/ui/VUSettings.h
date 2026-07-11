#ifndef VU_SETTINGS_H
#define VU_SETTINGS_H

#include <QSettings>

enum class VUTheme {
    VintageAmber = 0,
    DarkStealth = 1,
    WarmTube = 2
};

struct VUSettings {
    double radiusScale = 0.85;
    double pivotY = 1.30;
    double needleExtension = 15.0;
    double ambientGlow = 0.5;
    double hotSpotAlpha = 0.5;
    double lightWash = 0.2;
    VUTheme theme = VUTheme::VintageAmber;

    VUSettings();

    void load();
    void save() const;
    void reset();
};

#endif // VU_SETTINGS_H
