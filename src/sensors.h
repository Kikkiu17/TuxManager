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

#ifndef SENSORS_H
#define SENSORS_H

#include <QElapsedTimer>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVector>
#include <QtGlobal>

enum class SensorDeviceType
{
    Motherboard,
    Cpu,
    Gpu,
    Disk,
    Battery,
    Network,
    Other
};

enum class SensorType
{
    Voltage,
    Temperature,
    Power,
    Fan,
    Pwm,
    Current,
    Utilization,
    Clock,
    Other
};

using SensorCategoryType = SensorType;

struct SensorItem
{
    QString    id;
    QString    label;
    SensorType type        { SensorType::Other };
    double     value       { 0.0 };
    double     min         { 0.0 };
    double     max         { 0.0 };
    bool       initialized { false };

    const QString &Id() const { return this->id; }
    const QString &Label() const { return this->label; }
    SensorType Type() const { return this->type; }
    double Value() const { return this->value; }
    double Min() const { return this->min; }
    double Max() const { return this->max; }

    void ResetMinMax()
    {
        this->min = this->value;
        this->max = this->value;
    }

    void resetMinMax()
    {
        this->ResetMinMax();
    }

    QString FormatValue(double val) const
    {
        return FormatValue(val, this->type);
    }

    QString formatValue(double val) const
    {
        return FormatValue(val, this->type);
    }

    QString FormattedValue() const
    {
        return FormatValue(this->value, this->type);
    }

    QString formattedValue() const
    {
        return FormatValue(this->value, this->type);
    }

    QString FormattedMin() const
    {
        return FormatValue(this->min, this->type);
    }

    QString formattedMin() const
    {
        return FormatValue(this->min, this->type);
    }

    QString FormattedMax() const
    {
        return FormatValue(this->max, this->type);
    }

    QString formattedMax() const
    {
        return FormatValue(this->max, this->type);
    }

    static QString FormatValue(double val, SensorType type);
    static QString formatValue(double val, SensorType type)
    {
        return FormatValue(val, type);
    }

    static QString CategoryTitle(SensorType type);
    static QString categoryTitle(SensorType type)
    {
        return CategoryTitle(type);
    }
};

struct SensorCategory
{
    QString             id;
    QString             label;
    SensorType          type { SensorType::Other };
    QVector<SensorItem> items;

    const QString &Id() const { return this->id; }
    const QString &Label() const { return this->label; }
    SensorType Type() const { return this->type; }
    const QVector<SensorItem> &Items() const { return this->items; }
    const QVector<SensorItem> &Sensors() const { return this->items; }
};

struct SensorDevice
{
    QString                 id;
    QString                 name;
    SensorDeviceType        type { SensorDeviceType::Other };
    QVector<SensorCategory> categories;

    const QString &Id() const { return this->id; }
    const QString &Name() const { return this->name; }
    SensorDeviceType Type() const { return this->type; }
    const QVector<SensorCategory> &Categories() const { return this->categories; }
};

class Sensors
{
    public:
        using DeviceType   = SensorDeviceType;
        using CategoryType = SensorType;
        using Item         = SensorItem;
        using Category     = SensorCategory;
        using Device       = SensorDevice;

        Sensors();
        ~Sensors();

        /// Poll all hardware sensor sources and update device/category/sensor tree in place.
        bool Sample();
        bool sample() { return this->Sample(); }
        void Refresh() { this->Sample(); }
        void refresh() { this->Sample(); }

        /// Reset running Min/Max values on all tracked sensors to their current value.
        void ResetMinMax();
        void resetMinMax() { this->ResetMinMax(); }

        const QString &HostName() const { return this->m_hostName; }
        const QString &hostName() const { return this->m_hostName; }

        const QVector<SensorDevice> &Devices() const { return this->m_devices; }
        const QVector<SensorDevice> &devices() const { return this->m_devices; }

