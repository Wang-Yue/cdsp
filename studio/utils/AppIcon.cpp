#include "utils/AppIcon.h"

#include <QBrush>    // for QLinearGradient, QBrush
#include <QColor>    // for QColor
#include <QPainter>  // for QPainter
#include <QPointF>   // for QPointF
#include <QRectF>    // for QRectF
#include <Qt>        // for GlobalColor, PenStyle
#include <QtGlobal>  // for qreal
#include <algorithm> // for max
#include <vector>    // for vector

namespace AppIcon {

QPixmap createIconPixmap(int size) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    qreal s = static_cast<qreal>(size);
    qreal margin = std::max(1.0, s * 0.07);
    qreal radius = s * 0.22;
    QRectF bgRect(margin, margin, s - 2.0 * margin, s - 2.0 * margin);

    // Background: Gradient from modern iOS/macOS blue to vibrant indigo
    QLinearGradient bgGrad(bgRect.topLeft(), bgRect.bottomRight());
    bgGrad.setColorAt(0.0, QColor(0, 122, 255)); // #007AFF
    bgGrad.setColorAt(1.0, QColor(88, 86, 214)); // #5856D6

    p.setPen(Qt::NoPen);
    p.setBrush(bgGrad);
    p.drawRoundedRect(bgRect, radius, radius);

    // Equalizer spectrum bars
    const int numBars = 4;
    const qreal heights[numBars] = {0.45, 0.90, 1.00, 0.65};

    qreal innerW = bgRect.width() * 0.64;
    qreal innerH = bgRect.height() * 0.56;
    qreal startX = bgRect.center().x() - innerW / 2.0;
    qreal centerY = bgRect.center().y();
    qreal slotW = innerW / static_cast<qreal>(numBars);
    qreal barW = std::max(1.5, slotW * 0.58);
    qreal barRadius = std::max(0.75, barW / 2.0);

    for (int i = 0; i < numBars; ++i) {
        qreal barH = std::max(2.0, innerH * heights[i]);
        qreal bx = startX + static_cast<qreal>(i) * slotW + (slotW - barW) / 2.0;
        qreal by = centerY - barH / 2.0;
        QRectF barRect(bx, by, barW, barH);

        // White equalizer bars with high contrast
        p.setBrush(QColor(255, 255, 255, 245));
        p.drawRoundedRect(barRect, barRadius, barRadius);
    }

    return pixmap;
}

QIcon getAppIcon() {
    QIcon icon;
    const std::vector<int> sizes = {16, 20, 24, 32, 48, 64, 128, 256};
    for (int sz : sizes) {
        icon.addPixmap(createIconPixmap(sz));
    }
    return icon;
}

} // namespace AppIcon
