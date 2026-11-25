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

#include "processmodel.h"

#include "appregistry.h"
#include "proc.h"
#include "../configuration.h"
#include "../misc.h"

#include <algorithm>

#include <unistd.h>

using namespace OS;

namespace
{
    bool lessThanIdentity(const Process &lhs, const Process &rhs)
    {
        return lhs.GetIdentity() < rhs.GetIdentity();
    }

    bool processDataEquals(const Process &lhs, const Process &rhs)
    {
        return lhs.PID == rhs.PID
               && lhs.PPID == rhs.PPID
               && lhs.Name == rhs.Name
               && lhs.CmdLine == rhs.CmdLine
               && lhs.State == rhs.State
               && lhs.UID == rhs.UID
               && lhs.User == rhs.User
               && lhs.Priority == rhs.Priority
               && lhs.Nice == rhs.Nice
               && lhs.Threads == rhs.Threads
               && lhs.VMRssKb == rhs.VMRssKb
               && lhs.MemoryPssKb == rhs.MemoryPssKb
               && lhs.MemoryPssExact == rhs.MemoryPssExact
               && lhs.vmSizeKb == rhs.vmSizeKb
               && lhs.SharedKb == rhs.SharedKb
               && lhs.TextKb == rhs.TextKb
               && lhs.DataKb == rhs.DataKb
               && lhs.CPUTicks == rhs.CPUTicks
               && lhs.CPUPercent == rhs.CPUPercent
               && lhs.StartTimeTicks == rhs.StartTimeTicks
               && lhs.IsKernelThread == rhs.IsKernelThread
               && lhs.IOReadBytes == rhs.IOReadBytes
               && lhs.IOWriteBytes == rhs.IOWriteBytes
               && lhs.IOReadBps == rhs.IOReadBps
               && lhs.IOWriteBps == rhs.IOWriteBps
               && lhs.DiskPercent == rhs.DiskPercent
               && lhs.IOTotalsAvailable == rhs.IOTotalsAvailable
               && lhs.IORatesAvailable == rhs.IORatesAvailable
               && lhs.IOPermissionDenied == rhs.IOPermissionDenied
               && lhs.IconName == rhs.IconName;
    }
}

// ── Construction ──────────────────────────────────────────────────────────────

ProcessModel::ProcessModel(QObject *parent) : QAbstractTableModel(parent)
{
    this->m_numCpus = qMax(1, static_cast<int>(::sysconf(_SC_NPROCESSORS_ONLN)));
    this->m_myUid = ::getuid();
}

// ── QAbstractTableModel interface ─────────────────────────────────────────────

int ProcessModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return this->m_processes.size();
}

int ProcessModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return ColCount;
}

