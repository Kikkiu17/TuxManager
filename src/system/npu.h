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

#ifndef NPU_H
#define NPU_H

#include "../globals.h"
#include "../historybuffer.h"

#include <QElapsedTimer>
#include <QString>
#include <memory>
#include <vector>

class NPU
{
    public:
        struct NPUInfo
        {
            QString         ID;
            QString         Name;
            QString         DeviceNode;
            QString         Driver;
            QString         DriverVersion;
            QString         PCIAddress;
            QString         PowerState;
            QString         SysfsPath;
            QString         BusyTimePath;
            QString         ActiveTimePath;
            QString         CurrentFreqPath;
            QString         MaxFreqPath;
            QString         MemUsagePath;

            double          UtilPct { 0.0 };
            int             CurrentFreqMHz { -1 };
            int             MaxFreqMHz { -1 };
            qint64          MemUsedBytes { -1 };

            HistoryBuffer   UtilHistory { TUX_MANAGER_HISTORY_SIZE };
            HistoryBuffer   MemHistory { TUX_MANAGER_HISTORY_SIZE };

            qint64          PrevBusyTimeUs { -1 };
            qint64          PrevActiveTimeMs { -1 };
            qint64          PrevSampleNs { 0 };
        };

        NPU();
        ~NPU();

        void Detect();
        bool Sample();

        int NpuCount() const { return static_cast<int>(this->m_npus.size()); }
        const NPUInfo &FromIndex(int index) const;

    private:
        NPUInfo m_nullNPU;
        std::vector<std::unique_ptr<NPUInfo>> m_npus;
        QElapsedTimer m_sampleTimer;
        bool m_sampleTimerStarted { false };
};

#endif // NPU_H
