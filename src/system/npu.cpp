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

#include "npu.h"
#include "../logger.h"
#include "../misc.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSysInfo>

NPU::NPU()
{
    this->Detect();
}

NPU::~NPU() = default;

const NPU::NPUInfo &NPU::FromIndex(int index) const
{
    if (index < 0 || index >= static_cast<int>(this->m_npus.size()))
        return this->m_nullNPU;
    return *this->m_npus.at(index);
}

void NPU::Detect()
{
    LOG_DEBUG("Detecting NPU modules");
    this->m_npus.clear();

    // 1. Scan /sys/class/accel for accelerator devices (kernel 6.3+ DRM accel subsystem)
    const QDir accelDir(QStringLiteral("/sys/class/accel"));
    if (accelDir.exists())
    {
        const QStringList entries = accelDir.entryList({QStringLiteral("accel*")}, QDir::Dirs | QDir::System);
        for (const QString &entry : entries)
        {
            const QString sysfsPath = accelDir.filePath(entry);
            const QString devPath = sysfsPath + QStringLiteral("/device");
            if (!QFileInfo::exists(devPath))
                continue;

            auto npu = std::make_unique<NPUInfo>();
            npu->ID = entry;
            npu->SysfsPath = sysfsPath;
            npu->DeviceNode = QStringLiteral("/dev/accel/") + entry;
            if (!QFileInfo::exists(npu->DeviceNode))
                npu->DeviceNode = QStringLiteral("/dev/") + entry;

            const QString canonicalDev = QFileInfo(devPath).canonicalFilePath();
            npu->PCIAddress = QFileInfo(canonicalDev).fileName();

            // Driver
            npu->Driver = Misc::FileNameFromSymlink(devPath + QStringLiteral("/driver/module"));
            if (npu->Driver.isEmpty())
                npu->Driver = Misc::FileNameFromSymlink(devPath + QStringLiteral("/driver"));
            if (npu->Driver.isEmpty())
            {
                const QString uevent = Misc::ReadFile(devPath + QStringLiteral("/uevent"));
                const QStringList lines = uevent.split(QLatin1Char('\n'));
                for (const QString &line : lines)
                {
                    if (line.startsWith(QLatin1String("DRIVER=")))
                        npu->Driver = line.mid(7).trimmed();
                }
            }

            // Driver Version
            if (!npu->Driver.isEmpty())
            {
                const QString ver = Misc::ReadFile(QStringLiteral("/sys/module/") + npu->Driver + QStringLiteral("/version")).trimmed();
                if (!ver.isEmpty())
                    npu->DriverVersion = ver;
            }
            if (npu->DriverVersion.isEmpty())
                npu->DriverVersion = QSysInfo::kernelVersion();

            // Vendor & Device IDs
            const QString vendorStr = Misc::ReadFile(devPath + QStringLiteral("/vendor")).trimmed().toLower();

            // Try resolving name via lspci
            if (!npu->PCIAddress.isEmpty())
            {
                QProcess proc;
                proc.start(QStringLiteral("lspci"), {QStringLiteral("-s"), npu->PCIAddress});
                if (proc.waitForFinished(1000))
                {
                    QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
                    if (!out.isEmpty())
                    {
                        // E.g. "00:0b.0 Processing accelerators: Intel Corporation Core Ultra 200H/200V Series Processors NPU (rev 04)"
                        int colon1 = out.indexOf(QLatin1Char(':'));
                        if (colon1 != -1)
                        {
                            QString rest = out.mid(colon1 + 1).trimmed();
                            int colon2 = rest.indexOf(QLatin1Char(':'));
                            if (colon2 != -1)
                                npu->Name = rest.mid(colon2 + 1).trimmed();
                            else
                                npu->Name = rest;
                        }
                        int revIdx = npu->Name.lastIndexOf(QStringLiteral("(rev"));
                        if (revIdx != -1)
                            npu->Name = npu->Name.left(revIdx).trimmed();
                    }
                }
            }

            // Fallback device name
            if (npu->Name.isEmpty())
            {
                if (vendorStr == QLatin1String("0x8086"))
                    npu->Name = QStringLiteral("Intel NPU");
                else if (vendorStr == QLatin1String("0x1022"))
                    npu->Name = QStringLiteral("AMD Ryzen AI NPU");
                else if (vendorStr == QLatin1String("0x17cb"))
                    npu->Name = QStringLiteral("Qualcomm Cloud AI");
                else if (vendorStr == QLatin1String("0x1da3"))
                    npu->Name = QStringLiteral("Habana Gaudi");
                else if (vendorStr == QLatin1String("0x1ac1"))
                    npu->Name = QStringLiteral("Hailo AI Processor");
                else if (!npu->Driver.isEmpty())
                    npu->Name = QStringLiteral("NPU (%1)").arg(npu->Driver);
                else
                    npu->Name = QStringLiteral("NPU Accelerator");
            }

            // Check metric paths
            const QString busyTimePath = devPath + QStringLiteral("/npu_busy_time_us");
            if (QFileInfo::exists(busyTimePath))
                npu->BusyTimePath = busyTimePath;

            const QString activeTimePath = devPath + QStringLiteral("/power/runtime_active_time");
            if (QFileInfo::exists(activeTimePath))
                npu->ActiveTimePath = activeTimePath;

            const QString curFreqPath = devPath + QStringLiteral("/npu_current_frequency_mhz");
            if (QFileInfo::exists(curFreqPath))
                npu->CurrentFreqPath = curFreqPath;
            else if (QFileInfo::exists(devPath + QStringLiteral("/freq/current_freq")))
                npu->CurrentFreqPath = devPath + QStringLiteral("/freq/current_freq");

            const QString maxFreqPath = devPath + QStringLiteral("/npu_max_frequency_mhz");
            if (QFileInfo::exists(maxFreqPath))
                npu->MaxFreqPath = maxFreqPath;
            else if (QFileInfo::exists(devPath + QStringLiteral("/freq/hw_max_freq")))
                npu->MaxFreqPath = devPath + QStringLiteral("/freq/hw_max_freq");

            const QString memPath = devPath + QStringLiteral("/npu_memory_utilization");
            if (QFileInfo::exists(memPath))
                npu->MemUsagePath = memPath;

            LOG_INFO(QStringLiteral("Detected NPU: %1 (%2) driver=%3 node=%4")
                     .arg(npu->ID, npu->Name, npu->Driver, npu->DeviceNode));

            this->m_npus.push_back(std::move(npu));
        }
    }

    // 2. Also check /sys/bus/pci/devices/ if no accel devices were found
    if (this->m_npus.empty())
    {
        const QDir pciDir(QStringLiteral("/sys/bus/pci/devices"));
        if (pciDir.exists())
        {
            const QStringList pciEntries = pciDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &bdf : pciEntries)
            {
                const QString devPath = pciDir.filePath(bdf);
                const QString classStr = Misc::ReadFile(devPath + QStringLiteral("/class")).trimmed();
                QString driver = Misc::FileNameFromSymlink(devPath + QStringLiteral("/driver/module"));
                if (driver.isEmpty())
                    driver = Misc::FileNameFromSymlink(devPath + QStringLiteral("/driver"));

                // PCI class 0x120000 = Processing Accelerators
                bool isAccelerator = classStr.startsWith(QLatin1String("0x12"), Qt::CaseInsensitive)
                                     || driver == QLatin1String("intel_vpu")
                                     || driver == QLatin1String("amdxdna")
                                     || driver == QLatin1String("qaic")
                                     || driver == QLatin1String("hailo_pci");
                if (!isAccelerator)
                    continue;

                auto npu = std::make_unique<NPUInfo>();
                npu->ID = bdf;
                npu->PCIAddress = bdf;
                npu->SysfsPath = devPath;
                npu->Driver = driver;

                if (!driver.isEmpty())
                {
                    const QString ver = Misc::ReadFile(QStringLiteral("/sys/module/") + driver + QStringLiteral("/version")).trimmed();
                    if (!ver.isEmpty())
                        npu->DriverVersion = ver;
                }
                if (npu->DriverVersion.isEmpty())
                    npu->DriverVersion = QSysInfo::kernelVersion();

                QProcess proc;
                proc.start(QStringLiteral("lspci"), {QStringLiteral("-s"), bdf});
                if (proc.waitForFinished(1000))
                {
                    QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
                    if (!out.isEmpty())
                    {
                        int colon1 = out.indexOf(QLatin1Char(':'));
                        if (colon1 != -1)
                        {
                            QString rest = out.mid(colon1 + 1).trimmed();
                            int colon2 = rest.indexOf(QLatin1Char(':'));
                            if (colon2 != -1)
                                npu->Name = rest.mid(colon2 + 1).trimmed();
                            else
                                npu->Name = rest;
                        }
                        int revIdx = npu->Name.lastIndexOf(QStringLiteral("(rev"));
                        if (revIdx != -1)
                            npu->Name = npu->Name.left(revIdx).trimmed();
                    }
                }
                if (npu->Name.isEmpty())
                    npu->Name = QStringLiteral("Processing Accelerator (%1)").arg(bdf);

                this->m_npus.push_back(std::move(npu));
            }
        }
    }
}