QVariant ProcessModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()
        || index.row()    < 0
        || index.row()    >= this->m_processes.size()
        || index.column() < 0
        || index.column() >= ColCount)
        return {};

    const Process &proc = this->m_processes.at(index.row());

    if (role == Qt::DecorationRole)
    {
        if (index.column() == ColName && this->m_appRegistry)
            return this->m_appRegistry->IconFor(proc.IconName);
        return {};
    }

    if (role == Qt::DisplayRole)
    {
        switch (static_cast<Column>(index.column()))
        {
            case ColPid:      return proc.PID;
            case ColName:     return proc.Name;
            case ColUser:     return proc.User;
            case ColState:    return Process::GetStateString(proc.State);
            case ColCpu:
            {
                const double cpuPct = proc.CPUPercent * (this->m_cpuPerCore ? static_cast<double>(this->m_numCpus) : 1.0);
                return QString::number(cpuPct, 'f', 1) + " %";
            }
            case ColMemPss:   return Misc::FormatKiB(proc.MemoryPssKb, 0);
            case ColMemRss:   return Misc::FormatKiB(proc.VMRssKb, 0);
            case ColMemVirt:  return Misc::FormatKiB(proc.vmSizeKb, 0);
            case ColMemShared:return Misc::FormatKiB(proc.SharedKb, 0);
            case ColMemText:  return Misc::FormatKiB(proc.TextKb, 0);
            case ColMemData:  return Misc::FormatKiB(proc.DataKb, 0);
            case ColDisk:
                if (proc.IOPermissionDenied || !proc.IOTotalsAvailable)
                    return QString("?");
                if (!proc.IORatesAvailable)
                    return tr("measuring...");
                if (proc.DiskPercent < 0.0)
                    return QString("?");
                return QString::number(proc.DiskPercent, 'f', 1) + " %";
            case ColIoReads:
                return (!proc.IOPermissionDenied && proc.IOTotalsAvailable)
                       ? Misc::FormatBytes(proc.IOReadBytes, 0)
                       : QString("?");
            case ColIoWrites:
                return (!proc.IOPermissionDenied && proc.IOTotalsAvailable)
                       ? Misc::FormatBytes(proc.IOWriteBytes, 0)
                       : QString("?");
            case ColIoReadsPerSec:
                if (proc.IOPermissionDenied || !proc.IOTotalsAvailable)
                    return QString("?");
                return proc.IORatesAvailable ? Misc::FormatBytesPerSecond(proc.IOReadBps) : tr("measuring...");
            case ColIoWritesPerSec:
                if (proc.IOPermissionDenied || !proc.IOTotalsAvailable)
                    return QString("?");
                return proc.IORatesAvailable ? Misc::FormatBytesPerSecond(proc.IOWriteBps) : tr("measuring...");
            case ColThreads:  return proc.Threads;
            case ColPriority: return proc.Priority;
            case ColNice:     return proc.Nice;
            case ColCmdline:  return proc.CmdLine;
            default: break;
        }
    }

    // Raw numeric values for sorting
    if (role == Qt::UserRole)
    {
        switch (static_cast<Column>(index.column()))
        {
            case ColPid:      return static_cast<qlonglong>(proc.PID);
            case ColCpu:
            {
                const double cpuPct = proc.CPUPercent * (this->m_cpuPerCore ? static_cast<double>(this->m_numCpus) : 1.0);
                return cpuPct;
            }
            case ColMemPss:   return static_cast<qulonglong>(proc.MemoryPssKb);
            case ColMemRss:   return static_cast<qulonglong>(proc.VMRssKb);
            case ColMemVirt:  return static_cast<qulonglong>(proc.vmSizeKb);
            case ColMemShared:return static_cast<qulonglong>(proc.SharedKb);
            case ColMemText:  return static_cast<qulonglong>(proc.TextKb);
            case ColMemData:  return static_cast<qulonglong>(proc.DataKb);
            case ColDisk:
                if (proc.IOPermissionDenied || !proc.IOTotalsAvailable || !proc.IORatesAvailable || proc.DiskPercent < 0.0)
                    return QVariant::fromValue(-1.0);
                return QVariant::fromValue(proc.DiskPercent);
            case ColIoReads:
                return (!proc.IOPermissionDenied && proc.IOTotalsAvailable)
                       ? QVariant::fromValue(static_cast<qlonglong>(proc.IOReadBytes))
                       : QVariant::fromValue(static_cast<qlonglong>(-1));
            case ColIoWrites:
                return (!proc.IOPermissionDenied && proc.IOTotalsAvailable)
                       ? QVariant::fromValue(static_cast<qlonglong>(proc.IOWriteBytes))
                       : QVariant::fromValue(static_cast<qlonglong>(-1));
            case ColIoReadsPerSec:
                return (!proc.IOPermissionDenied && proc.IOTotalsAvailable && proc.IORatesAvailable)
                       ? QVariant::fromValue(proc.IOReadBps)
                       : QVariant::fromValue(-1.0);
            case ColIoWritesPerSec:
                return (!proc.IOPermissionDenied && proc.IOTotalsAvailable && proc.IORatesAvailable)
                       ? QVariant::fromValue(proc.IOWriteBps)
                       : QVariant::fromValue(-1.0);
            case ColThreads:  return proc.Threads;
            case ColPriority: return proc.Priority;
            case ColNice:     return proc.Nice;
            default:          return this->data(index, Qt::DisplayRole);
        }
    }

    if (role == Qt::TextAlignmentRole)
    {
        switch (static_cast<Column>(index.column()))
        {
            case ColPid:
            case ColCpu:
            case ColMemPss:
            case ColMemRss:
            case ColMemVirt:
            case ColMemShared:
            case ColMemText:
            case ColMemData:
            case ColDisk:
            case ColIoReads:
            case ColIoWrites:
            case ColIoReadsPerSec:
            case ColIoWritesPerSec:
            case ColThreads:
            case ColPriority:
            case ColNice:
                return QVariant(Qt::AlignRight | Qt::AlignVCenter);
            default:
                return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }

    if (role == Qt::ToolTipRole)
    {
        switch (static_cast<Column>(index.column()))
        {
            case ColMemPss:
                if (!proc.MemoryPssExact && proc.VMRssKb > 0)
                    return tr("Estimated from private RSS (insufficient permissions or pending PSS sample)");
                break;
            case ColDisk:
                if (proc.IOPermissionDenied)
                    return tr("Insufficient permissions to read process I/O statistics");
                if (proc.IOTotalsAvailable && proc.IORatesAvailable && proc.DiskPercent < 0.0)
                    return tr("Disk utilization unavailable (no block device statistics)");
                break;
            case ColIoReads:
            case ColIoWrites:
            case ColIoReadsPerSec:
            case ColIoWritesPerSec:
                if (proc.IOPermissionDenied)
                    return tr("Insufficient permissions to read process I/O statistics");
                break;
            default:
                break;
        }
    }

    return {};
}

