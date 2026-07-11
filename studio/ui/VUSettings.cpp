#include "ui/VUSettings.h"

VUSettings::VUSettings() {
    load();
}

void VUSettings::load() {
    QSettings settings;
    radiusScale = settings.value("vu_radius_scale", 0.85).toDouble();
    pivotY = settings.value("vu_pivot_y", 1.30).toDouble();
    needleExtension = settings.value("vu_needle_extension", 15.0).toDouble();
    ambientGlow = settings.value("vu_ambient_glow", 0.5).toDouble();
    hotSpotAlpha = settings.value("vu_hot_spot_alpha", 0.5).toDouble();
    lightWash = settings.value("vu_light_wash", 0.2).toDouble();

    if (radiusScale > 0.95 || pivotY > 1.35 || needleExtension > 25.0) {
        radiusScale = 0.85;
        pivotY = 1.30;
        needleExtension = 15.0;
        save();
    }

    int t = settings.value("vu_theme", static_cast<int>(VUTheme::VintageAmber)).toInt();
    if (t >= 0 && t <= 2) {
        theme = static_cast<VUTheme>(t);
    } else {
        theme = VUTheme::VintageAmber;
    }
}

void VUSettings::save() const {
    QSettings settings;
    settings.setValue("vu_radius_scale", radiusScale);
    settings.setValue("vu_pivot_y", pivotY);
    settings.setValue("vu_needle_extension", needleExtension);
    settings.setValue("vu_ambient_glow", ambientGlow);
    settings.setValue("vu_hot_spot_alpha", hotSpotAlpha);
    settings.setValue("vu_light_wash", lightWash);
    settings.setValue("vu_theme", static_cast<int>(theme));
}

void VUSettings::reset() {
    radiusScale = 0.85;
    pivotY = 1.30;
    needleExtension = 15.0;
    ambientGlow = 0.5;
    hotSpotAlpha = 0.5;
    lightWash = 0.2;
    theme = VUTheme::VintageAmber;
    save();
}
