#pragma once
#include <deque>
#include <QElapsedTimer>

/**
 * @brief 简单的滑动窗口速率统计器
 * 用最近 windowMs 毫秒的 (t, bytesDone) 两点差分计算平均 B/s
 */
class SpeedAverager {
public:
    explicit SpeedAverager(int windowMs = 4000) : m_windowMs(windowMs) { m_timer.start(); }
    void reset() { m_points.clear(); m_timer.restart(); }

    // 每当累计完成字节数变化时调用（例如整体 done 字节）
    void onProgress(qint64 bytesDone) {
        const qint64 t = m_timer.elapsed();
        m_points.push_back({t, bytesDone});
        // 丢弃窗口之外的点
        while (!m_points.empty() && (t - m_points.front().t) > m_windowMs) m_points.pop_front();
    }

    // 返回近窗口的平均 B/s
    double avgBytesPerSec() const {
        if (m_points.size() < 2) return 0.0;
        const auto& a = m_points.front();
        const auto& b = m_points.back();
        const double dt = (b.t - a.t) / 1000.0;
        if (dt <= 0.01) return 0.0;
        return (double(b.bytes) - double(a.bytes)) / dt; // 注意括号
    }

private:
    struct Point { qint64 t; qint64 bytes; };
    int m_windowMs;
    QElapsedTimer m_timer;
    std::deque<Point> m_points;
};
