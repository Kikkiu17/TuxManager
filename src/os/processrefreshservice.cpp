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
#include <utility>

using namespace OS;

ProcessRefreshWorker::ProcessRefreshWorker(QObject *parent) : QObject(parent)
{
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

    Process::LoadOptions opts;
    opts.IncludeKernelTasks = includeKernelTasks;
    opts.IncludeOtherUsers = includeOtherUsers;
    opts.MyUID = static_cast<uid_t>(myUid);
    opts.CollectIOMetrics = collectIOMetrics;
    opts.CollectPssMetrics = false; // Sampled incrementally via delta-cached updatePssMetrics()
    opts.IsSuperuser = isSuperuser;
    opts.EffectiveUID = static_cast<uid_t>(effectiveUid);

    Process::Counts totals;
    QList<Process> fresh = Process::LoadAll(opts, &totals);

    const bool isProcessConsumer = (consumer == static_cast<int>(ProcessRefreshService::Consumer::Processes));
    const bool handleIo = collectIOMetrics && isProcessConsumer;

    // Sample diskstats and I/O elapsed timer adjacent to Process::LoadAll() and
    // before updatePssMetrics() so PSS budget sampling never skews ioElapsedMs.
    if (handleIo)
    {
        const QHash<QString, Proc::DiskIoSample> currentDiskStats = Proc::ReadDiskIoStats();
        this->updateIoMetrics(fresh, currentDiskStats);
    }
    else if (isProcessConsumer)
    {
        this->flushIOMetrics();
    }

    this->updateCpuMetrics(consumer, fresh, totalJiffies);

    if (collectPssMetrics)
    {
        this->updatePssMetrics(fresh, isSuperuser, static_cast<uid_t>(effectiveUid));
    }
    else if (isProcessConsumer)
    {
        this->flushPssCache();
    }

    emit this->fetched(consumer, token, fresh, totals.Tasks, totals.Threads);
}

void ProcessRefreshWorker::updateCpuMetrics(int consumer, QList<Process> &processes, quint64 totalJiffies)
{
    CpuConsumerState &state = this->m_cpuStateByConsumer[consumer];
    const quint64 periodJiffies =
        (state.PrevTotalTicks > 0 && totalJiffies > state.PrevTotalTicks)
        ? (totalJiffies - state.PrevTotalTicks)
        : 0ULL;
    const bool hasPrevState = (state.PrevTotalTicks > 0 && !state.PrevSamples.isEmpty());

    if (periodJiffies > 0)
    {
        const double totalPeriod = static_cast<double>(periodJiffies);
        QHash<Process::Identity, CpuSampleEntry> nextSamples;
        nextSamples.reserve(processes.size());

        for (Process &proc : processes)
        {
            const Process::Identity id = proc.GetIdentity();
            double cpuPct = 0.0;

            const auto it = state.PrevSamples.constFind(id);
            if (it != state.PrevSamples.cend() && proc.CPUTicks >= it.value().Ticks)
            {
                const double pct = static_cast<double>(proc.CPUTicks - it.value().Ticks) / totalPeriod * 100.0;
                // Cap at 100% of the entire CPU
                cpuPct = qMin(pct, 100.0);
            }

            proc.CPUPercent = cpuPct;
            nextSamples.insert(id, { proc.CPUTicks, cpuPct });
        }

        state.PrevSamples = std::move(nextSamples);
        state.PrevTotalTicks = totalJiffies;
        return;
    }

    if (hasPrevState)
    {
        for (Process &proc : processes)
        {
            const auto it = state.PrevSamples.constFind(proc.GetIdentity());
            if (it != state.PrevSamples.cend())
                proc.CPUPercent = it.value().LastCpuPercent;
        }
        return;
    }

    state.PrevSamples.clear();
    state.PrevSamples.reserve(processes.size());
    for (const Process &proc : std::as_const(processes))
        state.PrevSamples.insert(proc.GetIdentity(), { proc.CPUTicks, 0.0 });
    state.PrevTotalTicks = totalJiffies;
}

