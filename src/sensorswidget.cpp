/*
 * Tux Manager - Linux system monitor
 * Copyright (C) 2026 Petr Bena <petr@bena.rocks>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "sensorswidget.h"
#include "ui_sensorswidget.h"

#include "colorscheme.h"
#include "configuration.h"
#include "ui/uihelper.h"

#include <QApplication>
#include <QClipboard>
#include <QHash>
#include <QHeaderView>
#include <QLinearGradient>
#include <QMenu>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QRadialGradient>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTreeWidgetItem>

namespace
{
    enum NodeKind
    {
        NodeRoot       = 0,
        NodeDevice     = 1,
        NodeCategory   = 2,
        NodeSensorLeaf = 3
    };

    constexpr int kNodeKindRole = Qt::UserRole;
    constexpr int kStableIdRole = Qt::UserRole + 1;

    bool isDarkPalette(const QWidget *widget)
    {
        if (widget)
            return widget->palette().color(QPalette::Base).lightness() < 128;
        if (const ColorScheme *scheme = ColorScheme::GetCurrent())
            return scheme->DarkMode;
        return ColorScheme::DetectDarkMode();
    }

    QColor baseBackgroundColor(const QWidget *widget)
    {
        if (widget)
            return widget->palette().color(QPalette::Base);
        return isDarkPalette(widget) ? QColor(30, 32, 36) : QColor(255, 255, 255);
    }

    QColor leafStripeColor(const QWidget *widget)
    {
        const QColor base = baseBackgroundColor(widget);
        if (base.lightness() < 128)
        {
            return QColor(qMin(255, base.red() + 14),
                          qMin(255, base.green() + 14),
                          qMin(255, base.blue() + 16));
        }
        return QColor(qMax(0, base.red() - 15),
                      qMax(0, base.green() - 15),
                      qMax(0, base.blue() - 15));
    }

    bool isStripedLeafIndex(const QModelIndex &index)
    {
        if (!index.isValid())
            return false;
        const QModelIndex col0 = index.sibling(index.row(), 0);
        const int kind = col0.data(kNodeKindRole).toInt();
        return kind == NodeSensorLeaf && (index.row() % 2 == 0);
    }

    template <typename PaintFn>
    QIcon makeMultiResIcon(PaintFn paintFn)
    {
        QIcon icon;
        for (int px : { 16, 32 })
        {
            QPixmap pix(px, px);
            pix.fill(Qt::transparent);
            QPainter p(&pix);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.scale(px / 16.0, px / 16.0);
            paintFn(p);
            p.end();
            icon.addPixmap(pix);
        }
        return icon;
    }

    class SensorsItemDelegate : public QStyledItemDelegate
    {
        public:
            explicit SensorsItemDelegate(QTreeWidget *tree)
                : QStyledItemDelegate(tree)
                , m_tree(tree)
            {
            }

            QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
            {
                QSize s = QStyledItemDelegate::sizeHint(option, index);
                int h = qMax(20, option.fontMetrics.height() + 4);
                if (h & 1)
                    ++h;
                s.setHeight(h);
                return s;
            }

            void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
            {
                QStyleOptionViewItem opt(option);
                this->initStyleOption(&opt, index);

                const bool selected = (opt.state & QStyle::State_Selected);
                if (!selected)
                {
                    const QColor bg = isStripedLeafIndex(index)
                                      ? leafStripeColor(this->m_tree)
                                      : baseBackgroundColor(this->m_tree);
                    painter->fillRect(opt.rect, bg);
                    opt.backgroundBrush = Qt::NoBrush;
                }

                opt.state &= ~QStyle::State_HasFocus;
                QStyledItemDelegate::paint(painter, opt, index);
            }

        private:
            QTreeWidget *m_tree { nullptr };
    };
} // namespace

// ============================================================================
// SensorsTreeWidget
// ============================================================================

SensorsTreeWidget::SensorsTreeWidget(QWidget *parent)
    : QTreeWidget(parent)
{
    this->setIndentation(19);
    this->setIconSize(QSize(16, 16));
    this->setUniformRowHeights(true);
    this->setAlternatingRowColors(false);
    this->setRootIsDecorated(true);
    this->setItemsExpandable(true);
    this->setExpandsOnDoubleClick(true);
    this->setAllColumnsShowFocus(true);
    this->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->setSelectionMode(QAbstractItemView::SingleSelection);
    this->setFrameShape(QFrame::StyledPanel);
    this->setItemDelegate(new SensorsItemDelegate(this));
}

void SensorsTreeWidget::drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const QColor baseBg = baseBackgroundColor(this);
    painter->fillRect(option.rect, baseBg);

    const bool selected = this->selectionModel()
                          && this->selectionModel()->isRowSelected(index.row(), index.parent());
    if (!selected && isStripedLeafIndex(index))
    {
        const QRect col0Rect = this->visualRect(index.sibling(index.row(), 0));
        if (col0Rect.isValid() && col0Rect.left() <= option.rect.right())
        {
            const QRect stripeRect(col0Rect.left(),
                                   option.rect.top(),
                                   option.rect.right() - col0Rect.left() + 1,
                                   option.rect.height());
            painter->fillRect(stripeRect, leafStripeColor(this));
        }
    }

    QTreeWidget::drawRow(painter, option, index);
}

void SensorsTreeWidget::drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const
{
    if (!index.isValid() || rect.isEmpty())
        return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);

    const QColor baseBg = baseBackgroundColor(this);
    painter->fillRect(rect, baseBg);

    const bool dark = isDarkPalette(this);
    const QColor dotColor = dark ? QColor(115, 120, 130) : QColor(150, 150, 150);
    const QColor boxBorderColor = dark ? QColor(140, 145, 155) : QColor(128, 128, 128);
    const QColor boxFillColor = dark ? QColor(45, 48, 54) : QColor(255, 255, 255);
    const QColor boxSignColor = dark ? QColor(225, 228, 235) : QColor(35, 35, 35);

    QVector<QModelIndex> chain;
    for (QModelIndex cur = index; cur.isValid(); cur = cur.parent())
        chain.prepend(cur);

    const int indent = this->indentation();
    const int topY = rect.top();
    const int botY = rect.bottom();
    const int midY = rect.top() + rect.height() / 2;

    auto drawVertDots = [&](int x, int y1, int y2)
    {
        if (y2 < y1)
            std::swap(y1, y2);
        painter->setPen(dotColor);
        for (int y = y1; y <= y2; ++y)
        {
            if (((y - topY) & 1) == 0)
                painter->drawPoint(x, y);
        }
    };

    auto drawHorizDots = [&](int x1, int x2, int y)
    {
        if (x2 < x1)
            std::swap(x1, x2);
        painter->setPen(dotColor);
        for (int x = x1; x <= x2; ++x)
        {
            if (((x - rect.left()) & 1) == 0)
                painter->drawPoint(x, y);
        }
    };

    for (int lvl = 0; lvl < chain.size(); ++lvl)
    {
        const QModelIndex &nodeIdx = chain.at(lvl);
        const int cellLeft = rect.left() + lvl * indent;
        const int cellMidX = cellLeft + indent / 2;
        const int cellRight = cellLeft + indent - 1;
        const bool hasNextSibling = nodeIdx.sibling(nodeIdx.row() + 1, 0).isValid();

        if (lvl < chain.size() - 1)
        {
            if (hasNextSibling)
                drawVertDots(cellMidX, topY, botY);
        } else
        {
            const bool hasLineUp = nodeIdx.parent().isValid() || nodeIdx.row() > 0;
            if (hasLineUp)
                drawVertDots(cellMidX, topY, midY);
            if (hasNextSibling)
                drawVertDots(cellMidX, midY, botY);

            drawHorizDots(cellMidX, cellRight, midY);

            if (this->model() && this->model()->hasChildren(nodeIdx))
            {
                const QRect boxRect(cellMidX - 4, midY - 4, 8, 8);
                painter->fillRect(boxRect, boxFillColor);
                painter->setPen(boxBorderColor);
                painter->drawRect(boxRect);

                painter->setPen(boxSignColor);
                painter->drawLine(cellMidX - 2, midY, cellMidX + 2, midY);
                if (!this->isExpanded(nodeIdx))
                    painter->drawLine(cellMidX, midY - 2, cellMidX, midY + 2);
            }
        }
    }

    painter->restore();
}

void SensorsTreeWidget::mousePressEvent(QMouseEvent *event)
{
    if (event && event->button() == Qt::LeftButton)
    {
        const QModelIndex idx = this->indexAt(event->pos());
        if (idx.isValid() && this->model() && this->model()->hasChildren(idx))
        {
            const QModelIndex col0 = idx.sibling(idx.row(), 0);
            const QRect itemRect = this->visualRect(col0);
            const int indent = this->indentation();
            const QRect branchCellRect(itemRect.left() - indent, itemRect.top(), indent, itemRect.height());
            if (branchCellRect.contains(event->pos()))
            {
                this->setExpanded(col0, !this->isExpanded(col0));
                event->accept();
                return;
            }
        }
    }
    QTreeWidget::mousePressEvent(event);
}

// ============================================================================
// Procedural HWMonitor Icons
// ============================================================================

QIcon SensorsWidget::SystemIcon()
{
    static QIcon cached;
    if (!cached.isNull())
        return cached;

    cached = makeMultiResIcon([](QPainter &p)
    {
        // Monitor stand & base
        p.setPen(QPen(QColor(70, 75, 82), 1.0));
        p.setBrush(QColor(150, 156, 164));
        p.drawRect(QRectF(6.0, 10.5, 4.0, 2.5));
        p.drawRoundedRect(QRectF(3.5, 12.5, 9.0, 2.0), 0.8, 0.8);

        // Monitor outer bezel
        p.setPen(QPen(QColor(55, 60, 68), 1.0));
        p.setBrush(QColor(195, 202, 210));
        p.drawRoundedRect(QRectF(1.5, 1.5, 13.0, 9.5), 1.2, 1.2);

        // Screen display
        QLinearGradient screenGrad(3.0, 3.0, 13.0, 9.5);
        screenGrad.setColorAt(0.0, QColor(42, 130, 170));
        screenGrad.setColorAt(1.0, QColor(18, 68, 96));
        p.setPen(QPen(QColor(25, 45, 60), 0.8));
        p.setBrush(screenGrad);
        p.drawRect(QRectF(2.8, 2.8, 10.4, 6.6));

        // Screen glint
        p.setPen(QPen(QColor(135, 210, 240, 180), 1.0));
        p.drawLine(QPointF(3.8, 3.8), QPointF(7.5, 3.8));

        // Green power LED
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(60, 220, 90));
        p.drawEllipse(QRectF(12.2, 9.7, 1.2, 1.2));
    });
    return cached;
}

QIcon SensorsWidget::DeviceIcon(SensorDeviceType type)
{
    static QHash<int, QIcon> cache;
    const int key = static_cast<int>(type);
    auto it = cache.find(key);
    if (it != cache.end())
        return it.value();

    QIcon icon;
    switch (type)
    {
        case SensorDeviceType::Motherboard:
            icon = makeMultiResIcon([](QPainter &p)
            {
                // Green PCB board
                p.setPen(QPen(QColor(16, 78, 28), 1.0));
                p.setBrush(QColor(34, 139, 54));
                p.drawRoundedRect(QRectF(1.5, 1.5, 13.0, 13.0), 1.0, 1.0);

                // Gold PCB traces
                p.setPen(QPen(QColor(235, 205, 75), 1.0));
                p.drawLine(QPointF(3.0, 10.5), QPointF(13.0, 10.5));
                p.drawLine(QPointF(3.0, 12.5), QPointF(11.0, 12.5));

                // Chipset IC square
                p.setPen(QPen(QColor(220, 225, 230), 0.8));
                p.setBrush(QColor(42, 46, 52));
                p.drawRect(QRectF(3.0, 3.0, 5.2, 5.2));

                // White PCI / DIMM slots on right
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(240, 244, 248));
                p.drawRect(QRectF(10.0, 2.8, 1.5, 6.2));
                p.drawRect(QRectF(12.2, 2.8, 1.3, 6.2));
            });
            break;

        case SensorDeviceType::Cpu:
            icon = makeMultiResIcon([](QPainter &p)
            {
                // Gold PGA substrate border
                p.setPen(QPen(QColor(110, 82, 16), 1.0));
                p.setBrush(QColor(218, 178, 52));
                p.drawRoundedRect(QRectF(1.5, 1.5, 13.0, 13.0), 1.0, 1.0);

                // Green interposer
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(46, 128, 62));
                p.drawRect(QRectF(3.0, 3.0, 10.0, 10.0));

                // Gold contact pins around edge
                p.setBrush(QColor(255, 230, 110));
                for (int i = 3; i <= 12; i += 2)
                {
                    p.drawRect(QRectF(i, 1.8, 1.0, 1.0));
                    p.drawRect(QRectF(i, 13.2, 1.0, 1.0));
                    p.drawRect(QRectF(1.8, i, 1.0, 1.0));
                    p.drawRect(QRectF(13.2, i, 1.0, 1.0));
                }

                // Metallic heatspreader / die center
                QLinearGradient ihsGrad(4.5, 4.5, 11.5, 11.5);
                ihsGrad.setColorAt(0.0, QColor(140, 148, 160));
                ihsGrad.setColorAt(1.0, QColor(72, 78, 88));
                p.setPen(QPen(QColor(45, 50, 58), 0.9));
                p.setBrush(ihsGrad);
                p.drawRoundedRect(QRectF(4.5, 4.5, 7.0, 7.0), 0.8, 0.8);
            });
            break;

        case SensorDeviceType::Gpu:
            icon = makeMultiResIcon([](QPainter &p)
            {
                // PCIe bracket on left
                p.setPen(QPen(QColor(90, 96, 104), 1.0));
                p.setBrush(QColor(180, 186, 194));
                p.drawLine(QPointF(2.0, 2.0), QPointF(2.0, 14.0));
                p.drawLine(QPointF(1.0, 2.0), QPointF(3.0, 2.0));

                // GPU card PCB
                p.setPen(QPen(QColor(18, 72, 54), 1.0));
                p.setBrush(QColor(30, 118, 88));
                p.drawRoundedRect(QRectF(2.5, 3.0, 12.0, 9.0), 1.0, 1.0);

                // Gold PCIe connector fingers
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(230, 185, 45));
                p.drawRect(QRectF(5.0, 12.0, 7.0, 1.8));

                // Cooling fan shroud
                p.setPen(QPen(QColor(40, 44, 50), 0.9));
                p.setBrush(QColor(52, 58, 66));
                p.drawEllipse(QRectF(6.0, 4.2, 6.6, 6.6));

                // Fan blades
                p.setPen(QPen(QColor(90, 200, 250), 1.2));
                p.drawLine(QPointF(9.3, 5.2), QPointF(9.3, 9.8));
                p.drawLine(QPointF(7.0, 7.5), QPointF(11.6, 7.5));
            });
            break;

        case SensorDeviceType::Disk:
            icon = makeMultiResIcon([](QPainter &p)
            {
                // 3D slanted silver top cover (like HWMonitor drive icon)
                QPolygonF topPoly;
                topPoly << QPointF(3.2, 3.5)
                        << QPointF(12.8, 3.5)
                        << QPointF(14.8, 8.8)
                        << QPointF(1.2, 8.8);
                QLinearGradient topGrad(1.2, 3.5, 14.8, 8.8);
                topGrad.setColorAt(0.0, QColor(238, 242, 246));
                topGrad.setColorAt(1.0, QColor(176, 184, 194));
                p.setPen(QPen(QColor(82, 88, 96), 1.0));
                p.setBrush(topGrad);
                p.drawPolygon(topPoly);

                // Front dark bezel
                p.setBrush(QColor(62, 68, 76));
                p.drawRect(QRectF(1.2, 8.8, 13.6, 3.8));

                // Activity LED & detail slot
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(80, 225, 100));
                p.drawEllipse(QRectF(12.0, 10.0, 1.6, 1.6));

                p.setBrush(QColor(170, 178, 188));
                p.drawRect(QRectF(2.8, 10.2, 6.5, 1.1));
            });
            break;

        case SensorDeviceType::Battery:
            icon = makeMultiResIcon([](QPainter &p)
            {
                // Positive terminal nub
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(145, 152, 162));
                p.drawRect(QRectF(13.2, 6.0, 1.6, 4.0));

                // Battery body
                p.setPen(QPen(QColor(75, 82, 92), 1.0));
                p.setBrush(QColor(42, 46, 54));
                p.drawRoundedRect(QRectF(1.5, 3.8, 11.8, 8.4), 1.2, 1.2);

                // Green charge bars
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(46, 204, 113));
                p.drawRect(QRectF(3.2, 5.4, 2.2, 5.2));
                p.drawRect(QRectF(6.3, 5.4, 2.2, 5.2));
                p.drawRect(QRectF(9.4, 5.4, 2.2, 5.2));
            });
            break;

        case SensorDeviceType::Network:
        case SensorDeviceType::Other:
        default:
            icon = makeMultiResIcon([](QPainter &p)
            {
                p.setPen(QPen(QColor(60, 80, 105), 1.0));
                p.setBrush(QColor(90, 145, 205));
                p.drawRoundedRect(QRectF(2.0, 2.0, 12.0, 12.0), 2.0, 2.0);
                p.setPen(QPen(Qt::white, 1.2));
                p.drawLine(QPointF(5.0, 8.0), QPointF(11.0, 8.0));
                p.drawLine(QPointF(8.0, 5.0), QPointF(8.0, 11.0));
            });
            break;
    }

    cache.insert(key, icon);
    return icon;
}

QIcon SensorsWidget::CategoryIcon(SensorType type)
{
    static QHash<int, QIcon> cache;
    const int key = static_cast<int>(type);
    auto it = cache.find(key);
    if (it != cache.end())
        return it.value();

    QIcon icon;
    switch (type)
    {
        case SensorType::Voltage:
            icon = makeMultiResIcon([](QPainter &p)
            {
                // Yellow jagged lightning bolt (matching HWMonitor Voltages icon)
                QPolygonF bolt;
                bolt << QPointF(10.5, 1.5)
                     << QPointF(4.0, 8.2)
                     << QPointF(8.0, 8.2)
                     << QPointF(5.0, 14.5)
                     << QPointF(12.2, 7.2)
                     << QPointF(8.2, 7.2);
                QLinearGradient grad(4.0, 1.5, 12.0, 14.5);
                grad.setColorAt(0.0, QColor(255, 235, 59));
                grad.setColorAt(1.0, QColor(255, 160, 0));
                p.setPen(QPen(QColor(125, 82, 0), 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(grad);
                p.drawPolygon(bolt);
            });
            break;

        case SensorType::Temperature:
            icon = makeMultiResIcon([](QPainter &p)
            {
                // Diagonal red thermometer (matching HWMonitor Temperatures icon)
                p.save();
                p.translate(8.0, 8.0);
                p.rotate(35.0);

                // Outer glass tube
                p.setPen(QPen(QColor(95, 100, 108), 1.0));
                p.setBrush(QColor(245, 247, 250));
                p.drawRoundedRect(QRectF(-1.8, -6.5, 3.6, 9.5), 1.8, 1.8);

                // Graduation ticks
                p.setPen(QPen(QColor(110, 115, 122), 0.9));
                p.drawLine(QPointF(-3.2, -4.5), QPointF(-1.8, -4.5));
                p.drawLine(QPointF(-3.2, -2.5), QPointF(-1.8, -2.5));
                p.drawLine(QPointF(-3.2, -0.5), QPointF(-1.8, -0.5));

                // Red mercury column & bulb
                p.setPen(QPen(QColor(145, 20, 20), 0.9));
                p.setBrush(QColor(229, 57, 53));
                p.drawRect(QRectF(-1.0, -2.5, 2.0, 6.0));
                p.drawEllipse(QPointF(0.0, 4.2), 3.0, 3.0);

                // Specular highlight on bulb
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 205, 210));
                p.drawEllipse(QPointF(-1.0, 3.3), 0.9, 0.9);
                p.restore();
            });
            break;

        case SensorType::Power:
            icon = makeMultiResIcon([](QPainter &p)
            {
                // Glossy red/orange circle with yellow lightning bolt (matching HWMonitor Powers icon)
                QRadialGradient radGrad(6.0, 5.0, 8.5);
                radGrad.setColorAt(0.0, QColor(255, 112, 67));
                radGrad.setColorAt(0.7, QColor(229, 57, 53));
                radGrad.setColorAt(1.0, QColor(175, 24, 24));
                p.setPen(QPen(QColor(130, 16, 16), 1.0));
                p.setBrush(radGrad);
                p.drawEllipse(QRectF(1.5, 1.5, 13.0, 13.0));

                QPolygonF bolt;
                bolt << QPointF(9.5, 3.2)
                     << QPointF(5.2, 8.3)
                     << QPointF(8.0, 8.3)
                     << QPointF(6.5, 12.8)
                     << QPointF(11.0, 7.5)
                     << QPointF(8.2, 7.5);
                p.setPen(QPen(QColor(255, 245, 157), 0.5));
                p.setBrush(QColor(255, 238, 88));
                p.drawPolygon(bolt);
            });
            break;

        case SensorType::Utilization:
            icon = makeMultiResIcon([](QPainter &p)
            {
                // Dark square monitor with green waveform graph (matching HWMonitor Utilization icon)
                p.setPen(QPen(QColor(90, 98, 108), 1.0));
                p.setBrush(QColor(14, 30, 18));
                p.drawRoundedRect(QRectF(1.5, 2.0, 13.0, 12.0), 1.0, 1.0);

                // Subtle grid lines
                p.setPen(QPen(QColor(28, 72, 38), 0.7));
                p.drawLine(QPointF(2.5, 6.0), QPointF(13.5, 6.0));
                p.drawLine(QPointF(2.5, 10.0), QPointF(13.5, 10.0));
                p.drawLine(QPointF(6.0, 3.0), QPointF(6.0, 13.0));
                p.drawLine(QPointF(10.0, 3.0), QPointF(10.0, 13.0));

                // Green waveform trace
                QPainterPath wave;
                wave.moveTo(2.5, 11.2);
                wave.lineTo(4.8, 9.5);
                wave.lineTo(6.5, 10.5);
                wave.lineTo(8.5, 4.5);
                wave.lineTo(10.5, 8.8);
                wave.lineTo(12.0, 6.2);
                wave.lineTo(13.5, 7.5);
                p.setPen(QPen(QColor(0, 230, 118), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(wave);
            });
            break;

        case SensorType::Clock:
            icon = makeMultiResIcon([](QPainter &p)
            {
                // Silver square with blue/cyan digital square-wave pulse (matching HWMonitor Clocks icon)
                QLinearGradient bgGrad(1.5, 2.0, 14.5, 14.0);
                bgGrad.setColorAt(0.0, QColor(246, 248, 250));
                bgGrad.setColorAt(1.0, QColor(206, 214, 224));
                p.setPen(QPen(QColor(122, 130, 140), 1.0));
                p.setBrush(bgGrad);
                p.drawRoundedRect(QRectF(1.5, 2.0, 13.0, 12.0), 1.0, 1.0);

                // Square wave pulse _|‾|_
                QPainterPath pulse;
                pulse.moveTo(3.0, 10.8);
                pulse.lineTo(5.5, 10.8);
                pulse.lineTo(5.5, 5.2);
                pulse.lineTo(9.5, 5.2);
                pulse.lineTo(9.5, 10.8);
                pulse.lineTo(13.0, 10.8);
                p.setPen(QPen(QColor(2, 119, 189), 1.5, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(pulse);
            });
            break;

        case SensorType::Fan:
        case SensorType::Pwm:
            icon = makeMultiResIcon([](QPainter &p)
            {
                // Cooling fan housing & 4 blades
                p.setPen(QPen(QColor(82, 90, 100), 1.0));
                p.setBrush(QColor(42, 46, 54));
                p.drawRoundedRect(QRectF(1.5, 1.5, 13.0, 13.0), 2.0, 2.0);

                p.save();
                p.translate(8.0, 8.0);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(41, 182, 246));
                for (int i = 0; i < 4; ++i)
                {
                    p.drawEllipse(QRectF(-1.6, -5.2, 3.2, 4.2));
                    p.rotate(90.0);
                }
                p.setBrush(QColor(215, 222, 230));
                p.drawEllipse(QPointF(0.0, 0.0), 1.8, 1.8);
                p.restore();
            });
            break;

        case SensorType::Current:
        case SensorType::Other:
        default:
            icon = makeMultiResIcon([](QPainter &p)
            {
                p.setPen(QPen(QColor(1, 87, 155), 1.0));
                p.setBrush(QColor(2, 136, 209));
                p.drawEllipse(QRectF(1.5, 1.5, 13.0, 13.0));

                QPainterPath path;
                path.moveTo(5.0, 11.5);
                path.lineTo(8.0, 4.2);
                path.lineTo(11.0, 11.5);
                path.moveTo(6.0, 9.2);
                path.lineTo(10.0, 9.2);
                p.setPen(QPen(Qt::white, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
            });
            break;
    }

    cache.insert(key, icon);
    return icon;
}

// ============================================================================
// SensorsWidget
// ============================================================================

SensorsWidget::SensorsWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::SensorsWidget)
    , m_refreshTimer(new QTimer(this))
{
    this->ui->setupUi(this);

    this->ui->treeWidget->setColumnCount(4);
    this->ui->treeWidget->setHeaderLabels({
        tr("Sensor"),
        tr("Value"),
        tr("Min"),
        tr("Max")
    });
    this->ui->treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);

    if (QTreeWidgetItem *headerItem = this->ui->treeWidget->headerItem())
    {
        for (int col = 0; col < 4; ++col)
            headerItem->setTextAlignment(col, Qt::AlignLeft | Qt::AlignVCenter);
    }

    QHeaderView *hv = this->ui->treeWidget->header();
    hv->setSectionsClickable(false);
    hv->setStretchLastSection(false);
    hv->setMinimumSectionSize(70);
    for (int col = 0; col < 4; ++col)
        hv->setSectionResizeMode(col, QHeaderView::Interactive);

    hv->resizeSection(0, 360);
    hv->resizeSection(1, 130);
    hv->resizeSection(2, 130);
    hv->resizeSection(3, 130);

    connect(this->m_refreshTimer, &QTimer::timeout, this, &SensorsWidget::onTimerTick);
    connect(this->ui->treeWidget, &QTreeWidget::customContextMenuRequested, this, &SensorsWidget::onContextMenu);

    // Populate initial snapshot right away so the widget is never empty
    this->Refresh();
    this->ui->treeWidget->clearSelection();
    this->ui->treeWidget->setCurrentItem(nullptr);
}

SensorsWidget::~SensorsWidget()
{
    delete this->ui;
}

void SensorsWidget::SetActive(bool active)
{
    if (this->m_active == active)
        return;

    this->m_active = active;
    if (active)
    {
        if (!CFG->RefreshPaused)
            this->Refresh();
        this->m_refreshTimer->start(CFG->RefreshRateMs);
    } else
    {
        this->m_refreshTimer->stop();
    }
}

void SensorsWidget::Refresh()
{
    this->m_sensors.Sample();
    this->rebuildOrUpdateTree();
}

void SensorsWidget::ResetMinMax()
{
    this->m_sensors.ResetMinMax();
    this->Refresh();
}

void SensorsWidget::ExpandAll()
{
    this->ui->treeWidget->expandAll();
}

void SensorsWidget::CollapseAll()
{
    this->ui->treeWidget->collapseAll();
}

QTreeWidget *SensorsWidget::TreeWidget() const
{
    return this->ui ? this->ui->treeWidget : nullptr;
}

void SensorsWidget::onTimerTick()
{
    if (CFG->RefreshPaused)
        return;
    this->Refresh();
}

void SensorsWidget::onContextMenu(const QPoint &pos)
{
    QMenu menu(this);

    const QTreeWidgetItem *clickedItem = this->ui->treeWidget->itemAt(pos);
    const bool isLeaf = clickedItem && (clickedItem->data(0, kNodeKindRole).toInt() == NodeSensorLeaf);

    if (clickedItem)
    {
        if (isLeaf)
        {
            const QString valText = clickedItem->text(1);
            QAction *copyValAction = menu.addAction(tr("Copy Value"));
            copyValAction->setEnabled(!valText.isEmpty());
            connect(copyValAction, &QAction::triggered, this, [valText]()
            {
                QApplication::clipboard()->setText(valText);
            });
        }

        QAction *copyRowAction = menu.addAction(tr("Copy Row"));
        connect(copyRowAction, &QAction::triggered, this, [clickedItem, isLeaf]()
        {
            if (isLeaf)
            {
                const QString rowText = QString("%1\t%2\t%3\t%4")
                                            .arg(clickedItem->text(0),
                                                 clickedItem->text(1),
                                                 clickedItem->text(2),
                                                 clickedItem->text(3));
                QApplication::clipboard()->setText(rowText);
            } else
            {
                QApplication::clipboard()->setText(clickedItem->text(0));
            }
        });

        menu.addSeparator();
    }

    QAction *resetMinMaxAction = menu.addAction(tr("Reset Min/Max"));
    connect(resetMinMaxAction, &QAction::triggered, this, &SensorsWidget::ResetMinMax);

    menu.addSeparator();

    QAction *expandAllAction = menu.addAction(tr("Expand All"));
    connect(expandAllAction, &QAction::triggered, this, &SensorsWidget::ExpandAll);

    QAction *collapseAllAction = menu.addAction(tr("Collapse All"));
    connect(collapseAllAction, &QAction::triggered, this, &SensorsWidget::CollapseAll);

    menu.addSeparator();

    UIHelper::AddRefreshIntervalContextMenu(&menu, this->m_refreshTimer, this->m_active);
    UIHelper::AddGlobalContextMenuItems(&menu, this);

    menu.exec(this->ui->treeWidget->viewport()->mapToGlobal(pos));
}

void SensorsWidget::rebuildOrUpdateTree()
{
    QSignalBlocker blocker(this->ui->treeWidget);
    QScrollBar *vScroll = this->ui->treeWidget->verticalScrollBar();
    QScrollBar *hScroll = this->ui->treeWidget->horizontalScrollBar();
    const int savedVPos = vScroll ? vScroll->value() : 0;
    const int savedHPos = hScroll ? hScroll->value() : 0;

    // 1. Root system node (machine hostname)
    QTreeWidgetItem *rootItem = nullptr;
    if (this->ui->treeWidget->topLevelItemCount() > 0)
    {
        rootItem = this->ui->treeWidget->topLevelItem(0);
    } else
    {
        rootItem = new QTreeWidgetItem();
        rootItem->setData(0, kNodeKindRole, NodeRoot);
        rootItem->setData(0, kStableIdRole, QStringLiteral("root"));
        rootItem->setIcon(0, SystemIcon());
        this->ui->treeWidget->addTopLevelItem(rootItem);
        rootItem->setExpanded(true);
    }

    const QString hostLabel = this->m_sensors.HostName().isEmpty()
                              ? QStringLiteral("System")
                              : this->m_sensors.HostName();
    if (rootItem->text(0) != hostLabel)
        rootItem->setText(0, hostLabel);

    // Build lookup of existing device items under rootItem
    QHash<QString, QTreeWidgetItem *> existingDevices;
    for (int i = 0; i < rootItem->childCount(); ++i)
    {
        if (QTreeWidgetItem *devItem = rootItem->child(i))
            existingDevices.insert(devItem->data(0, kStableIdRole).toString(), devItem);
    }

    int totalSensors = 0;
    int targetDevIdx = 0;
    const QVector<SensorDevice> &devices = this->m_sensors.Devices();

    for (const SensorDevice &dev : devices)
    {
        if (dev.Categories().isEmpty())
            continue;

        bool hasAnySensor = false;
        for (const SensorCategory &cat : dev.Categories())
        {
            if (!cat.Items().isEmpty())
            {
                hasAnySensor = true;
                break;
            }
        }
        if (!hasAnySensor)
            continue;

        const QString devKey = dev.Id().isEmpty() ? dev.Name() : dev.Id();
        QTreeWidgetItem *devItem = existingDevices.take(devKey);
        const bool isNewDev = (devItem == nullptr);
        if (isNewDev)
        {
            devItem = new QTreeWidgetItem();
            devItem->setData(0, kNodeKindRole, NodeDevice);
            devItem->setData(0, kStableIdRole, devKey);
            devItem->setIcon(0, DeviceIcon(dev.Type()));
        }

        if (devItem->text(0) != dev.Name())
            devItem->setText(0, dev.Name());

        if (rootItem->indexOfChild(devItem) < 0)
        {
            rootItem->insertChild(targetDevIdx, devItem);
            devItem->setExpanded(true);
        } else if (rootItem->indexOfChild(devItem) != targetDevIdx)
        {
            const bool wasExpanded = devItem->isExpanded();
            rootItem->takeChild(rootItem->indexOfChild(devItem));
            rootItem->insertChild(targetDevIdx, devItem);
            devItem->setExpanded(wasExpanded);
        }
        ++targetDevIdx;

        // Build lookup of existing category items under devItem
        QHash<QString, QTreeWidgetItem *> existingCategories;
        for (int i = 0; i < devItem->childCount(); ++i)
        {
            if (QTreeWidgetItem *catItem = devItem->child(i))
                existingCategories.insert(catItem->data(0, kStableIdRole).toString(), catItem);
        }

        int targetCatIdx = 0;
        for (const SensorCategory &cat : dev.Categories())
        {
            if (cat.Items().isEmpty())
                continue;

            const QString catKey = cat.Id().isEmpty() ? cat.Label() : cat.Id();
            QTreeWidgetItem *catItem = existingCategories.take(catKey);
            const bool isNewCat = (catItem == nullptr);
            if (isNewCat)
            {
                catItem = new QTreeWidgetItem();
                catItem->setData(0, kNodeKindRole, NodeCategory);
                catItem->setData(0, kStableIdRole, catKey);
                catItem->setIcon(0, CategoryIcon(cat.Type()));
            }

            if (catItem->text(0) != cat.Label())
                catItem->setText(0, cat.Label());

            if (devItem->indexOfChild(catItem) < 0)
            {
                devItem->insertChild(targetCatIdx, catItem);
                catItem->setExpanded(true);
            } else if (devItem->indexOfChild(catItem) != targetCatIdx)
            {
                const bool wasExpanded = catItem->isExpanded();
                devItem->takeChild(devItem->indexOfChild(catItem));
                devItem->insertChild(targetCatIdx, catItem);
                catItem->setExpanded(wasExpanded);
            }
            ++targetCatIdx;

            // Build lookup of existing sensor leaf items under catItem
            QHash<QString, QTreeWidgetItem *> existingLeaves;
            for (int i = 0; i < catItem->childCount(); ++i)
            {
                if (QTreeWidgetItem *leafItem = catItem->child(i))
                    existingLeaves.insert(leafItem->data(0, kStableIdRole).toString(), leafItem);
            }

            const QVector<SensorItem> &sensors = cat.Items();
            for (int sIdx = 0; sIdx < sensors.size(); ++sIdx)
            {
                const SensorItem &sensor = sensors.at(sIdx);
                const QString sensorKey = sensor.Id().isEmpty() ? sensor.Label() : sensor.Id();
                QTreeWidgetItem *leafItem = existingLeaves.take(sensorKey);
                if (!leafItem)
                {
                    leafItem = new QTreeWidgetItem();
                    leafItem->setData(0, kNodeKindRole, NodeSensorLeaf);
                    leafItem->setData(0, kStableIdRole, sensorKey);
                    for (int col = 0; col < 4; ++col)
                        leafItem->setTextAlignment(col, Qt::AlignLeft | Qt::AlignVCenter);
                }

                const QString valStr = sensor.FormattedValue();
                const QString minStr = sensor.FormattedMin();
                const QString maxStr = sensor.FormattedMax();

                if (leafItem->text(0) != sensor.Label())
                    leafItem->setText(0, sensor.Label());
                if (leafItem->text(1) != valStr)
                    leafItem->setText(1, valStr);
                if (leafItem->text(2) != minStr)
                    leafItem->setText(2, minStr);
                if (leafItem->text(3) != maxStr)
                    leafItem->setText(3, maxStr);

                if (catItem->indexOfChild(leafItem) < 0)
                {
                    catItem->insertChild(sIdx, leafItem);
                } else if (catItem->indexOfChild(leafItem) != sIdx)
                {
                    catItem->takeChild(catItem->indexOfChild(leafItem));
                    catItem->insertChild(sIdx, leafItem);
                }

                ++totalSensors;
            }

            for (QTreeWidgetItem *staleLeaf : std::as_const(existingLeaves))
                delete staleLeaf;
        }

        for (QTreeWidgetItem *staleCat : std::as_const(existingCategories))
            delete staleCat;
    }

    for (QTreeWidgetItem *staleDev : std::as_const(existingDevices))
        delete staleDev;

    if (vScroll)
        vScroll->setValue(savedVPos);
    if (hScroll)
        hScroll->setValue(savedHPos);

    this->ui->statusLabel->setText(tr("Ready — %1 sensors").arg(totalSensors));
    this->ui->treeWidget->viewport()->update();
}