QVariant ProcessModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || section < 0 || section >= ColCount)
        return {};

    const Column col = static_cast<Column>(section);
    if (role == Qt::DisplayRole)
        return columnHeader(col);

    if (role == Qt::ToolTipRole && col == ColCpu)
    {
        if (this->m_cpuPerCore)
            return tr("Per-core CPU usage (0–%1% across %2 logical CPUs)").arg(100 * this->m_numCpus).arg(this->m_numCpus);
        return tr("Total CPU usage across all cores (0–100%)");
    }

    return {};
}

Qt::ItemFlags ProcessModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

// ── Refresh ───────────────────────────────────────────────────────────────────

void ProcessModel::Refresh()
{
    QList<Process> fresh = this->RefreshSnapshot();

    this->SetProcesses(fresh);
}

void ProcessModel::SetProcesses(const QList<Process> &processes)
{
    QList<Process> sortedProcesses = processes;
    std::sort(sortedProcesses.begin(), sortedProcesses.end(), lessThanIdentity);

    int row = 0;
    while (row < this->m_processes.size() || row < sortedProcesses.size())
    {
        if (row >= this->m_processes.size())
        {
            const int insertFirst = row;
            int insertLast = insertFirst;
            while (insertLast + 1 < sortedProcesses.size())
                ++insertLast;

            this->beginInsertRows(QModelIndex(), insertFirst, insertLast);
            for (int i = insertFirst; i <= insertLast; ++i)
                this->m_processes.insert(i, sortedProcesses.at(i));
            this->endInsertRows();
            break;
        }

        if (row >= sortedProcesses.size())
        {
            const int removeFirst = row;
            const int removeLast = this->m_processes.size() - 1;
            this->beginRemoveRows(QModelIndex(), removeFirst, removeLast);
            while (this->m_processes.size() > removeFirst)
                this->m_processes.removeAt(removeFirst);
            this->endRemoveRows();
            break;
        }

        const Process::Identity currentId = this->m_processes.at(row).GetIdentity();
        const Process::Identity incomingId = sortedProcesses.at(row).GetIdentity();

        if (currentId == incomingId)
        {
            if (!processDataEquals(this->m_processes.at(row), sortedProcesses.at(row)))
            {
                this->m_processes[row] = sortedProcesses.at(row);
                emit this->dataChanged(this->index(row, 0),
                                       this->index(row, ColCount - 1),
                                       { Qt::DisplayRole, Qt::DecorationRole, Qt::UserRole, Qt::TextAlignmentRole, Qt::ToolTipRole });
            }
            ++row;
            continue;
        }

        if (currentId < incomingId)
        {
            int removeLast = row;
            while (removeLast + 1 < this->m_processes.size()
                   && this->m_processes.at(removeLast + 1).GetIdentity() < incomingId)
            {
                ++removeLast;
            }

            this->beginRemoveRows(QModelIndex(), row, removeLast);
            for (int i = removeLast; i >= row; --i)
                this->m_processes.removeAt(i);
            this->endRemoveRows();
            continue;
        }

        int insertLast = row;
        while (insertLast + 1 < sortedProcesses.size()
               && sortedProcesses.at(insertLast + 1).GetIdentity() < currentId)
        {
            ++insertLast;
        }

        this->beginInsertRows(QModelIndex(), row, insertLast);
        for (int i = row; i <= insertLast; ++i)
            this->m_processes.insert(i, sortedProcesses.at(i));
        this->endInsertRows();
    }
}

