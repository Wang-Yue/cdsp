#ifndef VIEWER_TRACKER_H
#define VIEWER_TRACKER_H

#include <QPointer>
#include <QWidget>
#include <algorithm>
#include <mutex>
#include <vector>

class ViewerTracker {
public:
    void registerViewer(QWidget* viewer) {
        if (!viewer)
            return;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& w : m_viewers) {
            if (w == viewer)
                return;
        }
        m_viewers.push_back(viewer);
    }

    void unregisterViewer(QWidget* viewer) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_viewers.erase(std::remove_if(m_viewers.begin(), m_viewers.end(),
                                       [viewer](const QPointer<QWidget>& w) { return !w || w == viewer; }),
                        m_viewers.end());
    }

    bool isVisible() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_viewers.begin(); it != m_viewers.end();) {
            if (!*it) {
                it = m_viewers.erase(it);
                continue;
            }
            QWidget* w = it->data();
            if (w->isVisible()) {
                QWidget* win = w->window();
                if (!win || !win->isMinimized()) {
                    return true;
                }
            }
            ++it;
        }
        return false;
    }

private:
    mutable std::mutex m_mutex;
    mutable std::vector<QPointer<QWidget>> m_viewers;
};

#endif // VIEWER_TRACKER_H