bool NPU::Sample()
{
    const qint64 currentNs = this->m_sampleTimerStarted ? this->m_sampleTimer.nsecsElapsed() : 0;

    for (auto &npu : this->m_npus)
    {
        const QString devPath = npu->SysfsPath.contains(QLatin1String("/accel"))
                                ? npu->SysfsPath + QStringLiteral("/device")
                                : npu->SysfsPath;

        // Power state
        QString pwr = Misc::ReadFile(devPath + QStringLiteral("/power_state")).trimmed();
        if (pwr.isEmpty())
            pwr = Misc::ReadFile(devPath + QStringLiteral("/power/runtime_status")).trimmed();
        npu->PowerState = pwr;

        // Frequencies
        if (!npu->CurrentFreqPath.isEmpty())
        {
            bool ok = false;
            int freq = Misc::ReadFile(npu->CurrentFreqPath).trimmed().toInt(&ok);
            if (ok)
                npu->CurrentFreqMHz = freq;
        }
        if (!npu->MaxFreqPath.isEmpty())
        {
            bool ok = false;
            int maxFreq = Misc::ReadFile(npu->MaxFreqPath).trimmed().toInt(&ok);
            if (ok)
                npu->MaxFreqMHz = maxFreq;
        }

        // Memory
        if (!npu->MemUsagePath.isEmpty())
        {
            bool ok = false;
            qint64 bytes = Misc::ReadFile(npu->MemUsagePath).trimmed().toLongLong(&ok);
            if (ok)
            {
                npu->MemUsedBytes = bytes;
                npu->MemHistory.Push(bytes / (1024.0 * 1024.0));
            }
        }

        // Calculate utilization %
        double util = 0.0;
        bool calculated = false;

        if (!npu->BusyTimePath.isEmpty())
        {
            bool ok = false;
            qint64 busyUs = Misc::ReadFile(npu->BusyTimePath).trimmed().toLongLong(&ok);
            if (ok)
            {
                if (npu->PrevBusyTimeUs >= 0 && npu->PrevSampleNs > 0)
                {
                    qint64 deltaBusyUs = busyUs - npu->PrevBusyTimeUs;
                    qint64 elapsedUs = (currentNs - npu->PrevSampleNs) / 1000;
                    if (elapsedUs > 0 && deltaBusyUs >= 0)
                    {
                        util = (static_cast<double>(deltaBusyUs) * 100.0) / static_cast<double>(elapsedUs);
                        calculated = true;
                    }
                }
                npu->PrevBusyTimeUs = busyUs;
            }
        }

        if (!calculated && !npu->ActiveTimePath.isEmpty())
        {
            bool ok = false;
            qint64 activeMs = Misc::ReadFile(npu->ActiveTimePath).trimmed().toLongLong(&ok);
            if (ok)
            {
                if (npu->PrevActiveTimeMs >= 0 && npu->PrevSampleNs > 0)
                {
                    qint64 deltaActiveMs = activeMs - npu->PrevActiveTimeMs;
                    qint64 elapsedMs = (currentNs - npu->PrevSampleNs) / 1000000;
                    if (elapsedMs > 0 && deltaActiveMs >= 0)
                    {
                        util = (static_cast<double>(deltaActiveMs) * 100.0) / static_cast<double>(elapsedMs);
                        calculated = true;
                    }
                }
                npu->PrevActiveTimeMs = activeMs;
            }
        }

        if (npu->PowerState.compare(QLatin1String("D3hot"), Qt::CaseInsensitive) == 0 ||
            npu->PowerState.compare(QLatin1String("suspended"), Qt::CaseInsensitive) == 0)
        {
            if (!calculated || util < 0.01)
            {
                util = 0.0;
                npu->CurrentFreqMHz = 0;
            }
        }

        npu->UtilPct = qBound(0.0, util, 100.0);
        npu->UtilHistory.Push(npu->UtilPct);
        npu->PrevSampleNs = currentNs;
    }

    this->m_sampleTimer.start();
    this->m_sampleTimerStarted = true;
    return !this->m_npus.empty();
}