QList<Process> ProcessModel::RefreshSnapshot()
{
    // Read total elapsed CPU jiffies (all CPUs, all states) from /proc/stat.
    // Using the actual CPU time budget as the denominator — rather than wall
    // clock time — matches htop's approach and gives accurate results even
    // when the timer fires slightly early or late.
    const quint64 totalJiffies = Proc::ReadTotalCpuJiffies();
    const quint64 periodJiffies =
        (this->m_prevCpuTotalTicks > 0 && totalJiffies > this->m_prevCpuTotalTicks)
        ? (totalJiffies - this->m_prevCpuTotalTicks) : 0;

    Process::LoadOptions opts;
    opts.IncludeKernelTasks = this->m_showKernelTasks;
    opts.IncludeOtherUsers  = this->m_showOtherUsersProcs;
    opts.MyUID              = this->m_myUid;
    opts.CollectIOMetrics   = this->m_ioMetricsEnabled;
    opts.CollectPssMetrics  = CFG->PssMetricsEnabled;
    opts.IsSuperuser        = CFG->IsSuperuser;
    opts.EffectiveUID       = CFG->EUID;
    QList<Process> fresh = Process::LoadAll(opts);

    // Calculate CPU% per process: (delta process ticks) / (total period jiffies) * 100
    if (periodJiffies > 0)
    {
        const double totalPeriod = static_cast<double>(periodJiffies);

        for (Process &proc : fresh)
        {
            const Process::Identity id = proc.GetIdentity();
            const auto it = this->m_prevTicks.constFind(id);
            if (it != this->m_prevTicks.cend() && proc.CPUTicks >= it.value())
            {
                const double pct = static_cast<double>(proc.CPUTicks - it.value()) / totalPeriod * 100.0;
                // Cap at 100% of the entire CPU
                proc.CPUPercent = qMin(pct, 100.0);
            }
        }
    }

    const QHash<QString, quint64> currentDiskIoMs = this->m_ioMetricsEnabled ? Proc::ReadDiskIoMs() : QHash<QString, quint64>();

    const qint64 ioElapsedMs = this->m_ioMetricsEnabled
                               ? (this->m_prevIoSampleTimer.isValid() ? this->m_prevIoSampleTimer.restart() : -1)
                               : -1;
    if (this->m_ioMetricsEnabled && !this->m_prevIoSampleTimer.isValid())
        this->m_prevIoSampleTimer.start();

    if (this->m_ioMetricsEnabled && ioElapsedMs > 0)
    {
        const double elapsedSec = static_cast<double>(ioElapsedMs) / 1000.0;
        for (Process &proc : fresh)
        {
            if (!proc.IOTotalsAvailable)
                continue;
            const Process::Identity id = proc.GetIdentity();
            const auto readIt = this->m_prevIoReadBytes.constFind(id);
            const auto writeIt = this->m_prevIoWriteBytes.constFind(id);
            if (readIt == this->m_prevIoReadBytes.cend() || writeIt == this->m_prevIoWriteBytes.cend())
                continue;

            const quint64 prevReadBytes = readIt.value();
            const quint64 prevWriteBytes = writeIt.value();
            if (proc.IOReadBytes < prevReadBytes || proc.IOWriteBytes < prevWriteBytes)
                continue;

            proc.IOReadBps = static_cast<double>(proc.IOReadBytes - prevReadBytes) / elapsedSec;
            proc.IOWriteBps = static_cast<double>(proc.IOWriteBytes - prevWriteBytes) / elapsedSec;
            proc.IORatesAvailable = true;
        }

        double totalDiskActivePct = 0.0;
        if (!this->m_prevDiskIoMs.isEmpty())
        {
            for (auto it = currentDiskIoMs.cbegin(); it != currentDiskIoMs.cend(); ++it)
            {
                const auto prevIt = this->m_prevDiskIoMs.constFind(it.key());
                if (prevIt == this->m_prevDiskIoMs.cend() || it.value() < prevIt.value())
                    continue;
                const quint64 deltaMs = it.value() - prevIt.value();
                const double diskPct = qBound(0.0, static_cast<double>(deltaMs) * 100.0 / static_cast<double>(ioElapsedMs), 100.0);
                totalDiskActivePct += diskPct;
            }
            totalDiskActivePct = qMin(totalDiskActivePct, 100.0);
        }

        double totalProcessIoBps = 0.0;
        for (const Process &proc : std::as_const(fresh))
        {
            if (proc.IORatesAvailable)
                totalProcessIoBps += (proc.IOReadBps + proc.IOWriteBps);
        }

        for (Process &proc : fresh)
        {
            if (currentDiskIoMs.isEmpty() || this->m_prevDiskIoMs.isEmpty())
            {
                proc.DiskPercent = proc.IORatesAvailable ? -1.0 : 0.0;
                continue;
            }
            if (!proc.IORatesAvailable || totalProcessIoBps <= 0.0)
            {
                proc.DiskPercent = 0.0;
                continue;
            }

            const double procIoBps = proc.IOReadBps + proc.IOWriteBps;
            proc.DiskPercent = (totalDiskActivePct > 0.0)
                               ? qBound(0.0, (procIoBps / totalProcessIoBps) * totalDiskActivePct, 100.0)
                               : 0.0;
        }
    }

    // Store snapshots for next sample
    this->m_prevTicks.clear();
    this->m_prevTicks.reserve(fresh.size());
    for (const Process &proc : std::as_const(fresh))
        this->m_prevTicks.insert(proc.GetIdentity(), proc.CPUTicks);
    this->m_prevCpuTotalTicks = totalJiffies;

    if (this->m_ioMetricsEnabled)
    {
        this->m_prevIoReadBytes.clear();
        this->m_prevIoWriteBytes.clear();
        for (const Process &proc : std::as_const(fresh))
        {
            if (!proc.IOTotalsAvailable)
                continue;
            const Process::Identity id = proc.GetIdentity();
            this->m_prevIoReadBytes.insert(id, proc.IOReadBytes);
            this->m_prevIoWriteBytes.insert(id, proc.IOWriteBytes);
        }
        this->m_prevDiskIoMs = currentDiskIoMs;
        if (!this->m_prevIoSampleTimer.isValid())
            this->m_prevIoSampleTimer.start();
    }

    return fresh;
}

