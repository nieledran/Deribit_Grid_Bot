#pragma once
#include <QPushButton>
#include <QTimer>
#include <QStylePainter>
#include <QStyleOptionButton>

class RebuildButton : public QPushButton {
    QTimer stripes;
    QPalette normal;
    int offset = 0;

public:
    explicit RebuildButton(const QString& label) : QPushButton(label), normal(palette())
    {
        stripes.setInterval(100);
        connect(&stripes, &QTimer::timeout, this, [this] {
            offset = (offset + 1) % 24;
            update();
        });
    }

    void setTransition(bool active)
    {
        if (active == stripes.isActive())
            return;
        if (active) {
            auto blue = normal;
            blue.setColor(QPalette::ButtonText, QColor("#1d4ed8"));
            blue.setColor(QPalette::Button, QColor("#dbeafe"));
            setPalette(blue);
            stripes.start();
        } else {
            stripes.stop();
            setPalette(normal);
        }
        setToolTip(active
                       ? "Old positions keep their takes. Each fully closed old cycle updates the "
                         "free grid at the current price. New fills keep the ordinary repeat rules."
                       : "");
        setProperty("transitionActive", active);
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        if (!stripes.isActive()) {
            QPushButton::paintEvent(event);
            return;
        }
        QStylePainter painter(this);
        QStyleOptionButton option;
        initStyleOption(&option);
        painter.drawControl(QStyle::CE_PushButtonBevel, option);
        painter.save();
        painter.setClipRect(rect().adjusted(3, 3, -3, -3));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(37, 99, 235, 18));
        for (int x = -height() - 24 + offset; x < width(); x += 24)
            painter.drawPolygon(QPolygon{QPoint(x, height()), QPoint(x + 10, height()),
                QPoint(x + height() + 10, 0), QPoint(x + height(), 0)});
        painter.restore();
        painter.drawControl(QStyle::CE_PushButtonLabel, option);
    }
};
