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

#ifndef SENSORSWIDGET_H
#define SENSORSWIDGET_H

#include "sensors.h"

#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QTreeWidget>
#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui
{
    class SensorsWidget;
}
QT_END_NAMESPACE

class SensorsTreeWidget : public QTreeWidget
{
    Q_OBJECT

    public:
        explicit SensorsTreeWidget(QWidget *parent = nullptr);
        ~SensorsTreeWidget() override = default;

    protected:
        void drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
        void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override;
        void mousePressEvent(QMouseEvent *event) override;
};

class SensorsWidget : public QWidget
{
    Q_OBJECT

    public:
        explicit SensorsWidget(QWidget *parent = nullptr);
        ~SensorsWidget() override;

        void SetActive(bool active);
        bool IsActive() const { return this->m_active; }
        void setActive(bool active) { this->SetActive(active); }
        bool isActive() const { return this->IsActive(); }

        void Refresh();
        void refresh() { this->Refresh(); }
        void Sample() { this->Refresh(); }
        void sample() { this->Refresh(); }

        void ResetMinMax();
        void resetMinMax() { this->ResetMinMax(); }

        void ExpandAll();
        void expandAll() { this->ExpandAll(); }

        void CollapseAll();
        void collapseAll() { this->CollapseAll(); }

        QTreeWidget *TreeWidget() const;
        QTreeWidget *treeWidget() const { return this->TreeWidget(); }

        const Sensors &GetSensors() const { return this->m_sensors; }
        const Sensors &sensors() const { return this->m_sensors; }

        static QIcon SystemIcon();
        static QIcon DeviceIcon(SensorDeviceType type);
        static QIcon CategoryIcon(SensorType type);

    private slots:
        void onTimerTick();
        void onContextMenu(const QPoint &pos);

    private:
        Ui::SensorsWidget *ui { nullptr };
        Sensors            m_sensors;
        QTimer            *m_refreshTimer { nullptr };
        bool               m_active { false };

        void rebuildOrUpdateTree();
};

using SensorsTab = SensorsWidget;

#endif // SENSORSWIDGET_H
