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

#ifndef PERF_SWAPDETAILWIDGET_H
#define PERF_SWAPDETAILWIDGET_H

#include "swapgrapharea.h"
#include "historybuffer.h"
#include "graphwidget.h"

#include <QLabel>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class SwapDetailWidget; }
QT_END_NAMESPACE

namespace Perf
{
    class SwapDetailWidget : public QWidget
    {
        Q_OBJECT

        public:
            explicit SwapDetailWidget(QWidget *parent = nullptr);
            ~SwapDetailWidget();

            void Init();
            void ApplyColorScheme();
            void SetCompactMode(bool compact);

        private slots:
            void onUpdated();
            void onContextMenuRequested(const QPoint &globalPos);
            void onSwapDevicesChanged();

        private:
            Ui::SwapDetailWidget *ui { nullptr };
            QVector<QLabel *> m_statLabels;
            QVector<QLabel *> m_axisLabels;

            const HistoryBuffer *m_inHistory { nullptr };
            const HistoryBuffer *m_outHistory { nullptr };
    };
} // namespace Perf

#endif // PERF_SWAPDETAILWIDGET_H
