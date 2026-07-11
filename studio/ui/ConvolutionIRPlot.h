#ifndef CONVOLUTION_IR_PLOT_H
#define CONVOLUTION_IR_PLOT_H

#include <QPainter>
#include <QWidget>
#include <string>
#include <vector>

class ConvolutionIRPlot : public QWidget {
    Q_OBJECT

public:
    explicit ConvolutionIRPlot(QWidget* parent = nullptr);

    void setIRPath(const std::string& path, const std::string& title = "");
    void setSamples(const std::vector<double>& samples, const std::string& title = "");

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::string m_path;
    std::string m_title;
    std::vector<double> m_samples;
    std::string m_errorMsg;

    void loadIR();
};

#endif // CONVOLUTION_IR_PLOT_H
