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

#ifndef GPUDRMBACKEND_H
#define GPUDRMBACKEND_H

#include "../gpu.h"

#include <QElapsedTimer>
#include <QPair>
#include <QSet>
#include <QVector>
#include <memory>
#include <vector>

class GpuDrmBackend
{
    public:
        void Detect(bool skipNvidia, bool skipAmd, const QSet<QString> &claimedIntelBdfs, bool skipIntel = false);
        void Detect(bool skipNvidia, bool skipAmd, bool skipIntel);
        bool Sample(std::vector<std::unique_ptr<GPU::GPUInfo>> &gpus);
        bool HasCards() const { return !this->m_cards.isEmpty(); }

    private:
        enum class SamplerKind
        {
            Amd,
            Intel
        };

        struct DRMCard
        {
            QString ID;
            QString Name;
            QString Vendor;
            QString DriverName;
            QString DriverVersion;
            SamplerKind Sampler { SamplerKind::Amd };
            QString BusyPath;
            QString VramTotalPath;
            QString VramUsedPath;
            QString GttTotalPath;
            QString GttUsedPath;
            QString TempPath;
            QString ActFreqPath;
            QString CurFreqPath;
            QString RenderNodePath;
            QString CardNodePath;
            QVector<QPair<QString, QString>> EngineBusyPaths;
            QStringList CachedFDInfoPaths;
        };

        bool sampleAmdCard(DRMCard &card, GPU::GPUInfo &gpu, qint64 fdInfoElapsedNs);
        bool sampleIntelCard(DRMCard &card, GPU::GPUInfo &gpu, qint64 fdInfoElapsedNs);
        QHash<QString, qint64> scanFdInfoEngines(DRMCard &card);

        QVector<DRMCard>   m_cards;
        QElapsedTimer      m_fdInfoTimer;
        bool               m_fdInfoTimerStarted { false };
        int                m_fdInfoRescanCounter { 0 };
};

#endif // GPUDRMBACKEND_H
