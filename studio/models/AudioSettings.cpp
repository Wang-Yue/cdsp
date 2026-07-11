#include "models/AudioSettings.h"

AudioSettings::AudioSettings(QObject* parent) : QObject(parent) {
    loadPreferences();
}

float AudioSettings::getVolume(Fader fader) const {
    switch (fader) {
    case Fader::Main:
        return volume;
    case Fader::Aux1:
        return fader1Volume;
    case Fader::Aux2:
        return fader2Volume;
    case Fader::Aux3:
        return fader3Volume;
    case Fader::Aux4:
        return fader4Volume;
    }
    return volume;
}

void AudioSettings::setVolume(float db, Fader fader) {
    switch (fader) {
    case Fader::Main:
        volume = db;
        break;
    case Fader::Aux1:
        fader1Volume = db;
        break;
    case Fader::Aux2:
        fader2Volume = db;
        break;
    case Fader::Aux3:
        fader3Volume = db;
        break;
    case Fader::Aux4:
        fader4Volume = db;
        break;
    }
    savePreferences();
}

bool AudioSettings::getMuted(Fader fader) const {
    switch (fader) {
    case Fader::Main:
        return isMuted;
    case Fader::Aux1:
        return fader1Muted;
    case Fader::Aux2:
        return fader2Muted;
    case Fader::Aux3:
        return fader3Muted;
    case Fader::Aux4:
        return fader4Muted;
    }
    return isMuted;
}

void AudioSettings::setMuted(bool muted, Fader fader) {
    switch (fader) {
    case Fader::Main:
        isMuted = muted;
        break;
    case Fader::Aux1:
        fader1Muted = muted;
        break;
    case Fader::Aux2:
        fader2Muted = muted;
        break;
    case Fader::Aux3:
        fader3Muted = muted;
        break;
    case Fader::Aux4:
        fader4Muted = muted;
        break;
    }
    savePreferences();
}

void AudioSettings::notifyChange() {
    savePreferences();
    emit changed();
    emit settingsChanged();
    if (onChanged)
        onChanged();
}

void AudioSettings::loadPreferences() {
    QSettings s("DSPMonitor", "MonitorQt");
    chunkSize = s.value("chunksize", 1024).toInt();
    enableRateAdjust = s.value("enableRateAdjust", false).toBool();
    resamplerEnabled = s.value("resamplerEnabled", false).toBool();

    resamplerType = stringToResamplerType(s.value("resamplerType", "Synchronous").toString().toStdString());
    resamplerProfile = stringToResamplerProfile(s.value("resamplerProfile", "Balanced").toString().toStdString());
    resamplerUseProfile = s.value("resamplerUseProfile", true).toBool();
    resamplerAttenuation = s.value("resamplerAttenuation", 0.0).toDouble();
    resamplerSincLen = s.value("resamplerSincLen", 256).toInt();
    resamplerOversamplingFactor = s.value("resamplerOversamplingFactor", 128).toInt();
    resamplerWindow = s.value("resamplerWindow", "BlackmanHarris").toString().toStdString();
    resamplerFCutoff = s.value("resamplerFCutoff", 0.95).toDouble();
    resamplerInterpolation = static_cast<ResamplerInterpolation>(
        s.value("resamplerInterpolation", static_cast<int>(ResamplerInterpolation::Cubic)).toInt());
    resamplerSincInterpolation = static_cast<SincInterpolation>(
        s.value("resamplerSincInterpolation", static_cast<int>(SincInterpolation::Cubic)).toInt());
    resamplerAppleQuality = static_cast<AppleResamplerQuality>(
        s.value("resamplerAppleQuality", static_cast<int>(AppleResamplerQuality::Max)).toInt());
    resamplerAppleComplexity = static_cast<AppleResamplerComplexity>(
        s.value("resamplerAppleComplexity", static_cast<int>(AppleResamplerComplexity::Normal)).toInt());

    volume = s.value("volume", 0.0f).toFloat();
    isMuted = s.value("isMuted", false).toBool();

    fader1Volume = s.value("fader1Volume", 0.0f).toFloat();
    fader2Volume = s.value("fader2Volume", 0.0f).toFloat();
    fader3Volume = s.value("fader3Volume", 0.0f).toFloat();
    fader4Volume = s.value("fader4Volume", 0.0f).toFloat();

    fader1Muted = s.value("fader1Muted", false).toBool();
    fader2Muted = s.value("fader2Muted", false).toBool();
    fader3Muted = s.value("fader3Muted", false).toBool();
    fader4Muted = s.value("fader4Muted", false).toBool();

    silenceThreshold = s.value("silenceThreshold", -60).toInt();
    silenceTimeout = s.value("silenceTimeout", 0).toInt();
    queuelimit = s.value("queuelimit", 4).toInt();
    stopOnRateChange = s.value("stopOnRateChange", false).toBool();
    rateMeasureInterval = s.value("rateMeasureInterval", 1.0).toDouble();
    multithreaded = s.value("multithreaded", false).toBool();
    workerThreads = s.value("workerThreads", 0).toInt();
    darkMode = s.value("darkMode", false).toBool();
    autoStartEngine = s.value("autoStartEngine", false).toBool();
    logLevel = s.value("logLevel", 2).toInt();

    showLevelMetersInDashboard = s.value("show_levels_in_dashboard", true).toBool();
    showSpectrumInDashboard = s.value("show_spectrum_in_dashboard", true).toBool();
    showSpectrogramInDashboard = s.value("show_spectrogram_in_dashboard", true).toBool();
    showVectorScopeInDashboard = s.value("show_vectorscope_in_dashboard", true).toBool();
    showAnalogVUInDashboard = s.value("show_analog_vu_in_dashboard", true).toBool();
}