        static QString FormatValue(double val, SensorType type)
        {
            return SensorItem::FormatValue(val, type);
        }

        static QString formatValue(double val, SensorType type)
        {
            return SensorItem::FormatValue(val, type);
        }

        static QString CategoryTitle(SensorType type)
        {
            return SensorItem::CategoryTitle(type);
        }

    private:
        struct CpuCoreTick
        {
            quint64 prevIdle  { 0 };
            quint64 prevTotal { 0 };
            double  lastPct   { 0.0 };
        };

        struct RaplZoneState
        {
            QString id;
            QString label;
            QString energyPath;
            qint64  maxRangeUj   { 0 };
            qint64  lastEnergyUj { -1 };
            double  lastWatts    { 0.0 };
        };

        struct DiskIoState
        {
            quint64 prevReadSectors  { 0 };
            quint64 prevWriteSectors { 0 };
            quint64 prevIoMs         { 0 };
            double  lastActivePct    { 0.0 };
            bool    initialized      { false };
        };

        struct MinMaxRecord
        {
            double min         { 0.0 };
            double max         { 0.0 };
            bool   initialized { false };
        };

        SensorItem makeSensorItem(const QString &id, const QString &label, SensorType type, double value);
        static void appendCategoryIfNotEmpty(SensorDevice &dev, const QString &catIdPrefix, SensorType type, const QVector<SensorItem> &items);

        void sampleMotherboardAndMisc(QVector<SensorDevice> &outDevices,
                                      QSet<QString> &claimedHwmonDirs);
        void sampleCpu(QVector<SensorDevice> &outDevices,
                       QSet<QString> &claimedHwmonDirs,
                       double dtSec);
        void sampleGpus(QVector<SensorDevice> &outDevices,
                        QSet<QString> &claimedHwmonDirs);
        void sampleDisks(QVector<SensorDevice> &outDevices,
                         QSet<QString> &claimedHwmonDirs,
                         qint64 dtMs);
        void sampleBatteries(QVector<SensorDevice> &outDevices,
                             QSet<QString> &claimedHwmonDirs);
        void sampleHwmonDirIntoCategories(const QString &hwmonPath,
                                          const QString &idPrefix,
                                          const QString &chipName,
                                          QVector<SensorItem> &voltages,
                                          QVector<SensorItem> &temperatures,
                                          QVector<SensorItem> &powers,
                                          QVector<SensorItem> &fans,
                                          QVector<SensorItem> &currents,
                                          bool skipCpuVcore = false,
                                          bool skipCpuFan = false);

        void initRaplZones();
        void primeBaselineCounters();
        void initNvml();
        void shutdownNvml();

        QString                      m_hostName;
        QString                      m_cpuModelName;
        bool                         m_isAmdCpu { false };
        int                          m_cpuLogicalCount { 0 };
        QString                      m_boardVendor;
        QString                      m_boardName;
        QString                      m_boardDisplayName;
        QVector<SensorDevice>        m_devices;
        QHash<QString, MinMaxRecord> m_minMaxById;

        // CPU /proc/stat state
        CpuCoreTick                  m_totalCpuTick;
        QVector<CpuCoreTick>         m_coreCpuTicks;
        qint64                       m_lastCpuStatNs { -1 };

        // RAPL state
        bool                         m_raplInitialized { false };
        QVector<RaplZoneState>       m_raplZones;
        qint64                       m_lastRaplNs { -1 };

        // Disk /proc/diskstats state
        QHash<QString, DiskIoState>  m_diskIoStates;
        qint64                       m_lastDiskIoNs { -1 };

        // Timing state
        QElapsedTimer                m_elapsedTimer;
        qint64                       m_lastSampleMs { 0 };

        // Optional dynamic NVML handle for NVIDIA GPU sensors
        void                        *m_nvmlLib { nullptr };
        bool                         m_nvmlAvailable { false };
};

#endif // SENSORS_H
