#ifndef VECTOR_SCOPE_VIEW_H
#define VECTOR_SCOPE_VIEW_H

#include "config/DSPConfigTypes.h"
#include "models/VectorScopeEngine.h"
#include <QWidget>
#include <QPainter>
#include <memory>

class VectorScopeView : public QWidget {
    Q_OBJECT

public:
    explicit VectorScopeView(QWidget* parent = nullptr);
    explicit VectorScopeView(std::shared_ptr<VectorScopeEngine> engine, QWidget* parent = nullptr);

    void setEngine(std::shared_ptr<VectorScopeEngine> engine);
    void setSamples(const AudioSamplesData& samples, bool showParticles = false, bool autoScale = true);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    std::shared_ptr<VectorScopeEngine> m_engine;
    AudioSamplesData m_samples;
    bool m_showParticles = false;
    bool m_autoScale = true;

    QImage m_persistenceBuffer;
    float m_phaseCorrSmoothed = 1.0f;
    float m_balanceSmoothed = 0.0f;
};

#endif // VECTOR_SCOPE_VIEW_H
