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

#include "processrefreshservice.h"

#include "proc.h"

#include <QPair>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <unistd.h>
#include <utility>

using namespace OS;

ProcessRefreshWorker::ProcessRefreshWorker(QObject *parent) : QObject(parent)
{
    this->m_numCpus = static_cast<int>(::sysconf(_SC_NPROCESSORS_ONLN));
    if (this->m_numCpus < 1)
        this->m_numCpus = 1;
}

void ProcessRefreshWorker::fetch(int consumer,
                                 quint64 token,
                                 bool includeKernelTasks,
                                 bool includeOtherUsers,
                                 bool collectIOMetrics,
                                 bool collectPssMetrics,
                                 bool isSuperuser,
                                 uint effectiveUid,
                                 uint myUid)
{
    const quint64 totalJiffies = Proc::ReadTotalCpuJiffies();
    const quint64 periodJiffies =
        (this->m_prevCpuTotalTicks > 0 && totalJiffies > this->m_prevCpuTotalTicks)
        ? (totalJiffies - this->m_prevCpuTotalTicks) : 0;

    Process::LoadOptions opts;
    opts.IncludeKernelTasks = includeKernelTasks;
    opts.IncludeOtherUsers = includeOtherUsers;
    opts.MyUID = static_cast<uid_t>(myUid);
    opts.CollectIOMetrics = collectIOMetrics;
    opts.CollectPssMetrics = false; // Sampled incrementally via delta-cached updatePssMetrics()
    opts.IsSuperuser = isSuperuser;
    opts.EffectiveUID = static_cast<uid_t>(effectiveUid);

    QList<Process> fresh = Process::LoadAll(opts);

    const bool isProcessConsumer = (consumer == static_cast<int>(ProcessRefreshService::Consumer::Processes));
    if (collectPssMetrics)
    {
        this->updatePssMetrics(fresh, isSuperuser, static_cast<uid_t>(effectiveUid));
    } else if (isProcessConsumer)
    {
        this->flushPssCache();
    }

    if (periodJiffies > 0)
    {
        const double totalPeriod = static_cast<double>(periodJiffies);

        for (Process &proc : fresh)
        {
            const auto it = this->m_prevTicks.constFind(proc.PID);
            if (it == this->m_prevTicks.cend() || proc.CPUTicks < it.value())
                continue;

            const double pct = static_cast<double>(proc.CPUTicks - it.value()) / totalPeriod * 100.0;
            // Cap at 100% of the entire CPU
            proc.CPUPercent = qMin(pct, 100.0);
        }
    }

    const bool handleIo = collectIOMetrics && isProcessConsumer;
    const QHash<QString, quint64> currentDiskIoMs = handleIo ? Proc::ReadDiskIoMs() : QHash<QString, quint64>();

    const qint64 ioElapsedMs = handleIo
                               ? (this->m_prevIoSampleTimer.isValid() ? this->m_prevIoSampleTimer.restart() : -1)
                               : -1;
    if (handleIo && !this->m_prevIoSampleTimer.isValid())
        this->m_prevIoSampleTimer.start();

    if (handleIo && ioElapsedMs > 0)
    {
        const double elapsedSec = static_cast<double>(ioElapsedMs) / 1000.0;
        for (Process &proc : fresh)
        {
            if (!proc.IOTotalsAvailable)
                continue;

            const auto readIt = this->m_prevIoReadBytes.constFind(proc.PID);
            const auto writeIt = this->m_prevIoWriteBytes.constFind(proc.PID);
            if (readIt == this->m_prevIoReadBytes.cend() || writeIt == this->m_prevIoWriteBytes.cend())
                continue;
            if (proc.IOReadBytes < readIt.value() || proc.IOWriteBytes < writeIt.value())
                continue;

            proc.IOReadBps = static_cast<double>(proc.IOReadBytes - readIt.value()) / elapsedSec;
            proc.IOWriteBps = static_cast<double>(proc.IOWriteBytes - writeIt.value()) / elapsedSec;
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
            if (!proc.IORatesAvailable || totalProcessIoBps <= 0.0)
            {
                proc.DiskPercent = 0.0;
                continue;
            }

            const double procIoBps = proc.IOReadBps + proc.IOWriteBps;
            if (!currentDiskIoMs.isEmpty())
            {
                proc.DiskPercent = (totalDiskActivePct > 0.0)
                                   ? qBound(0.0, (procIoBps / totalProcessIoBps) * totalDiskActivePct, 100.0)
                                   : 0.0;
            }
            else
            {
                // Fallback for environments without /proc/diskstats access: relative process share
                proc.DiskPercent = qBound(0.0, (procIoBps / totalProcessIoBps) * 100.0, 100.0);
            }
        }
    }

    this->m_prevTicks.clear();
    this->m_prevTicks.reserve(fresh.size());
    for (const Process &proc : std::as_const(fresh))
        this->m_prevTicks.insert(proc.PID, proc.CPUTicks);
    this->m_prevCpuTotalTicks = totalJiffies;

    if (handleIo)
    {
        this->m_prevIoReadBytes.clear();
        this->m_prevIoWriteBytes.clear();
        for (const Process &proc : std::as_const(fresh))
        {
            if (!proc.IOTotalsAvailable)
                continue;
            this->m_prevIoReadBytes.insert(proc.PID, proc.IOReadBytes);
            this->m_prevIoWriteBytes.insert(proc.PID, proc.IOWriteBytes);
        }
        this->m_prevDiskIoMs = currentDiskIoMs;
        if (!this->m_prevIoSampleTimer.isValid())
            this->m_prevIoSampleTimer.start();
    } else if (isProcessConsumer)
    {
        this->flushIOMetrics();
    }

    emit fetched(consumer, token, fresh);
}

