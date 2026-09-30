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

#ifndef PERF_GPUDETAILWIDGET_H
#define PERF_GPUDETAILWIDGET_H

#include "globals.h"
#include "graphwidget.h"
#include "historybuffer.h"

#include <QComboBox>
#include <QLabel>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class GpuDetailWidget; }
QT_END_NAMESPACE

namespace Perf
{
    class GpuDetailWidget : public QWidget
    {
        Q_OBJECT

        public:
            explicit GpuDetailWidget(QWidget *parent = nullptr);
            ~GpuDetailWidget();

            void SetGpu(int index);
            void ApplyColorScheme();
            void SetCompactMode(bool compact);

        private slots:
            void onUpdated();
            void onEngineSelectionChanged(int slot, int comboIndex);

        private:
            void rebuildEngineSelectors();
            void bindGpuIdentity();
            void bindEngineGraphSource(int slot);
            void bindMemoryAndCopySources();

            Ui::GpuDetailWidget *ui { nullptr };
            int m_gpuIndex { -1 };

            QVector<QComboBox *>   m_engineSelectors;
            QVector<QLabel *>      m_engineValueLabels;
            QVector<GraphWidget *> m_engineGraphs;
            QVector<int>           m_selectedEngineBySlot;
    };
} // namespace Perf

#endif // PERF_GPUDETAILWIDGET_H