void AudioSettings::savePreferences() {
    QSettings s("DSPMonitor", "MonitorQt");
    s.setValue("chunksize", chunkSize);
    s.setValue("enableRateAdjust", enableRateAdjust);
    s.setValue("resamplerEnabled", resamplerEnabled);
    s.setValue("resamplerType", QString::fromStdString(resamplerTypeToString(resamplerType)));
    s.setValue("resamplerProfile", QString::fromStdString(resamplerProfileToString(resamplerProfile)));
    s.setValue("resamplerUseProfile", resamplerUseProfile);
    s.setValue("resamplerAttenuation", resamplerAttenuation);
    s.setValue("resamplerSincLen", resamplerSincLen);
    s.setValue("resamplerOversamplingFactor", resamplerOversamplingFactor);
    s.setValue("resamplerWindow", QString::fromStdString(resamplerWindow));
    s.setValue("resamplerFCutoff", resamplerFCutoff);
    s.setValue("resamplerInterpolation", static_cast<int>(resamplerInterpolation));
    s.setValue("resamplerSincInterpolation", static_cast<int>(resamplerSincInterpolation));
    s.setValue("resamplerAppleQuality", static_cast<int>(resamplerAppleQuality));
    s.setValue("resamplerAppleComplexity", static_cast<int>(resamplerAppleComplexity));

    s.setValue("volume", volume);
    s.setValue("isMuted", isMuted);
    s.setValue("fader1Volume", fader1Volume);
    s.setValue("fader2Volume", fader2Volume);
    s.setValue("fader3Volume", fader3Volume);
    s.setValue("fader4Volume", fader4Volume);

    s.setValue("fader1Muted", fader1Muted);
    s.setValue("fader2Muted", fader2Muted);
    s.setValue("fader3Muted", fader3Muted);
    s.setValue("fader4Muted", fader4Muted);

    s.setValue("silenceThreshold", silenceThreshold);
    s.setValue("silenceTimeout", silenceTimeout);
    s.setValue("queuelimit", queuelimit);
    s.setValue("stopOnRateChange", stopOnRateChange);
    s.setValue("rateMeasureInterval", rateMeasureInterval);
    s.setValue("multithreaded", multithreaded);
    s.setValue("workerThreads", workerThreads);
    s.setValue("darkMode", darkMode);
    s.setValue("autoStartEngine", autoStartEngine);
    s.setValue("logLevel", logLevel);

    s.setValue("show_levels_in_dashboard", showLevelMetersInDashboard);
    s.setValue("show_spectrum_in_dashboard", showSpectrumInDashboard);
    s.setValue("show_spectrogram_in_dashboard", showSpectrogramInDashboard);
    s.setValue("show_vectorscope_in_dashboard", showVectorScopeInDashboard);
    s.setValue("show_analog_vu_in_dashboard", showAnalogVUInDashboard);
    emit settingsChanged();
}
