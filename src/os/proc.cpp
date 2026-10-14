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

#include "proc.h"

#include <QFile>

using namespace OS;

quint64 Proc::ReadTotalCpuJiffies()
{
    QFile f("/proc/stat");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return 0;

    const QByteArray line = f.readLine();
    f.close();

    // Format: cpu user nice system idle iowait irq softirq steal [guest guestnice]
    // guest/guestnice are already included in user/nice, so sum only fields 1-8.
    const QList<QByteArray> parts = line.simplified().split(' ');
    quint64 total = 0;
    const int last = qMin(parts.size() - 1, 8);
    for (int i = 1; i <= last; ++i)
        total += parts.at(i).toULongLong();
    return total;
}

QHash<QString, quint64> Proc::ReadDiskIoMs()
{
    QFile f("/proc/diskstats");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QHash<QString, quint64> result;
    for (;;)
    {
        const QByteArray line = f.readLine();
        if (line.isNull())
            break;

        const QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() < 14)
            continue;

        const QString devName = QString::fromUtf8(parts.at(2));
        if (devName.startsWith("loop") || devName.startsWith("ram")
            || devName.startsWith("zram") || devName.startsWith("sr")
            || devName.startsWith("dm-"))
        {
            continue;
        }

        // Only track base block devices exposed in /sys/block (e.g. sda, nvme0n1)
        if (!QFile::exists(QStringLiteral("/sys/block/") + devName))
            continue;

        const quint64 ioMs = parts.at(12).toULongLong();
        result.insert(devName, ioMs);
    }
    return result;
}

