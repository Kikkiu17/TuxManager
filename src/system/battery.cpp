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

#include "battery.h"
#include "../logger.h"
#include "../metrics.h"
#include "../misc.h"

#include <QDateTime>
#include <QDir>
#include <QtMath>

Battery::Battery()
{
    this->Detect();
}

Battery::~Battery() = default;

const Battery::BatteryInfo &Battery::FromIndex(int index) const
{
    if (index < 0 || index >= static_cast<int>(this->m_batteries.size()))
        return this->m_nullBattery;
    return *this->m_batteries.at(index);
}

void Battery::Detect()
{
    LOG_DEBUG("Detecting Battery modules");
    this->m_batteries.clear();
    this->m_psysEnergyPath.clear();
    this->m_pkgEnergyPath.clear();
    this->m_psysMaxRangeUj = 0;
    this->m_pkgMaxRangeUj = 0;
    this->m_lastPsysEnergyUj = -1;
    this->m_lastPkgEnergyUj = -1;
    this->m_lastSampleTimeMs = 0;

    // Detect powercap RAPL zones for platform/CPU power
    const QDir powercapDir(QStringLiteral("/sys/class/powercap"));
    if (powercapDir.exists())
    {
        const QStringList entries = powercapDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &entry : entries)
        {
            const QString zonePath = powercapDir.filePath(entry);
            const QString name = Misc::ReadFile(zonePath + QStringLiteral("/name")).trimmed();
            const QString energyPath = zonePath + QStringLiteral("/energy_uj");
            if (!QFile::exists(energyPath))
                continue;

            if (name == QLatin1String("psys") && this->m_psysEnergyPath.isEmpty())
            {
                this->m_psysEnergyPath = energyPath;
                bool ok = false;
                this->m_psysMaxRangeUj = Misc::ReadFile(zonePath + QStringLiteral("/max_energy_range_uj")).trimmed().toLongLong(&ok);
                LOG_DEBUG(QStringLiteral("Detected PSYS RAPL energy at %1 (max: %2 uJ)").arg(energyPath).arg(this->m_psysMaxRangeUj));
            }
            else if (name == QLatin1String("package-0") && this->m_pkgEnergyPath.isEmpty())
            {
                this->m_pkgEnergyPath = energyPath;
                bool ok = false;
                this->m_pkgMaxRangeUj = Misc::ReadFile(zonePath + QStringLiteral("/max_energy_range_uj")).trimmed().toLongLong(&ok);
                LOG_DEBUG(QStringLiteral("Detected Package RAPL energy at %1 (max: %2 uJ)").arg(energyPath).arg(this->m_pkgMaxRangeUj));
            }
        }
    }

    const QDir powerSupplyDir(QStringLiteral("/sys/class/power_supply"));
    if (!powerSupplyDir.exists())
        return;

    const QStringList entries = powerSupplyDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries)
    {
        const QString sysfsPath = powerSupplyDir.filePath(entry);
        const QString type = Misc::ReadFile(sysfsPath + QStringLiteral("/type")).trimmed();
        if (type != QLatin1String("Battery"))
            continue;

        const QString presentStr = Misc::ReadFile(sysfsPath + QStringLiteral("/present")).trimmed();
        if (!presentStr.isEmpty() && presentStr == QLatin1String("0"))
            continue;

        auto bat = std::make_unique<BatteryInfo>();
        bat->ID = entry;
        bat->Name = entry;
        bat->SysfsPath = sysfsPath;
        bat->Manufacturer = Misc::ReadFile(sysfsPath + QStringLiteral("/manufacturer")).trimmed();
        bat->ModelName = Misc::ReadFile(sysfsPath + QStringLiteral("/model_name")).trimmed();
        bat->SerialNumber = Misc::ReadFile(sysfsPath + QStringLiteral("/serial_number")).trimmed();
        bat->Technology = Misc::ReadFile(sysfsPath + QStringLiteral("/technology")).trimmed();

        LOG_INFO(QStringLiteral("Detected Battery: %1 (%2 %3)")
                     .arg(bat->ID, bat->Manufacturer, bat->ModelName));
        this->m_batteries.push_back(std::move(bat));
    }
}

