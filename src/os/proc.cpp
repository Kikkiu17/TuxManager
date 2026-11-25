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

#include <QDir>
#include <QFile>
#include <QFileInfo>

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
    const QHash<QString, DiskIoSample> stats = Proc::ReadDiskIoStats();
    QHash<QString, quint64> result;
    result.reserve(stats.size());
    for (auto it = stats.cbegin(); it != stats.cend(); ++it)
        result.insert(it.key(), it.value().IoMs);
    return result;
}

QHash<QString, Proc::DiskIoSample> Proc::ReadDiskIoStats()
{
    QFile f(QStringLiteral("/proc/diskstats"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QHash<QString, DiskIoSample> result;
    for (;;)
    {
        const QByteArray line = f.readLine();
        if (line.isNull())
            break;

        const QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() < 14)
            continue;

        const QString devName = QString::fromUtf8(parts.at(2));
        if (devName.isEmpty()
            || devName.contains(QLatin1Char('/'))
            || devName.startsWith(QStringLiteral("loop"))
            || devName.startsWith(QStringLiteral("pktcdvd"))
            || devName.startsWith(QStringLiteral("sr"))
            || devName.startsWith(QStringLiteral("dm-"))
            || devName.startsWith(QStringLiteral("ram"))
            || devName.startsWith(QStringLiteral("zram")))
        {
            continue;
        }

        // Only track physical base block devices exposed in /sys/block (e.g. sda, nvme0n1),
        // skipping partitions (absent from /sys/block) and virtual aggregators (md*, bcache*)
        // whose non-empty slaves directory would double-count underlying physical disks.
        const QString sysBlockPath = QStringLiteral("/sys/block/") + devName;
        if (!QFileInfo::exists(sysBlockPath))
            continue;

        const QDir slavesDir(sysBlockPath + QStringLiteral("/slaves"));
        if (slavesDir.exists() && !slavesDir.isEmpty(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot))
            continue;

        DiskIoSample sample;
        sample.ReadBytes  = parts.at(5).toULongLong() * 512ULL;
        sample.ReadMs     = parts.at(6).toULongLong();
        sample.WriteBytes = parts.at(9).toULongLong() * 512ULL;
        sample.WriteMs    = parts.at(10).toULongLong();
        sample.IoMs       = parts.at(12).toULongLong();
        sample.DiscardMs  = (parts.size() >= 18) ? parts.at(17).toULongLong() : 0ULL;
        sample.FlushMs    = (parts.size() >= 20) ? parts.at(19).toULongLong() : 0ULL;
        result.insert(devName, sample);
    }
    return result;
}

