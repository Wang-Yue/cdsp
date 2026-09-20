#ifndef VECTOR_SCOPE_VIEW_H
#define VECTOR_SCOPE_VIEW_H

#include "config/DSPConfigTypes.h"    // for AudioSamplesData
#include "models/VectorScopeEngine.h" // for VectorScopeEngine

#include <QLine>   // for QLine
#include <QObject> // for Q_OBJECT
#include <QPoint>  // for QPoint
#include <QWidget> // for QWidget
#include <memory>  // for shared_ptr
#include <vector>  // for vector

class VectorScopeView : public QWidget {
    Q_OBJECT

public:
    explicit VectorScopeView(QWidget* parent = nullptr);
    explicit VectorScopeView(std::shared_ptr<VectorScopeEngine> engine, QWidget* parent = nullptr);
    ~VectorScopeView() override;

    void setEngine(std::shared_ptr<VectorScopeEngine> engine);
    void setSamples(const AudioSamplesData& samples, bool showParticles = false, bool autoScale = true,
                    int channelL = 0, int channelR = 1, float traceDecayRate = 0.85f);

protected:
    void paintEvent(QPaintEvent* event) override;

    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    std::shared_ptr<VectorScopeEngine> m_engine;
    AudioSamplesData m_samples;
    bool m_showParticles = false;
    bool m_autoScale = true;
    int m_channelL = 0;
    int m_channelR = 1;
    float m_traceDecayRate = 0.85f;

    float m_autoScaleFactor = 1.0f;
    std::vector<QLine> m_cachedLines;
    std::vector<QPoint> m_cachedPoints;
};

#endif // VECTOR_SCOPE_VIEW_H