void ProcessRefreshWorker::updatePssMetrics(QList<Process> &processes, bool isSuperuser, uid_t effectiveUid)
{
    static constexpr quint64 PSS_MIN_DELTA_KB       = 256ULL;
    static constexpr quint64 PSS_MAX_DELTA_KB       = 4096ULL;
    static constexpr quint64 PSS_STALE_TICKS        = 15ULL;
    static constexpr qint64  PSS_UNCACHED_BUDGET_MS = 60;
    static constexpr qint64  PSS_DIRTY_BUDGET_MS    = 25;
    static constexpr qint64  PSS_STALE_BUDGET_MS    = 8;
    static constexpr int     PSS_MAX_STALE_PER_TICK = 10;

    ++this->m_pssTick;

    QSet<Process::Identity> liveIdentities;
    liveIdentities.reserve(processes.size());

    QVector<int> uncachedIndices;
    QVector<QPair<int, quint64>> dirtyIndices;
    QVector<QPair<int, quint64>> staleIndices;

    for (int i = 0; i < processes.size(); ++i)
    {
        Process &proc = processes[i];
        const Process::Identity id = proc.GetIdentity();
        liveIdentities.insert(id);

        if (proc.IsKernelThread || proc.VMRssKb == 0)
        {
            proc.MemoryPssKb = 0;
            proc.MemoryPssExact = true;
            continue;
        }

        // Default fallback when smaps_rollup is unreadable or deferred by the per-tick budget.
        proc.MemoryPssKb = proc.PrivateRssKb();
        proc.MemoryPssExact = false;

        if (!isSuperuser && proc.UID != effectiveUid)
            continue;

        const auto it = this->m_pssCache.constFind(id);
        if (it == this->m_pssCache.cend())
        {
            uncachedIndices.append(i);
            continue;
        }

        const PssCacheEntry &entry = it.value();
        if (entry.PermissionDenied)
            continue;

        // Clamp cached PSS to current RSS so PSS never exceeds RSS if budget defers re-sampling.
        proc.MemoryPssKb = qMin(entry.PssKb, proc.VMRssKb);
        proc.MemoryPssExact = true;

        const quint64 rssDelta = (proc.VMRssKb >= entry.LastRssKb)
                                 ? (proc.VMRssKb - entry.LastRssKb)
                                 : (entry.LastRssKb - proc.VMRssKb);
        const quint64 sharedDelta = (proc.SharedKb >= entry.LastSharedKb)
                                    ? (proc.SharedKb - entry.LastSharedKb)
                                    : (entry.LastSharedKb - proc.SharedKb);
        const quint64 thresholdKb = qBound(PSS_MIN_DELTA_KB, proc.VMRssKb / 100ULL, PSS_MAX_DELTA_KB);

        if (rssDelta >= thresholdKb || sharedDelta >= thresholdKb || proc.VMRssKb < entry.PssKb)
        {
            dirtyIndices.append({ i, qMax(rssDelta, sharedDelta) });
        } else if (this->m_pssTick >= entry.LastSampledTick && (this->m_pssTick - entry.LastSampledTick) >= PSS_STALE_TICKS)
        {
            staleIndices.append({ i, entry.LastSampledTick });
        }
    }

    // Prioritize heaviest uncached processes and largest RSS/Shared changes first.
    std::sort(uncachedIndices.begin(), uncachedIndices.end(), [&processes](int a, int b)
    {
        return processes.at(a).VMRssKb > processes.at(b).VMRssKb;
    });
    std::sort(dirtyIndices.begin(), dirtyIndices.end(), [](const QPair<int, quint64> &a, const QPair<int, quint64> &b)
    {
        return a.second > b.second;
    });
    std::sort(staleIndices.begin(), staleIndices.end(), [](const QPair<int, quint64> &a, const QPair<int, quint64> &b)
    {
        return a.second < b.second;
    });

    auto sampleOne = [&](int idx)
    {
        Process &proc = processes[idx];
        const Process::Identity id = proc.GetIdentity();
        quint64 pssKb = 0;
        bool permissionDenied = false;

        if (Process::LoadPss(proc.PID, pssKb, &permissionDenied))
        {
            proc.MemoryPssKb = pssKb;
            proc.MemoryPssExact = true;

            PssCacheEntry entry;
            entry.PssKb = pssKb;
            entry.LastRssKb = proc.VMRssKb;
            entry.LastSharedKb = proc.SharedKb;
            entry.LastSampledTick = this->m_pssTick;
            entry.PermissionDenied = false;
            this->m_pssCache.insert(id, entry);
        } else if (permissionDenied)
        {
            proc.MemoryPssKb = proc.PrivateRssKb();
            proc.MemoryPssExact = false;

            PssCacheEntry entry;
            entry.LastRssKb = proc.VMRssKb;
            entry.LastSharedKb = proc.SharedKb;
            entry.LastSampledTick = this->m_pssTick;
            entry.PermissionDenied = true;
            this->m_pssCache.insert(id, entry);
        }
    };

    QElapsedTimer budgetTimer;
    budgetTimer.start();
    int sampledPriority = 0;

    for (int idx : std::as_const(uncachedIndices))
    {
        if (sampledPriority > 0 && budgetTimer.elapsed() >= PSS_UNCACHED_BUDGET_MS)
            break;
        sampleOne(idx);
        ++sampledPriority;
    }

    for (const QPair<int, quint64> &item : std::as_const(dirtyIndices))
    {
        if (sampledPriority > 0 && budgetTimer.elapsed() >= PSS_DIRTY_BUDGET_MS)
            break;
        sampleOne(item.first);
        ++sampledPriority;
    }

    int sampledStale = 0;
    for (const QPair<int, quint64> &item : std::as_const(staleIndices))
    {
        if (sampledStale >= PSS_MAX_STALE_PER_TICK || budgetTimer.elapsed() >= PSS_STALE_BUDGET_MS)
            break;
        sampleOne(item.first);
        ++sampledStale;
    }

    // Prune exited processes from the PSS cache.
    for (auto it = this->m_pssCache.begin(); it != this->m_pssCache.end(); )
    {
        if (!liveIdentities.contains(it.key()))
            it = this->m_pssCache.erase(it);
        else
            ++it;
    }
}

