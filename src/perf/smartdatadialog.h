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

#ifndef PERF_SMARTDATADIALOG_H
#define PERF_SMARTDATADIALOG_H

#include "../system/smartreader.h"
#include <QDialog>
#include <QTimer>

QT_BEGIN_NAMESPACE
namespace Ui { class SmartDataDialog; }
QT_END_NAMESPACE

namespace Perf
{
    class SmartDataDialog : public QDialog
    {
        Q_OBJECT

        public:
            explicit SmartDataDialog(const QString &diskName, const QString &diskModel, QWidget *parent = nullptr);
            ~SmartDataDialog() override;

        private slots:
            void refreshData(bool triggerDeviceUpdate = false);
            void updateRelativeTime();
            void onFilterChanged(const QString &filterText);
            void onModeChanged(int index);
            void onCopyClicked();
            void showContextMenu(const QPoint &pos);

        private:
            void populateTable();
            void applyFilter();

            Ui::SmartDataDialog *ui { nullptr };
            QString              m_diskName;
            QString              m_diskModel;
            System::SmartData    m_smartData;
            QTimer              *m_timer { nullptr };
    };
} // namespace Perf

#endif // PERF_SMARTDATADIALOG_H
