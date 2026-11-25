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

#ifndef OS_PROCESSREFRESHSERVICE_H
#define OS_PROCESSREFRESHSERVICE_H

#include "proc.h"
#include "process.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QThread>

// This provides a mechanism to refresh data from /proc using dedicated thread, so that we don't
// cause random UI stutters when in process tab by blocking UI thread for too long

namespace OS
{
    class ProcessRefreshWorker : public QObject
    {
        Q_OBJECT

        public:
            explicit ProcessRefreshWorker(QObject *parent = nullptr);

        public slots:
            void fetch(int consumer,
                       quint64 token,
                       bool includeKernelTasks,
                       bool includeOtherUsers,
                       bool collectIOMetrics,
                       bool collectPssMetrics,
                       bool isSuperuser,
                       uint effectiveUid,
                       uint myUid);

        signals:
            void fetched(int consumer, quint64 token, const QList<Process> &processes, int totalTasks, qlonglong totalThreads);

        private:
            struct PssCacheEntry
            {
                quint64 PssKb           { 0 };
                quint64 LastRssKb       { 0 };
                quint64 LastSharedKb    { 0 };
                quint64 LastSampledTick { 0 };
                bool    PermissionDenied{ false };
            };

            struct CpuSampleEntry
            {
                quint64 Ticks          { 0 };
                double  LastCpuPercent { 0.0 };
            };

            struct CpuConsumerState
            {
                QHash<Process::Identity, CpuSampleEntry> PrevSamples;
                quint64                                  PrevTotalTicks { 0 };
            };

            struct IoSampleEntry
            {
                quint64 ReadBytes           { 0 };
                quint64 WriteBytes          { 0 };
                quint64 CancelledWriteBytes { 0 };
                double  LastReadBps         { 0.0 };
                double  LastWriteBps        { 0.0 };
                double  LastDiskPercent     { 0.0 };
                bool    HasRates            { false };
            };

            QHash<int, CpuConsumerState>            m_cpuStateByConsumer;
            QHash<Process::Identity, IoSampleEntry> m_prevIoSamples;
            QHash<QString, Proc::DiskIoSample>      m_prevDiskStats;
            QHash<Process::Identity, PssCacheEntry> m_pssCache;
            quint64                                 m_pssTick { 0 };
            QElapsedTimer                           m_prevIoSampleTimer;

            void updateCpuMetrics(int consumer, QList<Process> &processes, quint64 totalJiffies);
            void updateIoMetrics(QList<Process> &processes, const QHash<QString, Proc::DiskIoSample> &currentDiskStats);
            void updatePssMetrics(QList<Process> &processes, bool isSuperuser, uid_t effectiveUid);
            void flushIOMetrics();
            void flushPssCache();
    };

    class ProcessRefreshService : public QObject
    {
        Q_OBJECT

        public:
            enum class Consumer
            {
                Processes = 0,
                Users = 1
            };

            explicit ProcessRefreshService(QObject *parent = nullptr);
            ~ProcessRefreshService() override;

            void RequestSnapshot(Consumer consumer, quint64 token, const Process::LoadOptions &options);

        signals:
            void requestRefresh(int consumer,
                                quint64 token,
                                bool includeKernelTasks,
                                bool includeOtherUsers,
                                bool collectIOMetrics,
                                bool collectPssMetrics,
                                bool isSuperuser,
                                uint effectiveUid,
                                uint myUid);
            void snapshotReady(int consumer, quint64 token, const QList<Process> &processes, int totalTasks, qlonglong totalThreads);

        private:
            QThread              *m_workerThread { nullptr };
            ProcessRefreshWorker *m_worker { nullptr };
    };
}

#endif // OS_PROCESSREFRESHSERVICE_H