void ProcessModel::SetIOMetricsEnabled(bool enabled)
{
    if (this->m_ioMetricsEnabled == enabled)
        return;

    this->m_ioMetricsEnabled = enabled;
    this->FlushIOMetrics();
}

void ProcessModel::FlushIOMetrics()
{
    this->m_prevIoReadBytes.clear();
    this->m_prevIoWriteBytes.clear();
    this->m_prevDiskIoMs.clear();
    this->m_prevIoSampleTimer.invalidate();
}

void ProcessModel::SetCpuPerCore(bool perCore)
{
    if (this->m_cpuPerCore == perCore)
        return;

    this->m_cpuPerCore = perCore;
    emit this->headerDataChanged(Qt::Horizontal, ColCpu, ColCpu);
    if (!this->m_processes.isEmpty())
    {
        emit this->dataChanged(this->index(0, ColCpu),
                               this->index(this->m_processes.size() - 1, ColCpu),
                               { Qt::DisplayRole, Qt::UserRole, Qt::ToolTipRole });
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

QString ProcessModel::columnHeader(Column col)
{
    switch (col)
    {
        case ColPid:      return tr("PID");
        case ColName:     return tr("Name");
        case ColUser:     return tr("User");
        case ColState:    return tr("State");
        case ColCpu:      return tr("CPU %");
        case ColMemPss:   return tr("MEM PSS");
        case ColMemRss:   return tr("MEM RES");
        case ColMemVirt:  return tr("MEM VIRT");
        case ColMemShared:return tr("MEM SHR");
        case ColMemText:  return tr("MEM TEXT");
        case ColMemData:  return tr("MEM DATA");
        case ColDisk:     return tr("Disk %");
        case ColIoReads:  return tr("IO Reads");
        case ColIoWrites: return tr("IO Writes");
        case ColIoReadsPerSec: return tr("IO Read/s");
        case ColIoWritesPerSec:return tr("IO Write/s");
        case ColThreads:  return tr("Threads");
        case ColPriority: return tr("Priority");
        case ColNice:     return tr("Nice");
        case ColCmdline:  return tr("Command");
        default:          return {};
    }
}
