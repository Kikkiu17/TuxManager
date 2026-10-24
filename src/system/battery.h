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

#ifndef BATTERY_H
#define BATTERY_H

#include "../globals.h"
#include "../historybuffer.h"

#include <QString>
#include <memory>
#include <vector>

class Battery
{
    public:
        enum class State
        {
            Unknown,
            Charging,
            Discharging,
            Full,
            NotCharging
        };

        struct BatteryInfo
        {
            QString         ID;                     ///< e.g. "BAT0"
            QString         Name;                   ///< Display name e.g. "BAT0"
            QString         SysfsPath;              ///< e.g. "/sys/class/power_supply/BAT0"
            QString         Manufacturer;           ///< e.g. "SMP"
            QString         ModelName;              ///< e.g. "5B11H56399"
            QString         SerialNumber;           ///< e.g. "1516"
            QString         Technology;             ///< e.g. "Li-poly"
            QString         StatusStr;              ///< e.g. "Charging", "Discharging", "Full"
            State           Status { State::Unknown };

            bool            Present { true };
            double          CapacityPct { 0.0 };          ///< Current charge percentage (0.0..100.0)
            double          PowerDrawW { 0.0 };           ///< Signed power draw (+ for charging, - for discharging)
            double          PowerRateW { 0.0 };           ///< Absolute power rate in Watts (magnitude)
            double          TotalPowerW { 0.0 };          ///< Total power absorbed by the PC (Watts)
            double          PlatformPowerW { 0.0 };       ///< Platform / SoC / System power (Watts)
            double          GpuPowerW { 0.0 };            ///< Dedicated GPU power (Watts)
            double          VoltageV { 0.0 };             ///< Current voltage in Volts
            double          EnergyNowWh { 0.0 };          ///< Current energy in Watt-hours
            double          EnergyFullWh { 0.0 };         ///< Full charge capacity in Watt-hours
            double          EnergyFullDesignWh { 0.0 };   ///< Design capacity in Watt-hours
            double          HealthPct { -1.0 };           ///< Battery health percentage (0..100%)
            int             CycleCount { -1 };            ///< Battery charge cycle count
            qint64          TimeToEmptySec { -1 };        ///< Estimated seconds until empty
            qint64          TimeToFullSec { -1 };         ///< Estimated seconds until fully charged

            HistoryBuffer   CapacityHistory { TUX_MANAGER_HISTORY_SIZE };
            HistoryBuffer   PowerDrawHistory { TUX_MANAGER_HISTORY_SIZE };
        };

        Battery();
        ~Battery();

        void Detect();
        bool Sample();

        int BatteryCount() const { return static_cast<int>(this->m_batteries.size()); }
        const BatteryInfo &FromIndex(int index) const;

    private:
        BatteryInfo m_nullBattery;
        std::vector<std::unique_ptr<BatteryInfo>> m_batteries;

        QString m_psysEnergyPath;
        qint64  m_psysMaxRangeUj { 0 };
        qint64  m_lastPsysEnergyUj { -1 };

        QString m_pkgEnergyPath;
        qint64  m_pkgMaxRangeUj { 0 };
        qint64  m_lastPkgEnergyUj { -1 };

        qint64  m_lastSampleTimeMs { 0 };
};

#endif // BATTERY_H
