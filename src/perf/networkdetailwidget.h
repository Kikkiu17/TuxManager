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

#ifndef PERF_NETWORKDETAILWIDGET_H
#define PERF_NETWORKDETAILWIDGET_H

#include "historybuffer.h"
#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class NetworkDetailWidget; }
QT_END_NAMESPACE

namespace Perf
{
    class NetworkDetailWidget : public QWidget
    {
        Q_OBJECT

        public:
            explicit NetworkDetailWidget(QWidget *parent = nullptr);
            ~NetworkDetailWidget();

            void SetNetwork(int index);
            void ApplyColorScheme();
            void SetCompactMode(bool compact);

        private slots:
            void onUpdated();
            void onGraphContextMenuRequested(const QPoint &pos);
            void onShowBitsTriggered();
            void onShowBytesTriggered();

        private:
            void applyTransferUnitMode();
            QString formatTransferRate(double bytesPerSec) const;

            Ui::NetworkDetailWidget *ui;
            int                      m_networkIndex { -1 };
    };
} // namespace Perf

#endif // PERF_NETWORKDETAILWIDGET_H
