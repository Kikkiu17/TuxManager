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

#ifndef GPUNVMLBACKEND_H
#define GPUNVMLBACKEND_H

#include "../gpu.h"

#include <QString>
#include <memory>
#include <vector>

class GpuNvmlBackend
{
    public:
        GpuNvmlBackend() = default;
        ~GpuNvmlBackend();

        void Detect();
        bool Sample(std::vector<std::unique_ptr<GPU::GPUInfo>> &gpus);
        bool IsAvailable() const { return this->m_available; }

    private:
        struct DeviceEntry
        {
            void   *handle { nullptr };
            QString id;
            QString name;
            QString pciRuntimeStatusPath;
            qint64  totalMemMiB { 0 };
        };

        void unload();

        bool    m_available { false };
        void   *m_libHandle { nullptr };
        QString m_driverVersion;
        std::vector<DeviceEntry> m_devices;
};

#endif // GPUNVMLBACKEND_H