void ProcessRefreshWorker::updateIoMetrics(QList<Process> &processes, const QHash<QString, Proc::DiskIoSample> &currentDiskStats)
{
    static constexpr qint64 MIN_IO_SAMPLE_MS = 50;

    const bool hasPrevIo = !this->m_prevIoSamples.isEmpty();
    const qint64 elapsedCheckMs = this->m_prevIoSampleTimer.isValid()
                                      ? this->m_prevIoSampleTimer.elapsed()
                                      : -1;

    if (elapsedCheckMs >= 0 && elapsedCheckMs < MIN_IO_SAMPLE_MS && hasPrevIo)
    {
        for (Process &proc : processes)
        {
            if (!proc.IOTotalsAvailable)
                continue;

            const auto it = this->m_prevIoSamples.constFind(proc.GetIdentity());
            if (it == this->m_prevIoSamples.cend())
                continue;

            proc.IOReadBps = it.value().LastReadBps;
            proc.IOWriteBps = it.value().LastWriteBps;
            proc.DiskPercent = it.value().LastDiskPercent;
            proc.IORatesAvailable = it.value().HasRates;
        }
        return;
    }

    qint64 ioElapsedMs = -1;
    if (this->m_prevIoSampleTimer.isValid())
        ioElapsedMs = this->m_prevIoSampleTimer.restart();
    else
        this->m_prevIoSampleTimer.start();

    if (ioElapsedMs >= MIN_IO_SAMPLE_MS && hasPrevIo)
    {
        const double elapsedSec = static_cast<double>(ioElapsedMs) / 1000.0;
        double totalProcReadBps = 0.0;
        double totalProcWriteBps = 0.0;

        for (Process &proc : processes)
        {
            if (!proc.IOTotalsAvailable)
                continue;

            const auto it = this->m_prevIoSamples.constFind(proc.GetIdentity());
            if (it == this->m_prevIoSamples.cend())
                continue;

            const IoSampleEntry &prev = it.value();
            if (proc.IOReadBytes < prev.ReadBytes || proc.IOWriteBytes < prev.WriteBytes)
                continue;

            const quint64 dRead = proc.IOReadBytes - prev.ReadBytes;
            const quint64 dWriteRaw = proc.IOWriteBytes - prev.WriteBytes;
            const quint64 dCancelled = (proc.IOCancelledWriteBytes >= prev.CancelledWriteBytes)
                                           ? (proc.IOCancelledWriteBytes - prev.CancelledWriteBytes)
                                           : 0ULL;
            const quint64 dWriteNet = (dWriteRaw > dCancelled) ? (dWriteRaw - dCancelled) : 0ULL;

            proc.IOReadBps = static_cast<double>(dRead) / elapsedSec;
            proc.IOWriteBps = static_cast<double>(dWriteNet) / elapsedSec;
            proc.IORatesAvailable = true;

            totalProcReadBps += proc.IOReadBps;
            totalProcWriteBps += proc.IOWriteBps;
        }

        if (currentDiskStats.isEmpty() || this->m_prevDiskStats.isEmpty())
        {
            for (Process &proc : processes)
            {
                if (proc.IORatesAvailable)
                    proc.DiskPercent = -1.0;
            }
        }
        else
        {
            double totalDiskReadBps = 0.0;
            double totalDiskWriteBps = 0.0;
            double weightedDiskReadActiveSum = 0.0;
            double weightedDiskWriteActiveSum = 0.0;

            for (auto it = currentDiskStats.cbegin(); it != currentDiskStats.cend(); ++it)
            {
                const auto prevIt = this->m_prevDiskStats.constFind(it.key());
                if (prevIt == this->m_prevDiskStats.cend())
                    continue;

                const Proc::DiskIoSample &cur = it.value();
                const Proc::DiskIoSample &prev = prevIt.value();
                if (cur.IoMs < prev.IoMs
                    || cur.ReadBytes < prev.ReadBytes
                    || cur.WriteBytes < prev.WriteBytes
                    || cur.ReadMs < prev.ReadMs
                    || cur.WriteMs < prev.WriteMs
                    || cur.DiscardMs < prev.DiscardMs
                    || cur.FlushMs < prev.FlushMs)
                {
                    continue;
                }

                const quint64 dIoMs = cur.IoMs - prev.IoMs;
                const quint64 dReadBytes = cur.ReadBytes - prev.ReadBytes;
                const quint64 dWriteBytes = cur.WriteBytes - prev.WriteBytes;
                const quint64 dReadMs = cur.ReadMs - prev.ReadMs;
                const quint64 dWriteMs = cur.WriteMs - prev.WriteMs;
                const quint64 dDiscardMs = cur.DiscardMs - prev.DiscardMs;
                const quint64 dFlushMs = cur.FlushMs - prev.FlushMs;

                const double diskActivePct = qBound(0.0,
                                                    static_cast<double>(dIoMs) * 100.0 / static_cast<double>(ioElapsedMs),
                                                    100.0);
                const double diskReadBps = static_cast<double>(dReadBytes) / elapsedSec;
                const double diskWriteBps = static_cast<double>(dWriteBytes) / elapsedSec;

                double diskReadActivePct = 0.0;
                double diskWriteActivePct = 0.0;
                const quint64 totalOpMs = dReadMs + dWriteMs + dDiscardMs + dFlushMs;
                if (totalOpMs > 0)
                {
                    diskReadActivePct = (dReadBytes > 0)
                                            ? (diskActivePct * static_cast<double>(dReadMs) / static_cast<double>(totalOpMs))
                                            : 0.0;
                    diskWriteActivePct = (dWriteBytes > 0)
                                             ? (diskActivePct * static_cast<double>(dWriteMs) / static_cast<double>(totalOpMs))
                                             : 0.0;
                }
                else if (dReadBytes + dWriteBytes > 0)
                {
                    const double totalDiskBytes = static_cast<double>(dReadBytes + dWriteBytes);
                    diskReadActivePct = diskActivePct * static_cast<double>(dReadBytes) / totalDiskBytes;
                    diskWriteActivePct = diskActivePct * static_cast<double>(dWriteBytes) / totalDiskBytes;
                }

                totalDiskReadBps += diskReadBps;
                totalDiskWriteBps += diskWriteBps;
                weightedDiskReadActiveSum += diskReadBps * diskReadActivePct;
                weightedDiskWriteActiveSum += diskWriteBps * diskWriteActivePct;
            }

            const double effectiveReadActivePct =
                (totalDiskReadBps > 0.0) ? (weightedDiskReadActiveSum / totalDiskReadBps) : 0.0;
            const double effectiveWriteActivePct =
                (totalDiskWriteBps > 0.0) ? (weightedDiskWriteActiveSum / totalDiskWriteBps) : 0.0;

            // Bound denominators by physical disk throughput so unobserved root/kernel/writeback I/O
            // never inflates visible user-process Disk % shares.
            const double denomReadBps = qMax(totalProcReadBps, totalDiskReadBps);
            const double denomWriteBps = qMax(totalProcWriteBps, totalDiskWriteBps);

            for (Process &proc : processes)
            {
                if (!proc.IORatesAvailable)
                    continue;

                const double readPct =
                    (denomReadBps > 0.0 && effectiveReadActivePct > 0.0)
                        ? ((proc.IOReadBps / denomReadBps) * effectiveReadActivePct)
                        : 0.0;
                const double writePct =
                    (denomWriteBps > 0.0 && effectiveWriteActivePct > 0.0)
                        ? ((proc.IOWriteBps / denomWriteBps) * effectiveWriteActivePct)
                        : 0.0;
                proc.DiskPercent = qBound(0.0, readPct + writePct, 100.0);
            }
        }
    }

    QHash<Process::Identity, IoSampleEntry> nextIoSamples;
    nextIoSamples.reserve(processes.size());
    for (const Process &proc : std::as_const(processes))
    {
        if (!proc.IOTotalsAvailable)
            continue;

        IoSampleEntry entry;
        entry.ReadBytes = proc.IOReadBytes;
        entry.WriteBytes = proc.IOWriteBytes;
        entry.CancelledWriteBytes = proc.IOCancelledWriteBytes;
        entry.LastReadBps = proc.IOReadBps;
        entry.LastWriteBps = proc.IOWriteBps;
        entry.LastDiskPercent = proc.DiskPercent;
        entry.HasRates = proc.IORatesAvailable;
        nextIoSamples.insert(proc.GetIdentity(), entry);
    }

    this->m_prevIoSamples = std::move(nextIoSamples);
    this->m_prevDiskStats = currentDiskStats;
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
    this->m_prevIoSamples.clear();
    this->m_prevDiskStats.clear();
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
