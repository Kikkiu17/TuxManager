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

#ifndef OS_PROC_H
#define OS_PROC_H

#include <QHash>
#include <QString>
#include <QtGlobal>

namespace OS
{
    class Proc
    {
        public:
            struct DiskIoSample
            {
                quint64 ReadBytes  { 0 };
                quint64 WriteBytes { 0 };
                quint64 ReadMs     { 0 };
                quint64 WriteMs    { 0 };
                quint64 IoMs       { 0 };
                quint64 DiscardMs  { 0 };
                quint64 FlushMs    { 0 };
            };

            /// Read total elapsed CPU jiffies from /proc/stat (all CPUs, all states).
            /// Returns 0 on error.
            static quint64 ReadTotalCpuJiffies();

            /// Read disk I/O active milliseconds per base block device from /proc/diskstats.
            /// Returns map of device name (e.g. "nvme0n1", "sda") -> cumulative io_ms.
            static QHash<QString, quint64> ReadDiskIoMs();

            /// Read detailed disk I/O counters per physical base block device from /proc/diskstats.
            /// Returns map of device name (e.g. "nvme0n1", "sda") -> DiskIoSample.
            static QHash<QString, DiskIoSample> ReadDiskIoStats();
    };
} // namespace OS

#endif // OS_PROC_H
