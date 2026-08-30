#ifndef VU_SETTINGS_H
#define VU_SETTINGS_H

enum class VUTheme { VintageAmber = 0, DarkStealth = 1, WarmTube = 2 };

struct VUSettings {
    double radiusScale = 1.20;
    double pivotY = 1.55;
    double needleExtension = 45.0;
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