bool Battery::Sample()
{
    if (this->m_batteries.empty())
        return false;

    // Sample platform / CPU / GPU power metrics
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const double dtSec = (this->m_lastSampleTimeMs > 0) ? (nowMs - this->m_lastSampleTimeMs) / 1000.0 : 0.0;
    this->m_lastSampleTimeMs = nowMs;

    double psysW = 0.0;
    if (!this->m_psysEnergyPath.isEmpty())
    {
        bool ok = false;
        const qint64 curEnergy = Misc::ReadFile(this->m_psysEnergyPath).trimmed().toLongLong(&ok);
        if (ok && curEnergy >= 0)
        {
            if (this->m_lastPsysEnergyUj >= 0 && dtSec > 0.1 && dtSec < 10.0)
            {
                qint64 delta = curEnergy - this->m_lastPsysEnergyUj;
                if (delta < 0 && this->m_psysMaxRangeUj > 0)
                    delta += this->m_psysMaxRangeUj;
                if (delta >= 0)
                    psysW = (static_cast<double>(delta) / 1000000.0) / dtSec;
            }
            this->m_lastPsysEnergyUj = curEnergy;
        }
    }

    double pkgW = 0.0;
    if (!this->m_pkgEnergyPath.isEmpty())
    {
        bool ok = false;
        const qint64 curEnergy = Misc::ReadFile(this->m_pkgEnergyPath).trimmed().toLongLong(&ok);
        if (ok && curEnergy >= 0)
        {
            if (this->m_lastPkgEnergyUj >= 0 && dtSec > 0.1 && dtSec < 10.0)
            {
                qint64 delta = curEnergy - this->m_lastPkgEnergyUj;
                if (delta < 0 && this->m_pkgMaxRangeUj > 0)
                    delta += this->m_pkgMaxRangeUj;
                if (delta >= 0)
                    pkgW = (static_cast<double>(delta) / 1000000.0) / dtSec;
            }
            this->m_lastPkgEnergyUj = curEnergy;
        }
    }

    double gpuW = 0.0;
    if (GPU *gpuSubsystem = Metrics::GetGPU())
    {
        for (int i = 0; i < gpuSubsystem->GpuCount(); ++i)
        {
            const auto &g = gpuSubsystem->FromIndex(i);
            if (g.PowerUsageW > 0.0)
                gpuW += g.PowerUsageW;
        }
    }

    double systemPowerW = 0.0;
    if (psysW > 0.0)
    {
        systemPowerW = psysW;
        if ((pkgW + gpuW) > psysW * 1.05)
            systemPowerW = pkgW + gpuW;
    }
    else if (pkgW > 0.0 || gpuW > 0.0)
    {
        systemPowerW = pkgW + gpuW;
    }

    for (auto &bat : this->m_batteries)
    {
        const QString path = bat->SysfsPath;

        // Present
        const QString presentStr = Misc::ReadFile(path + QStringLiteral("/present")).trimmed();
        if (!presentStr.isEmpty())
            bat->Present = (presentStr != QLatin1String("0"));

        if (!bat->Present)
        {
            bat->CapacityPct = 0.0;
            bat->PowerDrawW = 0.0;
            bat->PowerRateW = 0.0;
            bat->TotalPowerW = systemPowerW;
            bat->PlatformPowerW = systemPowerW;
            bat->GpuPowerW = gpuW;
            bat->CapacityHistory.Push(0.0);
            bat->PowerDrawHistory.Push(0.0);
            continue;
        }

        // Status
        bat->StatusStr = Misc::ReadFile(path + QStringLiteral("/status")).trimmed();
        if (bat->StatusStr.compare(QLatin1String("Charging"), Qt::CaseInsensitive) == 0)
            bat->Status = State::Charging;
        else if (bat->StatusStr.compare(QLatin1String("Discharging"), Qt::CaseInsensitive) == 0)
            bat->Status = State::Discharging;
        else if (bat->StatusStr.compare(QLatin1String("Full"), Qt::CaseInsensitive) == 0)
            bat->Status = State::Full;
        else if (bat->StatusStr.compare(QLatin1String("Not charging"), Qt::CaseInsensitive) == 0)
            bat->Status = State::NotCharging;
        else
            bat->Status = State::Unknown;

        // Capacity (%)
        bool okCap = false;
        double cap = Misc::ReadFile(path + QStringLiteral("/capacity")).trimmed().toDouble(&okCap);
        if (okCap)
            bat->CapacityPct = qBound(0.0, cap, 100.0);

        // Voltage (Volts)
        bool okVolt = false;
        double voltUv = Misc::ReadFile(path + QStringLiteral("/voltage_now")).trimmed().toDouble(&okVolt);
        if (!okVolt || voltUv <= 0.0)
            voltUv = Misc::ReadFile(path + QStringLiteral("/voltage_min_design")).trimmed().toDouble(&okVolt);
        bat->VoltageV = okVolt ? (voltUv / 1000000.0) : 0.0;

        // Power Draw (Watts)
        double rateW = 0.0;
        bool okPwr = false;
        double pwrUw = Misc::ReadFile(path + QStringLiteral("/power_now")).trimmed().toDouble(&okPwr);
        if (okPwr && pwrUw >= 0.0)
        {
            rateW = pwrUw / 1000000.0;
        }
        else
        {
            bool okCur = false;
            double curUa = Misc::ReadFile(path + QStringLiteral("/current_now")).trimmed().toDouble(&okCur);
            if (okCur && curUa >= 0.0 && bat->VoltageV > 0.0)
                rateW = (curUa / 1000000.0) * bat->VoltageV;
        }

        bat->PowerRateW = rateW;
        if (bat->Status == State::Discharging)
            bat->PowerDrawW = -rateW;
        else if (bat->Status == State::Charging)
            bat->PowerDrawW = +rateW;
        else
            bat->PowerDrawW = 0.0;

        bat->PlatformPowerW = systemPowerW;
        bat->GpuPowerW = gpuW;

        if (bat->Status == State::Discharging)
        {
            // Discharging on battery: battery is the power source supplying the entire PC
            bat->TotalPowerW = (bat->PowerRateW > 0.05) ? bat->PowerRateW : systemPowerW;
        }
        else if (bat->Status == State::Charging)
        {
            // Charging on AC: total power absorbed from AC adapter = battery charging power + system power
            bat->TotalPowerW = bat->PowerRateW + systemPowerW;
        }
        else
        {
            // Full or not charging on AC power
            bat->TotalPowerW = systemPowerW;
        }

        // Energy (Wh)
        bool okEnergyNow = false;
        double energyNowUwh = Misc::ReadFile(path + QStringLiteral("/energy_now")).trimmed().toDouble(&okEnergyNow);
        bool okEnergyFull = false;
        double energyFullUwh = Misc::ReadFile(path + QStringLiteral("/energy_full")).trimmed().toDouble(&okEnergyFull);
        bool okEnergyFullDesign = false;
        double energyFullDesignUwh = Misc::ReadFile(path + QStringLiteral("/energy_full_design")).trimmed().toDouble(&okEnergyFullDesign);

        if (okEnergyNow)
            bat->EnergyNowWh = energyNowUwh / 1000000.0;
        if (okEnergyFull)
            bat->EnergyFullWh = energyFullUwh / 1000000.0;
        if (okEnergyFullDesign)
            bat->EnergyFullDesignWh = energyFullDesignUwh / 1000000.0;

        // Fallback to charge_* if energy_* not available
        if (!okEnergyNow)
        {
            bool okChargeNow = false;
            double chargeNowUah = Misc::ReadFile(path + QStringLiteral("/charge_now")).trimmed().toDouble(&okChargeNow);
            bool okChargeFull = false;
            double chargeFullUah = Misc::ReadFile(path + QStringLiteral("/charge_full")).trimmed().toDouble(&okChargeFull);
            bool okChargeFullDesign = false;
            double chargeFullDesignUah = Misc::ReadFile(path + QStringLiteral("/charge_full_design")).trimmed().toDouble(&okChargeFullDesign);

            const double nominalV = bat->VoltageV > 0.0 ? bat->VoltageV : 11.1;
            if (okChargeNow)
                bat->EnergyNowWh = (chargeNowUah / 1000000.0) * nominalV;
            if (okChargeFull)
                bat->EnergyFullWh = (chargeFullUah / 1000000.0) * nominalV;
            if (okChargeFullDesign)
                bat->EnergyFullDesignWh = (chargeFullDesignUah / 1000000.0) * nominalV;
        }

        // Health (%)
        if (bat->EnergyFullDesignWh > 0.0 && bat->EnergyFullWh > 0.0)
            bat->HealthPct = qBound(0.0, (bat->EnergyFullWh / bat->EnergyFullDesignWh) * 100.0, 100.0);
        else
            bat->HealthPct = -1.0;

        // Cycle count
        bool okCycles = false;
        int cycles = Misc::ReadFile(path + QStringLiteral("/cycle_count")).trimmed().toInt(&okCycles);
        bat->CycleCount = okCycles ? cycles : -1;

        // Time estimation
        bat->TimeToEmptySec = -1;
        bat->TimeToFullSec = -1;

        bool okTimeEmpty = false;
        qint64 sysTimeEmpty = Misc::ReadFile(path + QStringLiteral("/time_to_empty_now-seconds")).trimmed().toLongLong(&okTimeEmpty);
        if (!okTimeEmpty)
            sysTimeEmpty = Misc::ReadFile(path + QStringLiteral("/time_to_empty_now")).trimmed().toLongLong(&okTimeEmpty);
        if (okTimeEmpty && sysTimeEmpty > 0)
            bat->TimeToEmptySec = sysTimeEmpty;

        bool okTimeFull = false;
        qint64 sysTimeFull = Misc::ReadFile(path + QStringLiteral("/time_to_full_now-seconds")).trimmed().toLongLong(&okTimeFull);
        if (!okTimeFull)
            sysTimeFull = Misc::ReadFile(path + QStringLiteral("/time_to_full_now")).trimmed().toLongLong(&okTimeFull);
        if (okTimeFull && sysTimeFull > 0)
            bat->TimeToFullSec = sysTimeFull;

        // Computed fallbacks if kernel doesn't report time
        if (bat->TimeToEmptySec <= 0 && bat->Status == State::Discharging && bat->PowerRateW > 0.1 && bat->EnergyNowWh > 0.0)
            bat->TimeToEmptySec = static_cast<qint64>((bat->EnergyNowWh / bat->PowerRateW) * 3600.0);

        if (bat->TimeToFullSec <= 0 && bat->Status == State::Charging && bat->PowerRateW > 0.1 && bat->EnergyFullWh > bat->EnergyNowWh)
            bat->TimeToFullSec = static_cast<qint64>(((bat->EnergyFullWh - bat->EnergyNowWh) / bat->PowerRateW) * 3600.0);

        // Update histories
        bat->CapacityHistory.Push(bat->CapacityPct);
        bat->PowerDrawHistory.Push(bat->PowerRateW);
    }

    return true;
}
