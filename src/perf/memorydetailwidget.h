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

#ifndef PERF_MEMORYDETAILWIDGET_H
#define PERF_MEMORYDETAILWIDGET_H

#include "historybuffer.h"

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class MemoryDetailWidget; }
QT_END_NAMESPACE

namespace Perf
{
    class MemoryDetailWidget : public QWidget
    {
        Q_OBJECT

        public:
            explicit MemoryDetailWidget(QWidget *parent = nullptr);
            ~MemoryDetailWidget();

            void Init();
            void ApplyColorScheme();
            void SetCompactMode(bool compact);

        private slots:
            void onUpdated();

        private:
            void updateCompressedVisibility(bool visible);

            Ui::MemoryDetailWidget *ui;
            const HistoryBuffer  *m_memHistory { nullptr };
            bool                  m_compactMode { false };
    };
} // namespace Perf

#endif // PERF_MEMORYDETAILWIDGET_H