void ProcessRefreshWorker::flushIOMetrics()
{
    this->m_prevIoReadBytes.clear();
    this->m_prevIoWriteBytes.clear();
    this->m_prevDiskIoMs.clear();
    this->m_prevIoSampleTimer.invalidate();
}

void ProcessRefreshWorker::flushPssCache()
{
    this->m_pssCache.clear();
    this->m_pssTick = 0;
}

ProcessRefreshService::ProcessRefreshService(QObject *parent)
    : QObject(parent)
    , m_workerThread(new QThread(this))
    , m_worker(new ProcessRefreshWorker())
{
    qRegisterMetaType<OS::Process>("OS::Process");
    qRegisterMetaType<QList<OS::Process>>("QList<OS::Process>");

    this->m_worker->moveToThread(this->m_workerThread);
    connect(this->m_workerThread, &QThread::finished, this->m_worker, &QObject::deleteLater);
    connect(this,
            &ProcessRefreshService::requestRefresh,
            this->m_worker,
            &ProcessRefreshWorker::fetch,
            Qt::QueuedConnection);
    connect(this->m_worker,
            &ProcessRefreshWorker::fetched,
            this,
            &ProcessRefreshService::snapshotReady,
            Qt::QueuedConnection);
    this->m_workerThread->start();
}

ProcessRefreshService::~ProcessRefreshService()
{
    this->m_workerThread->quit();
    this->m_workerThread->wait(1000);
}

void ProcessRefreshService::RequestSnapshot(Consumer consumer, quint64 token, const Process::LoadOptions &options)
{
    emit requestRefresh(static_cast<int>(consumer),
                        token,
                        options.IncludeKernelTasks,
                        options.IncludeOtherUsers,
                        options.CollectIOMetrics,
                        options.CollectPssMetrics,
                        options.IsSuperuser,
                        static_cast<uint>(options.EffectiveUID),
                        static_cast<uint>(options.MyUID));
}
