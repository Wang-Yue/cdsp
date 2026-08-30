#include "ui/VUSettings.h"

#include <QSettings> // for QSettings
#include <QString>   // for QString, operator==
#include <QVariant>  // for QVariant

VUSettings::VUSettings() {
    load();
}

void VUSettings::load() {
    QSettings settings("DSPMonitor", "MonitorQt");
    radiusScale = settings.value("vu_radius_scale", 1.20).toDouble();
    pivotY = settings.value("vu_pivot_y", 1.55).toDouble();
    needleExtension = settings.value("vu_needle_extension", 45.0).toDouble();
    ambientGlow = settings.value("vu_ambient_glow", 0.5).toDouble();
    hotSpotAlpha = settings.value("vu_hot_spot_alpha", 0.5).toDouble();
    lightWash = settings.value("vu_light_wash", 0.2).toDouble();

    if (settings.contains("vu_theme")) {
        QString tStr = settings.value("vu_theme").toString();
        if (tStr == "Dark Stealth") {
            theme = VUTheme::DarkStealth;
        } else if (tStr == "Warm Tube") {
            theme = VUTheme::WarmTube;
        } else {
            theme = VUTheme::VintageAmber;
        }
    } else {
        theme = VUTheme::VintageAmber;
    }
}

void VUSettings::save() const {
    QSettings settings("DSPMonitor", "MonitorQt");
    settings.setValue("vu_radius_scale", radiusScale);
    settings.setValue("vu_pivot_y", pivotY);
    settings.setValue("vu_needle_extension", needleExtension);
    settings.setValue("vu_ambient_glow", ambientGlow);
    settings.setValue("vu_hot_spot_alpha", hotSpotAlpha);
    settings.setValue("vu_light_wash", lightWash);

    QString themeName;
    switch (theme) {
    case VUTheme::DarkStealth:
        themeName = "Dark Stealth";
        break;
    case VUTheme::WarmTube:
        themeName = "Warm Tube";
        break;
    case VUTheme::VintageAmber:
    default:
        themeName = "Vintage Amber";
        break;
    }
    settings.setValue("vu_theme", themeName);
}

void VUSettings::reset() {
    radiusScale = 1.20;
    pivotY = 1.55;
    needleExtension = 45.0;
    ambientGlow = 0.5;
    hotSpotAlpha = 0.5;
    lightWash = 0.2;
    theme = VUTheme::VintageAmber;
    save();
}
