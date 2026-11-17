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

#include "sensors.h"
#include "misc.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSysInfo>
#include <QtMath>

#include <dlfcn.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>

namespace
{
    using NvmlReturn = unsigned int;
    using NvmlDevice = void *;

    struct NvmlUtilization
    {
        unsigned int gpu;
        unsigned int memory;
    };

    struct NvmlMemory
    {
        quint64 total;
        quint64 free;
        quint64 used;
    };

    struct NvmlFanSpeedInfoV1
    {
        unsigned int version;
        unsigned int fan;
        unsigned int speed;
    };

    struct NvmlThermalSensor
    {
        int controller;
        int defaultMinTemp;
        int defaultMaxTemp;
        int currentTemp;
        int target;
    };

    struct NvmlGpuThermalSettings
    {
        unsigned int      count;
        NvmlThermalSensor sensor[3];
    };

    struct NvmlFieldValue
    {
        unsigned int  fieldId;
        unsigned int  scopeId;
        long long     timestamp;
        long long     latencyUsec;
        int           valueType;
        int           nvmlReturn;
        unsigned char value[16];
    };

    static constexpr NvmlReturn kNvmlSuccess = 0;

    using FnNvmlInitV2                  = NvmlReturn (*)();
    using FnNvmlShutdown                = NvmlReturn (*)();
    using FnNvmlDeviceGetCountV2        = NvmlReturn (*)(unsigned int *);
    using FnNvmlDeviceGetHandleByIndex  = NvmlReturn (*)(unsigned int, NvmlDevice *);
    using FnNvmlDeviceGetName           = NvmlReturn (*)(NvmlDevice, char *, unsigned int);
    using FnNvmlDeviceGetTemperature    = NvmlReturn (*)(NvmlDevice, unsigned int, unsigned int *);
    using FnNvmlDeviceGetThermalSettings = NvmlReturn (*)(NvmlDevice, unsigned int, NvmlGpuThermalSettings *);
    using FnNvmlDeviceGetFieldValues    = NvmlReturn (*)(NvmlDevice, int, NvmlFieldValue *);
    using FnNvmlDeviceGetPowerUsage     = NvmlReturn (*)(NvmlDevice, unsigned int *);
    using FnNvmlDeviceGetFanSpeed       = NvmlReturn (*)(NvmlDevice, unsigned int *);
    using FnNvmlDeviceGetNumFans        = NvmlReturn (*)(NvmlDevice, unsigned int *);
    using FnNvmlDeviceGetFanSpeedRPM    = NvmlReturn (*)(NvmlDevice, NvmlFanSpeedInfoV1 *);
    using FnNvmlDeviceGetClockInfo      = NvmlReturn (*)(NvmlDevice, unsigned int, unsigned int *);
    using FnNvmlDeviceGetUtilization    = NvmlReturn (*)(NvmlDevice, NvmlUtilization *);
    using FnNvmlDeviceGetMemoryInfo     = NvmlReturn (*)(NvmlDevice, NvmlMemory *);

    struct NvmlSymbols
    {
        FnNvmlInitV2                 InitV2                 { nullptr };
        FnNvmlShutdown               Shutdown               { nullptr };
        FnNvmlDeviceGetCountV2       DeviceGetCountV2       { nullptr };
        FnNvmlDeviceGetHandleByIndex DeviceGetHandleByIndex { nullptr };
        FnNvmlDeviceGetName          DeviceGetName          { nullptr };
        FnNvmlDeviceGetTemperature   DeviceGetTemperature   { nullptr };
        FnNvmlDeviceGetThermalSettings DeviceGetThermalSettings { nullptr };
        FnNvmlDeviceGetFieldValues   DeviceGetFieldValues   { nullptr };
        FnNvmlDeviceGetPowerUsage    DeviceGetPowerUsage    { nullptr };
        FnNvmlDeviceGetFanSpeed      DeviceGetFanSpeed      { nullptr };
        FnNvmlDeviceGetNumFans       DeviceGetNumFans       { nullptr };
        FnNvmlDeviceGetFanSpeedRPM   DeviceGetFanSpeedRPM   { nullptr };
        FnNvmlDeviceGetClockInfo     DeviceGetClockInfo     { nullptr };
        FnNvmlDeviceGetUtilization   DeviceGetUtilization   { nullptr };
        FnNvmlDeviceGetMemoryInfo    DeviceGetMemoryInfo    { nullptr };
    };

    NvmlSymbols g_nvml;

    int extractSensorIndex(const QString &fileName, const QString &prefix, const QString &suffix)
    {
        if (!fileName.startsWith(prefix) || !fileName.endsWith(suffix))
            return -1;
        const QString mid = fileName.mid(prefix.size(), fileName.size() - prefix.size() - suffix.size());
        bool ok = false;
        const int idx = mid.toInt(&ok);
        return ok ? idx : -1;
    }

    QStringList sortedSensorInputs(const QDir &dir, const QString &prefix)
    {
        const QString suffix = QStringLiteral("_input");
        QStringList files = dir.entryList(QStringList() << (prefix + QStringLiteral("*_input")), QDir::Files);
        std::sort(files.begin(), files.end(), [&](const QString &a, const QString &b)
        {
            return extractSensorIndex(a, prefix, suffix) < extractSensorIndex(b, prefix, suffix);
        });
        return files;
    }

    bool isSuperIoChip(const QString &chipName)
    {
        static const QStringList kSuperIoTokens {
            QStringLiteral("nct6"),
            QStringLiteral("it86"),
            QStringLiteral("it87"),
            QStringLiteral("f718"),
            QStringLiteral("f818"),
            QStringLiteral("w836")
        };
        return Misc::TextContainsAnyToken(chipName.toLower(), kSuperIoTokens);
    }

    bool isNuvotonChip(const QString &chipName)
    {
        return chipName.toLower().startsWith(QLatin1String("nct6"));
    }

    bool isIteChip(const QString &chipName)
    {
        const QString lower = chipName.toLower();
        return lower.startsWith(QLatin1String("it86")) || lower.startsWith(QLatin1String("it87"));
    }

    bool isFintekChip(const QString &chipName)
    {
        const QString lower = chipName.toLower();
        return lower.startsWith(QLatin1String("f718")) || lower.startsWith(QLatin1String("f818"));
    }

    bool isCpuHwmonChip(const QString &chipName)
    {
        static const QStringList kCpuChipTokens {
            QStringLiteral("coretemp"),
            QStringLiteral("k10temp"),
            QStringLiteral("zenpower"),
            QStringLiteral("cpu_thermal"),
            QStringLiteral("x86_pkg_temp"),
            QStringLiteral("soc_thermal"),
            QStringLiteral("fam15h_power")
        };
        return Misc::TextContainsAnyToken(chipName.toLower(), kCpuChipTokens);
    }

    struct ResolvedVoltageChannel
    {
        QString label;
        double  volts  { 0.0 };
        bool    ignore { false };
    };

    ResolvedVoltageChannel resolveSuperIoVoltage(const QString &chipName,
                                                 const QString &boardVendor,
                                                 int idx,
                                                 double rawVolts,
                                                 const QString &sysfsLabel)
    {
        ResolvedVoltageChannel out;
        out.label = sysfsLabel;
        out.volts = rawVolts;

        const bool isMsi    = boardVendor.contains(QLatin1String("Micro-Star"), Qt::CaseInsensitive)
                           || boardVendor.contains(QLatin1String("MSI"), Qt::CaseInsensitive);
        const bool isAsrock = boardVendor.contains(QLatin1String("ASRock"), Qt::CaseInsensitive);

        if (isNuvotonChip(chipName))
        {
            switch (idx)
            {
                case 0:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("Vcore");
                    break;
                case 1:
                    // External +5V or +12V resistor divider on Nuvoton VIN1:
                    // MSI / ASUS use Ri=4, Rf=1 (5x multiplier: ~1.016V -> 5.080V = +5V).
                    // ASRock Z690/Z790/X570 use Ri=2, Rf=1 (3x multiplier: ~1.67V -> 5.01V = +5V).
                    // ASRock X870 uses Ri=56, Rf=10 (6.6x multiplier: ~1.82V -> 12.0V = +12V).
                    if (rawVolts >= 0.85 && rawVolts <= 1.15)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("+5V");
                        out.volts = rawVolts * 5.0;
                    }
                    else if (rawVolts >= 1.50 && rawVolts <= 1.78 && !isMsi)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("+5V");
                        out.volts = rawVolts * 3.0;
                    }
                    else if (rawVolts >= 1.78 && rawVolts <= 1.98 && isAsrock)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("+12V");
                        out.volts = rawVolts * 6.6;
                    }
                    else if (out.label.isEmpty())
                    {
                        out.label = QStringLiteral("VIN1");
                    }
                    break;
                case 2:
                    // Internal 2x divider already scaled by Linux nct6775 kernel driver
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("AVCC");
                    break;
                case 3:
                    // Internal 2x divider already scaled by Linux nct6775 kernel driver
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("+3.3V");
                    break;
                case 4:
                    // External +12V resistor divider on Nuvoton VIN4 (in4):
                    // MSI / ASUS use Ri=11, Rf=1 (12x multiplier: ~1.016V -> 12.192V = +12V).
                    // ASRock uses Ri=28, Rf=5 or Ri=56, Rf=10 (6.6x multiplier: ~1.82V -> 12.0V = +12V).
                    if (rawVolts >= 0.85 && rawVolts <= 1.15)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("+12V");
                        out.volts = rawVolts * 12.0;
                    }
                    else if (rawVolts >= 1.65 && rawVolts <= 1.98)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("+12V");
                        out.volts = rawVolts * 6.6;
                    }
                    else if (out.label.isEmpty())
                    {
                        out.label = QStringLiteral("VIN4");
                    }
                    break;
                case 5:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("VIN5");
                    break;
                case 6:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("VIN6");
                    break;
                case 7:
                    // Internal 2x divider already scaled by Linux nct6775 kernel driver
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("3VSB");
                    break;
                case 8:
                    // Internal 2x divider already scaled by Linux nct6775 kernel driver
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("Vbat");
                    break;
                case 9:
                    if (out.label.isEmpty())
                    {
                        out.label = (rawVolts >= 1.70 && rawVolts <= 1.98)
                            ? QStringLiteral("CPU 1.8V (VTT)")
                            : QStringLiteral("VTT");
                    }
                    break;
                case 10:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("VIN10");
                    break;
                case 11:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("VIN11");
                    break;
                case 12:
                    if (rawVolts >= 0.80 && rawVolts <= 1.35)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("CPU NB/SoC");
                    }
                    else if (rawVolts >= 1.50 && rawVolts <= 1.80 && isAsrock)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("+5V");
                        out.volts = rawVolts * 3.0;
                    }
                    else if (out.label.isEmpty())
                    {
                        out.label = QStringLiteral("VIN12");
                    }
                    break;
                case 13:
                    // MSI Nuvoton boards wire DIMM (VDIMM) through a 1:1 divider (Ri=1, Rf=1 -> 2x multiplier: 0.704V -> 1.408V)
                    if (rawVolts >= 0.55 && rawVolts <= 0.85)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("DRAM");
                        out.volts = rawVolts * 2.0;
                    }
                    else if (out.label.isEmpty())
                    {
                        out.label = QStringLiteral("VIN13");
                    }
                    break;
                case 14:
                    // MSI Nuvoton boards wire ATX +5V Standby (5VSB) through Ri=768, Rf=330 (3.32727x: 1.536V -> 5.111V)
                    if (rawVolts >= 1.40 && rawVolts <= 1.65)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("5VSB");
                        out.volts = rawVolts * (1.0 + 768.0 / 330.0);
                    }
                    else if (out.label.isEmpty())
                    {
                        out.label = QStringLiteral("VIN14");
                    }
                    break;
                default:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("VIN%1").arg(idx);
                    break;
            }
            return out;
        }

        if (isIteChip(chipName))
        {
            switch (idx)
            {
                case 0:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("Vcore");
                    break;
                case 1:
                    if (out.label.isEmpty())
                        out.label = (rawVolts >= 1.10 && rawVolts <= 1.65) ? QStringLiteral("DRAM") : QStringLiteral("VIN1");
                    break;
                case 2:
                    if (rawVolts >= 3.00 && rawVolts <= 3.60)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("+3.3V");
                    }
                    else if (rawVolts >= 1.90 && rawVolts <= 2.15)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("+3.3V");
                        out.volts = rawVolts * 1.649;
                    }
                    else if (out.label.isEmpty())
                    {
                        out.label = QStringLiteral("VIN2");
                    }
                    break;
                case 3:
                    if (rawVolts >= 2.80 && rawVolts <= 3.20)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("+5V");
                        out.volts = rawVolts * 1.68;
                    }
                    else if (out.label.isEmpty())
                    {
                        out.label = QStringLiteral("VIN3");
                    }
                    break;
                case 4:
                    if (rawVolts >= 2.80 && rawVolts <= 3.20)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("+12V");
                        out.volts = rawVolts * 4.0;
                    }
                    else if (out.label.isEmpty())
                    {
                        out.label = QStringLiteral("VIN4");
                    }
                    break;
                case 7:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("3VSB");
                    break;
                case 8:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("Vbat");
                    break;
                default:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("VIN%1").arg(idx);
                    break;
            }
            return out;
        }

        if (isFintekChip(chipName))
        {
            switch (idx)
            {
                case 0:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("+3.3V");
                    if (rawVolts >= 1.50 && rawVolts <= 1.85)
                        out.volts = rawVolts * 2.0;
                    break;
                case 1:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("Vcore");
                    break;
                case 3:
                    if (rawVolts >= 0.85 && rawVolts <= 1.10)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("+5V");
                        out.volts = rawVolts * (1.0 + 20.0 / 4.7);
                    }
                    else if (out.label.isEmpty())
                    {
                        out.label = QStringLiteral("VIN3");
                    }
                    break;
                case 4:
                    if (rawVolts >= 0.95 && rawVolts <= 1.25)
                    {
                        if (out.label.isEmpty())
                            out.label = QStringLiteral("+12V");
                        out.volts = rawVolts * 11.0;
                    }
                    else if (out.label.isEmpty())
                    {
                        out.label = QStringLiteral("VIN4");
                    }
                    break;
                case 7:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("3VSB");
                    if (rawVolts >= 1.50 && rawVolts <= 1.85)
                        out.volts = rawVolts * 2.0;
                    break;
                case 8:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("Vbat");
                    if (rawVolts >= 1.40 && rawVolts <= 1.85)
                        out.volts = rawVolts * 2.0;
                    break;
                default:
                    if (out.label.isEmpty())
                        out.label = QStringLiteral("VIN%1").arg(idx);
                    break;
            }
            return out;
        }

        if (!out.label.isEmpty())
        {
            if (out.label.compare(QLatin1String("vddgfx"), Qt::CaseInsensitive) == 0)
                out.label = QStringLiteral("GPU Core (VDDGFX)");
            else if (out.label.compare(QLatin1String("vddnb"), Qt::CaseInsensitive) == 0)
                out.label = QStringLiteral("GPU SoC (VDDNB)");
            return out;
        }

        if (isSuperIoChip(chipName))
            {
                switch (idx)
                {
                    case 0: out.label = QStringLiteral("Vcore"); break;
                    case 2: out.label = QStringLiteral("AVCC");  break;
                    case 3: out.label = QStringLiteral("+3.3V"); break;
                    case 7: out.label = QStringLiteral("3VSB");  break;
                    case 8: out.label = QStringLiteral("Vbat");  break;
                default:
                    out.label = QStringLiteral("VIN%1").arg(idx);
                    break;
            }
        }
        else
        {
            out.label = QStringLiteral("VIN #%1").arg(idx);
        }
        return out;
    }

    QString resolveHwmonTempLabel(const QString &chipName, const QString &boardVendor, int idx, const QString &rawLabel)
    {
        const QString trimmed = rawLabel.trimmed();
        const bool isMsi = boardVendor.contains(QLatin1String("Micro-Star"), Qt::CaseInsensitive)
                        || boardVendor.contains(QLatin1String("MSI"), Qt::CaseInsensitive);

        if (isNuvotonChip(chipName))
        {
            if (trimmed.compare(QLatin1String("SYSTIN"), Qt::CaseInsensitive) == 0)
                return QStringLiteral("System (SYSTIN)");
            if (trimmed.compare(QLatin1String("CPUTIN"), Qt::CaseInsensitive) == 0)
                return QStringLiteral("CPU Socket (CPUTIN)");
            if (trimmed.compare(QLatin1String("AUXTIN0"), Qt::CaseInsensitive) == 0)
                return isMsi ? QStringLiteral("VRM MOS (AUXTIN0)") : QStringLiteral("Auxiliary #0 (AUXTIN0)");
            if (trimmed.compare(QLatin1String("AUXTIN1"), Qt::CaseInsensitive) == 0)
                return QStringLiteral("Auxiliary #1 (AUXTIN1)");
            if (trimmed.compare(QLatin1String("AUXTIN2"), Qt::CaseInsensitive) == 0)
                return isMsi ? QStringLiteral("Chipset (AUXTIN2)") : QStringLiteral("Auxiliary #2 (AUXTIN2)");
            if (trimmed.compare(QLatin1String("AUXTIN3"), Qt::CaseInsensitive) == 0)
                return QStringLiteral("Auxiliary #3 (AUXTIN3)");
        }

        // GPU hwmon temperature labels (amdgpu, i915, xe, nouveau)
        if (trimmed.compare(QLatin1String("edge"), Qt::CaseInsensitive) == 0)
            return QStringLiteral("GPU");
        if (trimmed.compare(QLatin1String("junction"), Qt::CaseInsensitive) == 0
            || trimmed.compare(QLatin1String("hotspot"), Qt::CaseInsensitive) == 0)
            return QStringLiteral("Hot Spot");
        if (trimmed.compare(QLatin1String("mem"), Qt::CaseInsensitive) == 0
            || trimmed.compare(QLatin1String("memory"), Qt::CaseInsensitive) == 0)
            return QStringLiteral("Memory");

        if (!trimmed.isEmpty())
            return trimmed;

        if (chipName.startsWith(QLatin1String("iwlwifi"), Qt::CaseInsensitive))
            return QStringLiteral("Wi-Fi (%1)").arg(chipName);
        if (chipName.startsWith(QLatin1String("mt79"), Qt::CaseInsensitive)
            || chipName.startsWith(QLatin1String("ath"), Qt::CaseInsensitive))
            return QStringLiteral("Wireless (%1)").arg(chipName);

        return chipName.isEmpty()
            ? QStringLiteral("Temp #%1").arg(idx)
            : QStringLiteral("%1 #%2").arg(chipName.toUpper()).arg(idx);
    }

    QString resolveSuperIoFanLabel(const QDir &dir, const QString &chipName, const QString &boardVendor, int idx, const QString &rawLabel)
    {
        const QString trimmed = rawLabel.trimmed();
        if (!trimmed.isEmpty())
            return trimmed;

        if (!isSuperIoChip(chipName))
            return QStringLiteral("FANIN%1").arg(idx);

        const bool isMsi = boardVendor.contains(QLatin1String("Micro-Star"), Qt::CaseInsensitive)
                        || boardVendor.contains(QLatin1String("MSI"), Qt::CaseInsensitive);

        // On MSI Nuvoton boards (and boards where pwm2_temp_sel selects CPU temp 7 while pwm1 selects System temp 1),
        // fan1 is Pump Fan (PUMP_FAN1), fan2 is CPU Fan (CPU_FAN1), and fan3..fan6 are System Fan #1..#4.
        const QString pwm1Sel = Misc::ReadFile(dir.filePath(QStringLiteral("pwm1_temp_sel"))).trimmed();
        const QString pwm2Sel = Misc::ReadFile(dir.filePath(QStringLiteral("pwm2_temp_sel"))).trimmed();
        const bool fan1IsPumpFan2IsCpu = (isNuvotonChip(chipName) && isMsi)
                                      || (pwm1Sel == QLatin1String("1") && pwm2Sel == QLatin1String("7"));

        if (fan1IsPumpFan2IsCpu)
        {
            switch (idx)
            {
                case 1: return QStringLiteral("Pump Fan (FANIN1)");
                case 2: return QStringLiteral("CPU Fan (FANIN2)");
                case 3: return QStringLiteral("System Fan #1 (FANIN3)");
                case 4: return QStringLiteral("System Fan #2 (FANIN4)");
                case 5: return QStringLiteral("System Fan #3 (FANIN5)");
                case 6: return QStringLiteral("System Fan #4 (FANIN6)");
                case 7: return QStringLiteral("Chipset Fan (FANIN7)");
                default: return QStringLiteral("FANIN%1").arg(idx);
            }
        }

        switch (idx)
        {
            case 1: return QStringLiteral("CPU Fan (FANIN1)");
            default: return QStringLiteral("System Fan #%1 (FANIN%2)").arg(idx - 1).arg(idx);
        }
    }

    quint64 parseProcStatCpuLine(const QList<QByteArray> &parts, quint64 &outIdle)
    {
        const quint64 user    = parts.value(1).toULongLong();
        const quint64 nice    = parts.value(2).toULongLong();
        const quint64 system  = parts.value(3).toULongLong();
        const quint64 idle    = parts.value(4).toULongLong();
        const quint64 iowait  = parts.value(5).toULongLong();
        const quint64 irq     = parts.value(6).toULongLong();
        const quint64 softirq = parts.value(7).toULongLong();
        const quint64 steal   = parts.value(8).toULongLong();

        outIdle = idle + iowait;
        return user + nice + system + outIdle + irq + softirq + steal;
    }

    bool shouldIgnoreBlockDevice(const QString &baseName)
    {
        return baseName.startsWith(QLatin1String("loop"))
            || baseName.startsWith(QLatin1String("pktcdvd"))
            || baseName.startsWith(QLatin1String("sr"))
            || baseName.startsWith(QLatin1String("dm-"))
            || baseName.startsWith(QLatin1String("md"))
            || baseName.startsWith(QLatin1String("ram"))
            || baseName.startsWith(QLatin1String("zram"));
    }

    QSet<QString> resolveBaseBlockDevices(const QString &devName)
    {
        QSet<QString> out;
        if (devName.isEmpty() || devName == QLatin1String(".") || devName == QLatin1String(".."))
            return out;

        const QString sysPath = QStringLiteral("/sys/class/block/") + devName;
        if (!QFileInfo::exists(sysPath))
            return out;

        const QDir slavesDir(sysPath + QStringLiteral("/slaves"));
        const QStringList slaves = slavesDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (!slaves.isEmpty())
        {
            for (const QString &slave : slaves)
                out.unite(resolveBaseBlockDevices(slave));
            return out;
        }

        if (QFileInfo::exists(sysPath + QStringLiteral("/partition")))
        {
            QString parentName;
            const QString canonical = QFileInfo(sysPath).canonicalFilePath();
            if (!canonical.isEmpty())
            {
                QDir d(canonical);
                if (d.cdUp())
                    parentName = d.dirName();
            }

            if ((parentName.isEmpty() || parentName == QLatin1String(".") || parentName == QLatin1String("..")) && !devName.isEmpty())
            {
                static const QRegularExpression partRe(QStringLiteral("^(.*?)(?:p)?\\d+$"));
                const QRegularExpressionMatch m = partRe.match(devName);
                if (m.hasMatch())
                    parentName = m.captured(1);
            }

            if (!parentName.isEmpty() && parentName != QLatin1String(".") && parentName != QLatin1String("..") && parentName != devName)
                return resolveBaseBlockDevices(parentName);
        }

        out.insert(devName);
        return out;
    }

    struct UDisks2DriveCache
    {
        QString model;
        double  temperatureC { -1.0 };
        qint64  fetchedMs    { 0 };
    };

    UDisks2DriveCache queryUDisks2ForBlock(const QString &blockName, qint64 nowMs)
    {
        static QHash<QString, UDisks2DriveCache> s_cache;
        const auto it = s_cache.constFind(blockName);
        if (it != s_cache.cend() && (nowMs - it->fetchedMs) < 5000)
            return it.value();

        UDisks2DriveCache entry;
        entry.fetchedMs = nowMs;

        QDBusConnection bus = QDBusConnection::systemBus();
        if (bus.isConnected())
        {
            QDBusInterface blockIface(
                QStringLiteral("org.freedesktop.UDisks2"),
                QStringLiteral("/org/freedesktop/UDisks2/block_devices/") + blockName,
                QStringLiteral("org.freedesktop.UDisks2.Block"),
                bus);

            if (blockIface.isValid())
            {
                const QDBusObjectPath driveObj = blockIface.property("Drive").value<QDBusObjectPath>();
                const QString drivePath = driveObj.path();
                if (!drivePath.isEmpty() && drivePath != QLatin1String("/"))
                {
                    QDBusInterface driveIface(
                        QStringLiteral("org.freedesktop.UDisks2"),
                        drivePath,
                        QStringLiteral("org.freedesktop.UDisks2.Drive"),
                        bus);
                    if (driveIface.isValid())
                        entry.model = driveIface.property("Model").toString().trimmed();

                    QDBusInterface ataIface(
                        QStringLiteral("org.freedesktop.UDisks2"),
                        drivePath,
                        QStringLiteral("org.freedesktop.UDisks2.Drive.Ata"),
                        bus);
                    if (ataIface.isValid())
                    {
                        const double tempK = ataIface.property("SmartTemperature").toDouble();
                        if (tempK > 273.15 && tempK < 400.0)
                            entry.temperatureC = tempK - 273.15;
                    }

                    if (entry.temperatureC < 0.0)
                    {
                        QDBusInterface nvmeIface(
                            QStringLiteral("org.freedesktop.UDisks2"),
                            drivePath,
                            QStringLiteral("org.freedesktop.UDisks2.NVMe.Controller"),
                            bus);
                        if (nvmeIface.isValid())
                        {
                            const double tempK = nvmeIface.property("SmartTemperature").toDouble();
                            if (tempK > 273.15 && tempK < 400.0)
                                entry.temperatureC = tempK - 273.15;
                        }
                    }
                }
            }
        }

        s_cache.insert(blockName, entry);
        return entry;
    }
}

QString SensorItem::FormatValue(double val, SensorType type)
{
    switch (type)
    {
        case SensorType::Temperature:
        {
            const int degC = qRound(val);
            const int degF = qRound(val * 9.0 / 5.0 + 32.0);
            return QStringLiteral("%1 °C (%2 °F)").arg(degC).arg(degF);
        }
        case SensorType::Voltage:
            return QStringLiteral("%1 V").arg(QString::number(val, 'f', 3));
        case SensorType::Power:
            return QStringLiteral("%1 W").arg(QString::number(val, 'f', 2));
        case SensorType::Fan:
            return QStringLiteral("%1 RPM").arg(qRound(val));
        case SensorType::Pwm:
            return QStringLiteral("%1 %").arg(qRound(val));
        case SensorType::Current:
            return QStringLiteral("%1 A").arg(QString::number(val, 'f', 2));
        case SensorType::Utilization:
            return QStringLiteral("%1 %").arg(qRound(val));
        case SensorType::Clock:
            return QStringLiteral("%1 MHz").arg(qRound(val));
        case SensorType::Other:
        default:
            return QString::number(val, 'f', 1);
    }
}

QString SensorItem::CategoryTitle(SensorType type)
{
    switch (type)
    {
        case SensorType::Voltage:     return QStringLiteral("Voltages");
        case SensorType::Temperature: return QStringLiteral("Temperatures");
        case SensorType::Power:       return QStringLiteral("Powers");
        case SensorType::Fan:         return QStringLiteral("Fans");
        case SensorType::Pwm:         return QStringLiteral("Fans PWM");
        case SensorType::Current:     return QStringLiteral("Currents");
        case SensorType::Utilization: return QStringLiteral("Utilization");
        case SensorType::Clock:       return QStringLiteral("Clocks");
        case SensorType::Other:
        default:                      return QStringLiteral("Other");
    }
}

Sensors::Sensors()
{
    this->m_hostName = QSysInfo::machineHostName();
    if (this->m_hostName.isEmpty())
        this->m_hostName = QStringLiteral("Linux-PC");

    this->m_cpuLogicalCount = qMax(1, static_cast<int>(::sysconf(_SC_NPROCESSORS_ONLN)));

    // Read CPU vendor & model name from /proc/cpuinfo
    QFile cpuInfoFile(QStringLiteral("/proc/cpuinfo"));
    if (cpuInfoFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        for (;;)
        {
            const QByteArray line = cpuInfoFile.readLine();
            if (line.isNull())
                break;
            const int colon = line.indexOf(':');
            if (colon < 0)
                continue;
            const QByteArray key = line.left(colon).trimmed();
            const QByteArray val = line.mid(colon + 1).trimmed();
            if (key == "vendor_id" && val.contains("AMD"))
            {
                this->m_isAmdCpu = true;
            }
            else if (this->m_cpuModelName.isEmpty() && (key == "model name" || key == "Processor" || key == "Hardware"))
            {
                this->m_cpuModelName = QString::fromUtf8(val);
            }
            if (!this->m_cpuModelName.isEmpty() && this->m_isAmdCpu)
                break;
        }
        cpuInfoFile.close();
    }
    if (this->m_cpuModelName.contains(QLatin1String("AMD"), Qt::CaseInsensitive))
        this->m_isAmdCpu = true;
    if (this->m_cpuModelName.isEmpty())
        this->m_cpuModelName = QStringLiteral("Processor");

    // Read Motherboard vendor & model from DMI
    this->m_boardVendor = Misc::ReadFile(QStringLiteral("/sys/class/dmi/id/board_vendor")).trimmed();
    this->m_boardName   = Misc::ReadFile(QStringLiteral("/sys/class/dmi/id/board_name")).trimmed();
    if (this->m_boardName.isEmpty())
        this->m_boardName = Misc::ReadFile(QStringLiteral("/sys/class/dmi/id/product_name")).trimmed();
    if (this->m_boardVendor.isEmpty())
        this->m_boardVendor = Misc::ReadFile(QStringLiteral("/sys/class/dmi/id/sys_vendor")).trimmed();

    if (!this->m_boardVendor.isEmpty() && !this->m_boardName.isEmpty())
        this->m_boardDisplayName = this->m_boardVendor + QLatin1Char(' ') + this->m_boardName;
    else if (!this->m_boardName.isEmpty())
        this->m_boardDisplayName = this->m_boardName;
    else if (!this->m_boardVendor.isEmpty())
        this->m_boardDisplayName = this->m_boardVendor;
    else
        this->m_boardDisplayName = QStringLiteral("Motherboard");

    this->initRaplZones();
    this->initNvml();

    this->m_elapsedTimer.start();

    // Prime baseline counters (/proc/stat, RAPL energy_uj, /proc/diskstats), sleep 100ms
    // (10 USER_HZ ticks) so the initial Sample() has clean, spike-free delta readings,
    // and then lock in initial Min/Max values.
    this->primeBaselineCounters();
    ::usleep(100000);
    this->Sample();
    this->ResetMinMax();
}

Sensors::~Sensors()
{
    this->shutdownNvml();
}

SensorItem Sensors::makeSensorItem(const QString &id, const QString &label, SensorType type, double value)
{
    SensorItem item;
    item.id    = id;
    item.label = label;
    item.type  = type;
    item.value = value;

    MinMaxRecord &rec = this->m_minMaxById[id];
    if (!rec.initialized)
    {
        rec.min         = value;
        rec.max         = value;
        rec.initialized = true;
    }
    else
    {
        if (value < rec.min)
            rec.min = value;
        if (value > rec.max)
            rec.max = value;
    }

    item.min         = rec.min;
    item.max         = rec.max;
    item.initialized = true;
    return item;
}

void Sensors::appendCategoryIfNotEmpty(SensorDevice &dev, const QString &catIdPrefix, SensorType type, const QVector<SensorItem> &items)
{
    if (items.isEmpty())
        return;

    SensorCategory cat;
    cat.id    = catIdPrefix + QLatin1Char(':') + SensorItem::CategoryTitle(type);
    cat.label = SensorItem::CategoryTitle(type);
    cat.type  = type;
    cat.items = items;
    dev.categories.append(cat);
}

void Sensors::ResetMinMax()
{
    for (SensorDevice &dev : this->m_devices)
    {
        for (SensorCategory &cat : dev.categories)
        {
            for (SensorItem &item : cat.items)
            {
                item.ResetMinMax();
                MinMaxRecord &rec = this->m_minMaxById[item.id];
                rec.min         = item.value;
                rec.max         = item.value;
                rec.initialized = true;
            }
        }
    }
}

bool Sensors::Sample()
{
    if (!this->m_elapsedTimer.isValid())
        this->m_elapsedTimer.start();

    const qint64 nowMs = this->m_elapsedTimer.elapsed();
    const qint64 dtMs  = (this->m_lastSampleMs > 0) ? (nowMs - this->m_lastSampleMs) : 0;
    const double dtSec = (dtMs > 0) ? (static_cast<double>(dtMs) / 1000.0) : 0.0;
    this->m_lastSampleMs = nowMs;

    QSet<QString> claimedHwmonDirs;

    QVector<SensorDevice> cpuDevices;
    QVector<SensorDevice> gpuDevices;
    QVector<SensorDevice> diskDevices;
    QVector<SensorDevice> batteryDevices;
    QVector<SensorDevice> mbDevices;

    // 1. Sample CPU, GPUs, Disks, Batteries first so they claim their dedicated hwmon directories.
    this->sampleCpu(cpuDevices, claimedHwmonDirs, dtSec);
    this->sampleGpus(gpuDevices, claimedHwmonDirs);
    this->sampleDisks(diskDevices, claimedHwmonDirs, dtMs);
    this->sampleBatteries(batteryDevices, claimedHwmonDirs);

    // 2. Sample Motherboard / Super I/O / ACPI / remaining hwmon & thermal zones.
    this->sampleMotherboardAndMisc(mbDevices, claimedHwmonDirs);

    // 3. Assemble in HWMonitor order: Motherboard -> CPU -> GPU -> Disks -> Battery
    QVector<SensorDevice> allDevices;
    allDevices.reserve(mbDevices.size() + cpuDevices.size() + gpuDevices.size() + diskDevices.size() + batteryDevices.size());
    allDevices.append(mbDevices);
    allDevices.append(cpuDevices);
    allDevices.append(gpuDevices);
    allDevices.append(diskDevices);
    allDevices.append(batteryDevices);

    this->m_devices = std::move(allDevices);
    return !this->m_devices.isEmpty();
}

void Sensors::initRaplZones()
{
    if (this->m_raplInitialized)
        return;
    this->m_raplInitialized = true;
    this->m_raplZones.clear();

    const QDir powercapDir(QStringLiteral("/sys/class/powercap"));
    if (!powercapDir.exists())
        return;

    const QStringList entries = powercapDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries)
    {
        if (!entry.startsWith(QLatin1String("intel-rapl:")) && !entry.startsWith(QLatin1String("amd-rapl:")))
            continue;

        const QString zonePath   = powercapDir.filePath(entry);
        const QString energyPath = zonePath + QStringLiteral("/energy_uj");
        if (!QFileInfo::exists(energyPath))
            continue;

        // Verify energy_uj is readable by current user
        bool okRead = false;
        const qint64 initialEnergy = Misc::ReadFile(energyPath).trimmed().toLongLong(&okRead);
        if (!okRead || initialEnergy < 0)
            continue;

        const QString rawName = Misc::ReadFile(zonePath + QStringLiteral("/name")).trimmed();
        QString label;
        if (rawName.startsWith(QLatin1String("package")))
            label = QStringLiteral("Package");
        else if (rawName == QLatin1String("core"))
            label = this->m_isAmdCpu ? QStringLiteral("Cores") : QStringLiteral("IA Cores");
        else if (rawName == QLatin1String("uncore"))
            label = QStringLiteral("GT");
        else if (rawName == QLatin1String("dram"))
            label = QStringLiteral("DRAM");
        else if (rawName == QLatin1String("psys"))
            label = QStringLiteral("Platform");
        else if (!rawName.isEmpty())
            label = rawName;
        else
            label = entry;

        bool okRange = false;
        const qint64 maxRange = Misc::ReadFile(zonePath + QStringLiteral("/max_energy_range_uj")).trimmed().toLongLong(&okRange);

        RaplZoneState state;
        state.id           = QStringLiteral("cpu:0:rapl:") + entry;
        state.label        = label;
        state.energyPath   = energyPath;
        state.maxRangeUj   = okRange ? maxRange : 0;
        state.lastEnergyUj = initialEnergy;
        state.lastWatts    = 0.0;
        this->m_raplZones.append(state);
    }
}

void Sensors::primeBaselineCounters()
{
    const qint64 nowNs = this->m_elapsedTimer.nsecsElapsed();

    // 1. Prime RAPL energy_uj counters
    for (RaplZoneState &zone : this->m_raplZones)
    {
        bool ok = false;
        const qint64 curEnergyUj = Misc::ReadFile(zone.energyPath).trimmed().toLongLong(&ok);
        if (ok && curEnergyUj >= 0)
            zone.lastEnergyUj = curEnergyUj;
    }
    this->m_lastRaplNs = nowNs;

    // 2. Prime CPU /proc/stat tick counters
    QFile statFile(QStringLiteral("/proc/stat"));
    if (statFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        int coreIdx = 0;
        for (;;)
        {
            const QByteArray raw = statFile.readLine();
            if (raw.isNull())
                break;
            const QList<QByteArray> parts = raw.simplified().split(' ');
            if (parts.isEmpty())
                continue;
            const QByteArray key = parts.at(0);
            if (key == "cpu")
            {
                quint64 idleAll = 0;
                this->m_totalCpuTick.prevTotal = parseProcStatCpuLine(parts, idleAll);
                this->m_totalCpuTick.prevIdle  = idleAll;
            }
            else if (key.startsWith("cpu") && key.size() > 3)
            {
                if (coreIdx >= this->m_coreCpuTicks.size())
                    this->m_coreCpuTicks.resize(coreIdx + 1);
                quint64 idleCore = 0;
                this->m_coreCpuTicks[coreIdx].prevTotal = parseProcStatCpuLine(parts, idleCore);
                this->m_coreCpuTicks[coreIdx].prevIdle  = idleCore;
                ++coreIdx;
            }
            else if (coreIdx > 0)
            {
                break;
            }
        }
        statFile.close();
        this->m_lastCpuStatNs = nowNs;
    }

    // 3. Prime Disk /proc/diskstats I/O counters
    QFile diskStatsFile(QStringLiteral("/proc/diskstats"));
    if (diskStatsFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        for (;;)
        {
            const QByteArray line = diskStatsFile.readLine();
            if (line.isNull())
                break;
            const QList<QByteArray> parts = line.simplified().split(' ');
            if (parts.size() < 14)
                continue;
            const QString devName      = QString::fromUtf8(parts.at(2));
            const quint64 readSectors  = parts.at(5).toULongLong();
            const quint64 writeSectors = parts.at(9).toULongLong();
            const quint64 ioMs         = parts.at(12).toULongLong();

            DiskIoState &st     = this->m_diskIoStates[devName];
            st.prevReadSectors  = readSectors;
            st.prevWriteSectors = writeSectors;
            st.prevIoMs         = ioMs;
            st.initialized      = true;
        }
        diskStatsFile.close();
        this->m_lastDiskIoNs = nowNs;
    }
}

void Sensors::initNvml()
{
    this->shutdownNvml();

    this->m_nvmlLib = ::dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!this->m_nvmlLib)
        this->m_nvmlLib = ::dlopen("libnvidia-ml.so", RTLD_LAZY | RTLD_LOCAL);
    if (!this->m_nvmlLib)
        return;

    g_nvml.InitV2                 = reinterpret_cast<FnNvmlInitV2>(::dlsym(this->m_nvmlLib, "nvmlInit_v2"));
    g_nvml.Shutdown               = reinterpret_cast<FnNvmlShutdown>(::dlsym(this->m_nvmlLib, "nvmlShutdown"));
    g_nvml.DeviceGetCountV2       = reinterpret_cast<FnNvmlDeviceGetCountV2>(::dlsym(this->m_nvmlLib, "nvmlDeviceGetCount_v2"));
    g_nvml.DeviceGetHandleByIndex = reinterpret_cast<FnNvmlDeviceGetHandleByIndex>(::dlsym(this->m_nvmlLib, "nvmlDeviceGetHandleByIndex_v2"));
    g_nvml.DeviceGetName            = reinterpret_cast<FnNvmlDeviceGetName>(::dlsym(this->m_nvmlLib, "nvmlDeviceGetName"));
    g_nvml.DeviceGetTemperature     = reinterpret_cast<FnNvmlDeviceGetTemperature>(::dlsym(this->m_nvmlLib, "nvmlDeviceGetTemperature"));
    g_nvml.DeviceGetThermalSettings = reinterpret_cast<FnNvmlDeviceGetThermalSettings>(::dlsym(this->m_nvmlLib, "nvmlDeviceGetThermalSettings"));
    g_nvml.DeviceGetFieldValues     = reinterpret_cast<FnNvmlDeviceGetFieldValues>(::dlsym(this->m_nvmlLib, "nvmlDeviceGetFieldValues"));
    g_nvml.DeviceGetPowerUsage      = reinterpret_cast<FnNvmlDeviceGetPowerUsage>(::dlsym(this->m_nvmlLib, "nvmlDeviceGetPowerUsage"));
    g_nvml.DeviceGetFanSpeed      = reinterpret_cast<FnNvmlDeviceGetFanSpeed>(::dlsym(this->m_nvmlLib, "nvmlDeviceGetFanSpeed"));
    g_nvml.DeviceGetNumFans       = reinterpret_cast<FnNvmlDeviceGetNumFans>(::dlsym(this->m_nvmlLib, "nvmlDeviceGetNumFans"));
    g_nvml.DeviceGetFanSpeedRPM   = reinterpret_cast<FnNvmlDeviceGetFanSpeedRPM>(::dlsym(this->m_nvmlLib, "nvmlDeviceGetFanSpeedRPM"));
    g_nvml.DeviceGetClockInfo     = reinterpret_cast<FnNvmlDeviceGetClockInfo>(::dlsym(this->m_nvmlLib, "nvmlDeviceGetClockInfo"));
    g_nvml.DeviceGetUtilization   = reinterpret_cast<FnNvmlDeviceGetUtilization>(::dlsym(this->m_nvmlLib, "nvmlDeviceGetUtilizationRates"));
    g_nvml.DeviceGetMemoryInfo    = reinterpret_cast<FnNvmlDeviceGetMemoryInfo>(::dlsym(this->m_nvmlLib, "nvmlDeviceGetMemoryInfo"));

    if (!g_nvml.InitV2 || !g_nvml.Shutdown || !g_nvml.DeviceGetCountV2 || !g_nvml.DeviceGetHandleByIndex || !g_nvml.DeviceGetName)
    {
        this->shutdownNvml();
        return;
    }

    if (g_nvml.InitV2() != kNvmlSuccess)
    {
        this->shutdownNvml();
        return;
    }

    this->m_nvmlAvailable = true;
}

void Sensors::shutdownNvml()
{
    if (this->m_nvmlAvailable && g_nvml.Shutdown)
        g_nvml.Shutdown();

    this->m_nvmlAvailable = false;
    g_nvml = NvmlSymbols{};

    if (this->m_nvmlLib)
    {
        ::dlclose(this->m_nvmlLib);
        this->m_nvmlLib = nullptr;
    }
}

void Sensors::sampleHwmonDirIntoCategories(const QString &hwmonPath,
                                           const QString &idPrefix,
                                           const QString &chipName,
                                           QVector<SensorItem> &voltages,
                                           QVector<SensorItem> &temperatures,
                                           QVector<SensorItem> &powers,
                                           QVector<SensorItem> &fans,
                                           QVector<SensorItem> &currents,
                                           bool skipCpuVcore,
                                           bool skipCpuFan)
{
    const QDir dir(hwmonPath);
    const QString suffix = QStringLiteral("_input");

    // Voltages: in*_input (millivolts -> Volts, with board/Super I/O resistor divider scaling)
    for (const QString &inFile : sortedSensorInputs(dir, QStringLiteral("in")))
    {
        const int idx = extractSensorIndex(inFile, QStringLiteral("in"), suffix);
        if (idx < 0)
            continue;
        if (skipCpuVcore && idx == 0 && isSuperIoChip(chipName))
            continue;

        bool ok = false;
        const double milliV = Misc::ReadFile(dir.filePath(inFile)).trimmed().toDouble(&ok);
        if (!ok || milliV <= 0.0)
            continue;

        const double rawVolts = milliV / 1000.0;
        const QString sysfsLabel = Misc::ReadFile(dir.filePath(QStringLiteral("in%1_label").arg(idx))).trimmed();
        const ResolvedVoltageChannel resolved = resolveSuperIoVoltage(chipName, this->m_boardVendor, idx, rawVolts, sysfsLabel);
        if (resolved.ignore)
            continue;

        voltages.append(this->makeSensorItem(
            QStringLiteral("%1:in%2").arg(idPrefix).arg(idx),
            resolved.label,
            SensorType::Voltage,
            resolved.volts));
    }

    // Temperatures: temp*_input (millidegrees C -> degrees C)
    for (const QString &tempFile : sortedSensorInputs(dir, QStringLiteral("temp")))
    {
        const int idx = extractSensorIndex(tempFile, QStringLiteral("temp"), suffix);
        if (idx < 0)
            continue;

        bool ok = false;
        const double milliC = Misc::ReadFile(dir.filePath(tempFile)).trimmed().toDouble(&ok);
        // Filter out disconnected thermistor values (e.g. -128000, -2000, 0)
        if (!ok || milliC <= 0.0 || milliC >= 150000.0)
            continue;

        const double degC = milliC / 1000.0;
        const QString rawLabel = Misc::ReadFile(dir.filePath(QStringLiteral("temp%1_label").arg(idx))).trimmed();
        const QString label = resolveHwmonTempLabel(chipName, this->m_boardVendor, idx, rawLabel);

        temperatures.append(this->makeSensorItem(
            QStringLiteral("%1:temp%2").arg(idPrefix).arg(idx),
            label,
            SensorType::Temperature,
            degC));
    }

    // Powers: power*_input and power*_average (microwatts -> Watts)
    QStringList powerFiles = dir.entryList(QStringList() << QStringLiteral("power*_input") << QStringLiteral("power*_average"), QDir::Files, QDir::Name);
    QSet<int> seenPowerIdx;
    for (const QString &pwrFile : powerFiles)
    {
        int idx = extractSensorIndex(pwrFile, QStringLiteral("power"), QStringLiteral("_input"));
        if (idx < 0)
            idx = extractSensorIndex(pwrFile, QStringLiteral("power"), QStringLiteral("_average"));
        if (idx < 0 || seenPowerIdx.contains(idx))
            continue;

        bool ok = false;
        const double microW = Misc::ReadFile(dir.filePath(pwrFile)).trimmed().toDouble(&ok);
        if (!ok || microW <= 0.0)
            continue;

        seenPowerIdx.insert(idx);
        const double watts = microW / 1000000.0;
        QString label = Misc::ReadFile(dir.filePath(QStringLiteral("power%1_label").arg(idx))).trimmed();
        if (label.isEmpty())
            label = QStringLiteral("Package");

        powers.append(this->makeSensorItem(
            QStringLiteral("%1:power%2").arg(idPrefix).arg(idx),
            label,
            SensorType::Power,
            watts));
    }

    // Fans: fan*_input (RPM)
    for (const QString &fanFile : sortedSensorInputs(dir, QStringLiteral("fan")))
    {
        const int idx = extractSensorIndex(fanFile, QStringLiteral("fan"), suffix);
        if (idx < 0)
            continue;
        if (skipCpuFan && idx == 2 && isSuperIoChip(chipName))
            continue;

        bool ok = false;
        const double rpm = Misc::ReadFile(dir.filePath(fanFile)).trimmed().toDouble(&ok);
        const QString itemId = QStringLiteral("%1:fan%2").arg(idPrefix).arg(idx);
        if (!ok || (rpm <= 0.0 && !this->m_minMaxById.contains(itemId)))
            continue;

        const QString rawLabel = Misc::ReadFile(dir.filePath(QStringLiteral("fan%1_label").arg(idx))).trimmed();
        const QString label = resolveSuperIoFanLabel(dir, chipName, this->m_boardVendor, idx, rawLabel);

        fans.append(this->makeSensorItem(itemId, label, SensorType::Fan, qMax(0.0, rpm)));
    }

    // Currents: curr*_input (milliamps -> Amps)
    for (const QString &currFile : sortedSensorInputs(dir, QStringLiteral("curr")))
    {
        const int idx = extractSensorIndex(currFile, QStringLiteral("curr"), suffix);
        if (idx < 0)
            continue;

        bool ok = false;
        const double milliA = Misc::ReadFile(dir.filePath(currFile)).trimmed().toDouble(&ok);
        if (!ok || milliA <= 0.0)
            continue;

        const double amps = milliA / 1000.0;
        QString label = Misc::ReadFile(dir.filePath(QStringLiteral("curr%1_label").arg(idx))).trimmed();
        if (label.isEmpty())
            label = QStringLiteral("Current #%1").arg(idx);

        currents.append(this->makeSensorItem(
            QStringLiteral("%1:curr%2").arg(idPrefix).arg(idx),
            label,
            SensorType::Current,
            amps));
    }
}

void Sensors::sampleCpu(QVector<SensorDevice> &outDevices, QSet<QString> &claimedHwmonDirs, double dtSec)
{
    SensorDevice cpuDev;
    cpuDev.id   = QStringLiteral("cpu:0");
    cpuDev.name = this->m_cpuModelName;
    cpuDev.type = SensorDeviceType::Cpu;

    QVector<SensorItem> voltages;
    QVector<SensorItem> temperatures;
    QVector<SensorItem> powers;
    QVector<SensorItem> utilization;
    QVector<SensorItem> clocks;
    QVector<SensorItem> fans;
    QVector<SensorItem> currents;

    // 1. Scan CPU hwmon devices (coretemp, k10temp, zenpower, etc.)
    const QDir hwmonRoot(QStringLiteral("/sys/class/hwmon"));
    const QStringList hwmons = hwmonRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &hwmon : hwmons)
    {
        const QString hwPath   = hwmonRoot.filePath(hwmon);
        const QString chipName = Misc::ReadFile(hwPath + QStringLiteral("/name")).trimmed();
        if (!isCpuHwmonChip(chipName))
            continue;

        claimedHwmonDirs.insert(QFileInfo(hwPath).canonicalFilePath());

        const QDir dir(hwPath);
        const QString suffix = QStringLiteral("_input");

        // CPU Voltages on CPU hwmon
        for (const QString &inFile : sortedSensorInputs(dir, QStringLiteral("in")))
        {
            const int idx = extractSensorIndex(inFile, QStringLiteral("in"), suffix);
            if (idx < 0)
                continue;
            bool ok = false;
            const double milliV = Misc::ReadFile(dir.filePath(inFile)).trimmed().toDouble(&ok);
            if (!ok || milliV <= 0.0)
                continue;
            QString label = Misc::ReadFile(dir.filePath(QStringLiteral("in%1_label").arg(idx))).trimmed();
            if (label.isEmpty())
                label = QStringLiteral("VID #%1").arg(idx);
            voltages.append(this->makeSensorItem(
                QStringLiteral("cpu:0:hwmon:%1:in%2").arg(chipName).arg(idx),
                label,
                SensorType::Voltage,
                milliV / 1000.0));
        }

        // CPU Temperatures on CPU hwmon
        for (const QString &tempFile : sortedSensorInputs(dir, QStringLiteral("temp")))
        {
            const int idx = extractSensorIndex(tempFile, QStringLiteral("temp"), suffix);
            if (idx < 0)
                continue;
            bool ok = false;
            const double milliC = Misc::ReadFile(dir.filePath(tempFile)).trimmed().toDouble(&ok);
            if (!ok || milliC <= 0.0 || milliC >= 150000.0)
                continue;

            QString label = Misc::ReadFile(dir.filePath(QStringLiteral("temp%1_label").arg(idx))).trimmed();
            if (label.startsWith(QLatin1String("Package id 0"), Qt::CaseInsensitive))
                label = QStringLiteral("Package");
            else if (label.startsWith(QLatin1String("Core "), Qt::CaseInsensitive))
                label = QStringLiteral("Core #%1").arg(label.mid(5).trimmed());
            else if (label.compare(QLatin1String("Tctl"), Qt::CaseInsensitive) == 0)
                label = QStringLiteral("Package (Tctl)");
            else if (label.compare(QLatin1String("Tdie"), Qt::CaseInsensitive) == 0)
                label = QStringLiteral("Package (Tdie)");
            else if (label.startsWith(QLatin1String("Tccd"), Qt::CaseInsensitive) && label.size() > 4)
                label = QStringLiteral("CCD #%1 (%2)").arg(label.mid(4).trimmed(), label);
            else if (label.isEmpty())
                label = (idx == 1) ? QStringLiteral("Package") : QStringLiteral("Core #%1").arg(idx - 1);

            temperatures.append(this->makeSensorItem(
                QStringLiteral("cpu:0:hwmon:%1:temp%2").arg(chipName).arg(idx),
                label,
                SensorType::Temperature,
                milliC / 1000.0));
        }

        // CPU Powers on CPU hwmon
        QStringList powerFiles = dir.entryList(QStringList() << QStringLiteral("power*_input") << QStringLiteral("power*_average"), QDir::Files, QDir::Name);
        QSet<int> seenPowerIdx;
        for (const QString &pwrFile : powerFiles)
        {
            int idx = extractSensorIndex(pwrFile, QStringLiteral("power"), QStringLiteral("_input"));
            if (idx < 0)
                idx = extractSensorIndex(pwrFile, QStringLiteral("power"), QStringLiteral("_average"));
            if (idx < 0 || seenPowerIdx.contains(idx))
                continue;
            bool ok = false;
            const double microW = Misc::ReadFile(dir.filePath(pwrFile)).trimmed().toDouble(&ok);
            if (!ok || microW <= 0.0)
                continue;
            seenPowerIdx.insert(idx);
            QString label = Misc::ReadFile(dir.filePath(QStringLiteral("power%1_label").arg(idx))).trimmed();
            if (label.isEmpty())
                label = QStringLiteral("Package");
            powers.append(this->makeSensorItem(
                QStringLiteral("cpu:0:hwmon:%1:power%2").arg(chipName).arg(idx),
                label,
                SensorType::Power,
                microW / 1000000.0));
        }
    }

    // If CPU hwmon did not expose Vcore/SoC voltage, read Super I/O in0_input (Vcore) and in12_input (CPU NB/SoC)
    if (voltages.isEmpty())
    {
        for (const QString &hwmon : hwmons)
        {
            const QString hwPath   = hwmonRoot.filePath(hwmon);
            const QString chipName = Misc::ReadFile(hwPath + QStringLiteral("/name")).trimmed();
            if (!isSuperIoChip(chipName))
                continue;

            bool okVcore = false;
            const double vcoreMv = Misc::ReadFile(hwPath + QStringLiteral("/in0_input")).trimmed().toDouble(&okVcore);
            if (okVcore && vcoreMv >= 300.0 && vcoreMv <= 2000.0)
            {
                voltages.append(this->makeSensorItem(
                    QStringLiteral("cpu:0:vcore"),
                    QStringLiteral("Vcore"),
                    SensorType::Voltage,
                    vcoreMv / 1000.0));
            }

            if (this->m_isAmdCpu && isNuvotonChip(chipName))
            {
                bool okSoc = false;
                const double socMv = Misc::ReadFile(hwPath + QStringLiteral("/in12_input")).trimmed().toDouble(&okSoc);
                if (okSoc && socMv >= 800.0 && socMv <= 1350.0)
                {
                    voltages.append(this->makeSensorItem(
                        QStringLiteral("cpu:0:soc"),
                        QStringLiteral("CPU NB/SoC"),
                        SensorType::Voltage,
                        socMv / 1000.0));
                }
            }

            if (!voltages.isEmpty())
                break;
        }
    }

    const qint64 nowNs = this->m_elapsedTimer.nsecsElapsed();
    const double minDeltaSec = this->m_devices.isEmpty() ? 0.08 : 0.20;

    // 2. Sample RAPL Power zones (/sys/class/powercap/intel-rapl*)
    Q_UNUSED(dtSec);
    const double raplDtSec = (this->m_lastRaplNs >= 0 && nowNs > this->m_lastRaplNs)
                           ? (static_cast<double>(nowNs - this->m_lastRaplNs) / 1000000000.0)
                           : 0.0;
    const bool updateRaplDelta = (raplDtSec >= minDeltaSec);
    if (updateRaplDelta)
        this->m_lastRaplNs = nowNs;

    for (RaplZoneState &zone : this->m_raplZones)
    {
        if (updateRaplDelta)
        {
            bool ok = false;
            const qint64 curEnergyUj = Misc::ReadFile(zone.energyPath).trimmed().toLongLong(&ok);
            if (ok && curEnergyUj >= 0)
            {
                if (zone.lastEnergyUj >= 0 && raplDtSec < 30.0)
                {
                    qint64 deltaUj = curEnergyUj - zone.lastEnergyUj;
                    if (deltaUj < 0 && zone.maxRangeUj > 0)
                        deltaUj += zone.maxRangeUj;
                    if (deltaUj > 0)
                    {
                        zone.lastWatts = (static_cast<double>(deltaUj) / 1000000.0) / raplDtSec;
                        MinMaxRecord &rec = this->m_minMaxById[zone.id];
                        if (rec.initialized && rec.min <= 0.0)
                            rec.min = zone.lastWatts;
                    }
                }
                zone.lastEnergyUj = curEnergyUj;
            }
        }
        powers.append(this->makeSensorItem(zone.id, zone.label, SensorType::Power, zone.lastWatts));
    }

    // 3. Sample CPU Utilization from /proc/stat (Total Processor + per-core CPU #0..#N)
    const double cpuStatDtSec = (this->m_lastCpuStatNs >= 0 && nowNs > this->m_lastCpuStatNs)
                              ? (static_cast<double>(nowNs - this->m_lastCpuStatNs) / 1000000000.0)
                              : 0.0;
    const bool updateCpuStatDelta = (cpuStatDtSec >= minDeltaSec) || this->m_coreCpuTicks.isEmpty();

    if (updateCpuStatDelta)
    {
        QFile statFile(QStringLiteral("/proc/stat"));
        if (statFile.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            this->m_lastCpuStatNs = nowNs;
            int coreIdx = 0;
            for (;;)
            {
                const QByteArray raw = statFile.readLine();
                if (raw.isNull())
                    break;

                const QList<QByteArray> parts = raw.simplified().split(' ');
                if (parts.isEmpty())
                    continue;

                const QByteArray key = parts.at(0);
                if (key == "cpu")
                {
                    quint64 idleAll = 0;
                    const quint64 totalAll = parseProcStatCpuLine(parts, idleAll);
                    const quint64 dTotal   = (totalAll > this->m_totalCpuTick.prevTotal) ? (totalAll - this->m_totalCpuTick.prevTotal) : 0;
                    const quint64 dIdle    = (idleAll  > this->m_totalCpuTick.prevIdle)  ? (idleAll  - this->m_totalCpuTick.prevIdle)  : 0;

                    if (dTotal > 0 && this->m_totalCpuTick.prevTotal > 0)
                        this->m_totalCpuTick.lastPct = qBound(0.0, (1.0 - static_cast<double>(dIdle) / static_cast<double>(dTotal)) * 100.0, 100.0);

                    this->m_totalCpuTick.prevTotal = totalAll;
                    this->m_totalCpuTick.prevIdle  = idleAll;
                }
                else if (key.startsWith("cpu") && key.size() > 3)
                {
                    if (coreIdx >= this->m_coreCpuTicks.size())
                        this->m_coreCpuTicks.resize(coreIdx + 1);

                    CpuCoreTick &tick = this->m_coreCpuTicks[coreIdx];
                    quint64 idleCore = 0;
                    const quint64 totalCore = parseProcStatCpuLine(parts, idleCore);
                    const quint64 dTotal    = (totalCore > tick.prevTotal) ? (totalCore - tick.prevTotal) : 0;
                    const quint64 dIdle     = (idleCore  > tick.prevIdle)  ? (idleCore  - tick.prevIdle)  : 0;

                    if (dTotal > 0 && tick.prevTotal > 0)
                        tick.lastPct = qBound(0.0, (1.0 - static_cast<double>(dIdle) / static_cast<double>(dTotal)) * 100.0, 100.0);

                    tick.prevTotal = totalCore;
                    tick.prevIdle  = idleCore;
                    ++coreIdx;
                }
                else if (coreIdx > 0)
                {
                    break;
                }
            }
            statFile.close();
        }
    }

    if (this->m_totalCpuTick.prevTotal > 0)
    {
        utilization.append(this->makeSensorItem(
            QStringLiteral("cpu:0:util:total"),
            QStringLiteral("Processor"),
            SensorType::Utilization,
            this->m_totalCpuTick.lastPct));
    }
    for (int coreIdx = 0; coreIdx < this->m_coreCpuTicks.size(); ++coreIdx)
    {
        utilization.append(this->makeSensorItem(
            QStringLiteral("cpu:0:util:core%1").arg(coreIdx),
            QStringLiteral("CPU #%1").arg(coreIdx),
            SensorType::Utilization,
            this->m_coreCpuTicks.at(coreIdx).lastPct));
    }

    // 4. Sample Per-Core Clocks (MHz) from cpufreq sysfs with /proc/cpuinfo fallback
    const int numCores = qMax(this->m_cpuLogicalCount, static_cast<int>(this->m_coreCpuTicks.size()));
    QVector<double> coreMhz(numCores, 0.0);
    bool gotSysfsFreq = false;
    for (int i = 0; i < numCores; ++i)
    {
        const QString base = QStringLiteral("/sys/devices/system/cpu/cpu%1/cpufreq/").arg(i);
        QString freqStr = Misc::ReadFile(base + QStringLiteral("scaling_cur_freq")).trimmed();
        if (freqStr.isEmpty())
            freqStr = Misc::ReadFile(base + QStringLiteral("cpuinfo_cur_freq")).trimmed();
        bool ok = false;
        const double kHz = freqStr.toDouble(&ok);
        if (ok && kHz > 0.0)
        {
            coreMhz[i]   = kHz / 1000.0;
            gotSysfsFreq = true;
        }
    }

    if (!gotSysfsFreq)
    {
        QFile f(QStringLiteral("/proc/cpuinfo"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            int curProc = -1;
            for (;;)
            {
                const QByteArray line = f.readLine();
                if (line.isNull())
                    break;
                const int colon = line.indexOf(':');
                if (colon < 0)
                    continue;
                const QByteArray key = line.left(colon).trimmed();
                const QByteArray val = line.mid(colon + 1).trimmed();
                if (key == "processor")
                    curProc = val.toInt();
                else if (key == "cpu MHz" && curProc >= 0 && curProc < coreMhz.size())
                    coreMhz[curProc] = val.toDouble();
            }
            f.close();
        }
    }

    for (int i = 0; i < coreMhz.size(); ++i)
    {
        if (coreMhz[i] <= 0.0)
            continue;
        clocks.append(this->makeSensorItem(
            QStringLiteral("cpu:0:clock:core%1").arg(i),
            QStringLiteral("Core #%1").arg(i),
            SensorType::Clock,
            coreMhz[i]));
    }

    appendCategoryIfNotEmpty(cpuDev, cpuDev.id, SensorType::Voltage,     voltages);
    appendCategoryIfNotEmpty(cpuDev, cpuDev.id, SensorType::Temperature, temperatures);
    appendCategoryIfNotEmpty(cpuDev, cpuDev.id, SensorType::Power,       powers);
    appendCategoryIfNotEmpty(cpuDev, cpuDev.id, SensorType::Utilization, utilization);
    appendCategoryIfNotEmpty(cpuDev, cpuDev.id, SensorType::Clock,       clocks);
    appendCategoryIfNotEmpty(cpuDev, cpuDev.id, SensorType::Fan,         fans);
    appendCategoryIfNotEmpty(cpuDev, cpuDev.id, SensorType::Current,     currents);

    if (!cpuDev.categories.isEmpty())
        outDevices.append(cpuDev);
}

void Sensors::sampleGpus(QVector<SensorDevice> &outDevices, QSet<QString> &claimedHwmonDirs)
{
    bool hasNvidiaFromNvml = false;

    // 1. Sample NVIDIA GPUs via dynamic NVML when available
    if (this->m_nvmlAvailable && g_nvml.DeviceGetCountV2 && g_nvml.DeviceGetHandleByIndex)
    {
        unsigned int count = 0;
        if (g_nvml.DeviceGetCountV2(&count) == kNvmlSuccess && count > 0)
        {
            for (unsigned int i = 0; i < count; ++i)
            {
                NvmlDevice handle = nullptr;
                if (g_nvml.DeviceGetHandleByIndex(i, &handle) != kNvmlSuccess || !handle)
                    continue;

                char nameBuf[128] = {};
                if (g_nvml.DeviceGetName)
                    g_nvml.DeviceGetName(handle, nameBuf, sizeof(nameBuf));
                QString gpuName = QString::fromLatin1(nameBuf).trimmed();
                if (gpuName.isEmpty())
                    gpuName = QStringLiteral("NVIDIA GPU #%1").arg(i);

                SensorDevice gpuDev;
                gpuDev.id   = QStringLiteral("gpu:nvml:%1").arg(i);
                gpuDev.name = gpuName;
                gpuDev.type = SensorDeviceType::Gpu;

                QVector<SensorItem> temperatures;
                QVector<SensorItem> powers;
                QVector<SensorItem> fans;
                QVector<SensorItem> utilization;
                QVector<SensorItem> clocks;

                unsigned int coreTempC = 0;
                if (g_nvml.DeviceGetTemperature)
                {
                    if (g_nvml.DeviceGetTemperature(handle, 0, &coreTempC) == kNvmlSuccess && coreTempC > 0 && coreTempC < 150)
                    {
                        temperatures.append(this->makeSensorItem(
                            gpuDev.id + QStringLiteral(":temp:core"),
                            QStringLiteral("GPU"),
                            SensorType::Temperature,
                            static_cast<double>(coreTempC)));
                    }
                }

                // Probe NVML thermal settings for additional GPU thermal sensors (Hot Spot, Memory, Power Supply, Board)
                bool hasMemTemp = false;
                if (g_nvml.DeviceGetThermalSettings)
                {
                    NvmlGpuThermalSettings thermal{};
                    // 15 = NVML_THERMAL_TARGET_ALL
                    if (g_nvml.DeviceGetThermalSettings(handle, 15U, &thermal) == kNvmlSuccess && thermal.count > 0)
                    {
                        const unsigned int maxSensors = qMin(thermal.count, 3U);
                        bool sawPrimaryGpuTarget = false;
                        for (unsigned int sIdx = 0; sIdx < maxSensors; ++sIdx)
                        {
                            const NvmlThermalSensor &ts = thermal.sensor[sIdx];
                            if (ts.currentTemp <= 0 || ts.currentTemp >= 150)
                                continue;

                            if (ts.target == 1) // NVML_THERMAL_TARGET_GPU
                            {
                                if (!sawPrimaryGpuTarget && coreTempC > 0)
                                {
                                    sawPrimaryGpuTarget = true;
                                    continue;
                                }
                                temperatures.append(this->makeSensorItem(
                                    gpuDev.id + QStringLiteral(":temp:hotspot"),
                                    QStringLiteral("Hot Spot"),
                                    SensorType::Temperature,
                                    static_cast<double>(ts.currentTemp)));
                            }
                            else if (ts.target == 2) // NVML_THERMAL_TARGET_MEMORY
                            {
                                temperatures.append(this->makeSensorItem(
                                    gpuDev.id + QStringLiteral(":temp:mem"),
                                    QStringLiteral("Memory"),
                                    SensorType::Temperature,
                                    static_cast<double>(ts.currentTemp)));
                                hasMemTemp = true;
                            }
                            else if (ts.target == 4) // NVML_THERMAL_TARGET_POWERSUPPLY
                            {
                                temperatures.append(this->makeSensorItem(
                                    gpuDev.id + QStringLiteral(":temp:vrm"),
                                    QStringLiteral("VRM"),
                                    SensorType::Temperature,
                                    static_cast<double>(ts.currentTemp)));
                            }
                            else if (ts.target == 8) // NVML_THERMAL_TARGET_BOARD
                            {
                                temperatures.append(this->makeSensorItem(
                                    gpuDev.id + QStringLiteral(":temp:board"),
                                    QStringLiteral("Board"),
                                    SensorType::Temperature,
                                    static_cast<double>(ts.currentTemp)));
                            }
                        }
                    }
                }

                // Fallback probe for GDDR6X / HBM memory temperature via NVML_FI_DEV_MEMORY_TEMP (82)
                if (!hasMemTemp && g_nvml.DeviceGetFieldValues)
                {
                    NvmlFieldValue fv{};
                    fv.fieldId = 82U; // NVML_FI_DEV_MEMORY_TEMP
                    if (g_nvml.DeviceGetFieldValues(handle, 1, &fv) == kNvmlSuccess && fv.nvmlReturn == 0)
                    {
                        unsigned int memTempC = 0;
                        ::memcpy(&memTempC, fv.value, sizeof(memTempC));
                        if (memTempC > 0 && memTempC < 150)
                        {
                            temperatures.append(this->makeSensorItem(
                                gpuDev.id + QStringLiteral(":temp:mem"),
                                QStringLiteral("Memory"),
                                SensorType::Temperature,
                                static_cast<double>(memTempC)));
                        }
                    }
                }

                if (g_nvml.DeviceGetPowerUsage)
                {
                    unsigned int powerMilliW = 0;
                    if (g_nvml.DeviceGetPowerUsage(handle, &powerMilliW) == kNvmlSuccess && powerMilliW > 0)
                    {
                        powers.append(this->makeSensorItem(
                            gpuDev.id + QStringLiteral(":power:pkg"),
                            QStringLiteral("Package"),
                            SensorType::Power,
                            static_cast<double>(powerMilliW) / 1000.0));
                    }
                }

                if (g_nvml.DeviceGetUtilization)
                {
                    NvmlUtilization util{};
                    if (g_nvml.DeviceGetUtilization(handle, &util) == kNvmlSuccess)
                    {
                        utilization.append(this->makeSensorItem(
                            gpuDev.id + QStringLiteral(":util:gpu"),
                            QStringLiteral("GPU"),
                            SensorType::Utilization,
                            qBound(0.0, static_cast<double>(util.gpu), 100.0)));
                        utilization.append(this->makeSensorItem(
                            gpuDev.id + QStringLiteral(":util:mem_ctrl"),
                            QStringLiteral("Memory Controller"),
                            SensorType::Utilization,
                            qBound(0.0, static_cast<double>(util.memory), 100.0)));
                    }
                }

                if (g_nvml.DeviceGetMemoryInfo)
                {
                    NvmlMemory mem{};
                    if (g_nvml.DeviceGetMemoryInfo(handle, &mem) == kNvmlSuccess && mem.total > 0)
                    {
                        const double memPct = static_cast<double>(mem.used) * 100.0 / static_cast<double>(mem.total);
                        utilization.append(this->makeSensorItem(
                            gpuDev.id + QStringLiteral(":util:vram"),
                            QStringLiteral("Frame Buffer"),
                            SensorType::Utilization,
                            qBound(0.0, memPct, 100.0)));
                    }
                }

                if (g_nvml.DeviceGetFanSpeedRPM)
                {
                    unsigned int numFans = 1;
                    if (g_nvml.DeviceGetNumFans)
                    {
                        unsigned int n = 0;
                        if (g_nvml.DeviceGetNumFans(handle, &n) == kNvmlSuccess && n > 0)
                            numFans = n;
                    }
                    for (unsigned int f = 0; f < numFans; ++f)
                    {
                        NvmlFanSpeedInfoV1 fanInfo{};
                        fanInfo.version = static_cast<unsigned int>(sizeof(NvmlFanSpeedInfoV1) | (1U << 24U));
                        fanInfo.fan     = f;
                        if (g_nvml.DeviceGetFanSpeedRPM(handle, &fanInfo) == kNvmlSuccess)
                        {
                            const QString fanLabel = (numFans == 1)
                                ? QStringLiteral("GPU")
                                : QStringLiteral("GPU #%1").arg(f);
                            fans.append(this->makeSensorItem(
                                QStringLiteral("%1:fan:%2").arg(gpuDev.id).arg(f),
                                fanLabel,
                                SensorType::Fan,
                                static_cast<double>(fanInfo.speed)));
                        }
                    }
                }

                if (g_nvml.DeviceGetFanSpeed)
                {
                    unsigned int fanPct = 0;
                    if (g_nvml.DeviceGetFanSpeed(handle, &fanPct) == kNvmlSuccess)
                    {
                        fans.append(this->makeSensorItem(
                            gpuDev.id + QStringLiteral(":fan:pwm"),
                            QStringLiteral("Fan PWM"),
                            SensorType::Pwm,
                            qBound(0.0, static_cast<double>(fanPct), 100.0)));
                    }
                }

                if (g_nvml.DeviceGetClockInfo)
                {
                    unsigned int gfxMhz = 0;
                    if (g_nvml.DeviceGetClockInfo(handle, 0, &gfxMhz) == kNvmlSuccess && gfxMhz > 0)
                    {
                        clocks.append(this->makeSensorItem(
                            gpuDev.id + QStringLiteral(":clock:gfx"),
                            QStringLiteral("Graphics"),
                            SensorType::Clock,
                            static_cast<double>(gfxMhz)));
                    }
                    unsigned int memMhz = 0;
                    if (g_nvml.DeviceGetClockInfo(handle, 2, &memMhz) == kNvmlSuccess && memMhz > 0)
                    {
                        clocks.append(this->makeSensorItem(
                            gpuDev.id + QStringLiteral(":clock:mem"),
                            QStringLiteral("Memory"),
                            SensorType::Clock,
                            static_cast<double>(memMhz)));
                    }
                    unsigned int vidMhz = 0;
                    if (g_nvml.DeviceGetClockInfo(handle, 3, &vidMhz) == kNvmlSuccess && vidMhz > 0)
                    {
                        clocks.append(this->makeSensorItem(
                            gpuDev.id + QStringLiteral(":clock:vid"),
                            QStringLiteral("Video"),
                            SensorType::Clock,
                            static_cast<double>(vidMhz)));
                    }
                }

                appendCategoryIfNotEmpty(gpuDev, gpuDev.id, SensorType::Temperature, temperatures);
                appendCategoryIfNotEmpty(gpuDev, gpuDev.id, SensorType::Power,       powers);
                appendCategoryIfNotEmpty(gpuDev, gpuDev.id, SensorType::Fan,         fans);
                appendCategoryIfNotEmpty(gpuDev, gpuDev.id, SensorType::Utilization, utilization);
                appendCategoryIfNotEmpty(gpuDev, gpuDev.id, SensorType::Clock,       clocks);

                if (!gpuDev.categories.isEmpty())
                {
                    outDevices.append(gpuDev);
                    hasNvidiaFromNvml = true;
                }
            }
        }
    }

    // 2. Scan /sys/class/drm/card* for AMD, Intel, and non-NVML GPUs
    const QDir drmDir(QStringLiteral("/sys/class/drm"));
    const QStringList cardEntries = drmDir.entryList({QStringLiteral("card[0-9]*")}, QDir::Dirs, QDir::Name);
    for (const QString &entry : cardEntries)
    {
        if (entry.contains(QLatin1Char('-')))
            continue;

        const QString devPath   = drmDir.filePath(entry + QStringLiteral("/device"));
        const QString vendorStr = Misc::ReadFile(devPath + QStringLiteral("/vendor")).trimmed().toLower();
        if (hasNvidiaFromNvml && vendorStr == QLatin1String("0x10de"))
        {
            const QStringList hwmons = QDir(devPath + QStringLiteral("/hwmon")).entryList({QStringLiteral("hwmon[0-9]*")}, QDir::Dirs);
            for (const QString &hm : hwmons)
                claimedHwmonDirs.insert(QFileInfo(devPath + QStringLiteral("/hwmon/") + hm).canonicalFilePath());
            continue;
        }

        SensorDevice gpuDev;
        gpuDev.id   = QStringLiteral("gpu:drm:") + entry;
        gpuDev.type = SensorDeviceType::Gpu;

        if (vendorStr == QLatin1String("0x1002"))
            gpuDev.name = QStringLiteral("AMD Radeon GPU (%1)").arg(entry);
        else if (vendorStr == QLatin1String("0x8086"))
            gpuDev.name = QStringLiteral("Intel Graphics (%1)").arg(entry);
        else if (vendorStr == QLatin1String("0x10de"))
            gpuDev.name = QStringLiteral("NVIDIA GPU (%1)").arg(entry);
        else
            gpuDev.name = QStringLiteral("GPU (%1)").arg(entry);

        QVector<SensorItem> voltages;
        QVector<SensorItem> temperatures;
        QVector<SensorItem> powers;
        QVector<SensorItem> fans;
        QVector<SensorItem> currents;
        QVector<SensorItem> utilization;
        QVector<SensorItem> clocks;

        const QStringList hwmons = QDir(devPath + QStringLiteral("/hwmon")).entryList({QStringLiteral("hwmon[0-9]*")}, QDir::Dirs, QDir::Name);
        for (const QString &hm : hwmons)
        {
            const QString hwPath = devPath + QStringLiteral("/hwmon/") + hm;
            claimedHwmonDirs.insert(QFileInfo(hwPath).canonicalFilePath());
            const QString chipName = Misc::ReadFile(hwPath + QStringLiteral("/name")).trimmed();

            this->sampleHwmonDirIntoCategories(
                hwPath,
                gpuDev.id + QLatin1Char(':') + hm,
                chipName,
                voltages,
                temperatures,
                powers,
                fans,
                currents);

            // Check pwm1 (0..255 -> 0..100%) on DRM GPU hwmon
            const QDir hwDir(hwPath);
            bool okPwm = false;
            const double pwmRaw = Misc::ReadFile(hwDir.filePath(QStringLiteral("pwm1"))).trimmed().toDouble(&okPwm);
            if (okPwm && pwmRaw >= 0.0 && pwmRaw <= 255.0)
            {
                bool okMax = false;
                double pwmMax = Misc::ReadFile(hwDir.filePath(QStringLiteral("pwm1_max"))).trimmed().toDouble(&okMax);
                if (!okMax || pwmMax <= 0.0)
                    pwmMax = 255.0;
                fans.append(this->makeSensorItem(
                    QStringLiteral("%1:%2:pwm1").arg(gpuDev.id, hm),
                    QStringLiteral("Fan PWM"),
                    SensorType::Pwm,
                    qBound(0.0, pwmRaw * 100.0 / pwmMax, 100.0)));
            }

            // Check freq*_input (in Hz -> MHz) on amdgpu hwmon
            for (const QString &freqFile : sortedSensorInputs(hwDir, QStringLiteral("freq")))
            {
                const int idx = extractSensorIndex(freqFile, QStringLiteral("freq"), QStringLiteral("_input"));
                if (idx < 0)
                    continue;
                bool ok = false;
                const double hz = Misc::ReadFile(hwDir.filePath(freqFile)).trimmed().toDouble(&ok);
                if (!ok || hz <= 0.0)
                    continue;
                QString label = Misc::ReadFile(hwDir.filePath(QStringLiteral("freq%1_label").arg(idx))).trimmed();
                if (label.compare(QLatin1String("sclk"), Qt::CaseInsensitive) == 0)
                    label = QStringLiteral("Graphics");
                else if (label.compare(QLatin1String("mclk"), Qt::CaseInsensitive) == 0)
                    label = QStringLiteral("Memory");
                else if (label.isEmpty())
                    label = QStringLiteral("Clock #%1").arg(idx);

                clocks.append(this->makeSensorItem(
                    QStringLiteral("%1:freq%2").arg(gpuDev.id).arg(idx),
                    label,
                    SensorType::Clock,
                    hz / 1000000.0));
            }
        }

        // DRM busy percentage
        bool okBusy = false;
        const double busyPct = Misc::ReadFile(devPath + QStringLiteral("/gpu_busy_percent")).trimmed().toDouble(&okBusy);
        if (okBusy)
        {
            utilization.append(this->makeSensorItem(
                gpuDev.id + QStringLiteral(":util:gpu"),
                QStringLiteral("GPU"),
                SensorType::Utilization,
                qBound(0.0, busyPct, 100.0)));
        }

        // DRM VRAM usage %
        bool okTotal = false;
        bool okUsed  = false;
        const qint64 vramTotal = Misc::ReadFile(devPath + QStringLiteral("/mem_info_vram_total")).trimmed().toLongLong(&okTotal);
        const qint64 vramUsed  = Misc::ReadFile(devPath + QStringLiteral("/mem_info_vram_used")).trimmed().toLongLong(&okUsed);
        if (okTotal && okUsed && vramTotal > 0)
        {
            const double vramPct = static_cast<double>(vramUsed) * 100.0 / static_cast<double>(vramTotal);
            utilization.append(this->makeSensorItem(
                gpuDev.id + QStringLiteral(":util:vram"),
                QStringLiteral("Memory"),
                SensorType::Utilization,
                qBound(0.0, vramPct, 100.0)));
        }

        // Intel GT frequency fallback
        if (clocks.isEmpty())
        {
            bool okFreq = false;
            double mhz = Misc::ReadFile(drmDir.filePath(entry + QStringLiteral("/gt_act_freq_mhz"))).trimmed().toDouble(&okFreq);
            if (!okFreq || mhz <= 0.0)
                mhz = Misc::ReadFile(drmDir.filePath(entry + QStringLiteral("/gt_cur_freq_mhz"))).trimmed().toDouble(&okFreq);
            if (okFreq && mhz > 0.0)
            {
                clocks.append(this->makeSensorItem(
                    gpuDev.id + QStringLiteral(":clock:gt"),
                    QStringLiteral("Graphics"),
                    SensorType::Clock,
                    mhz));
            }
        }

        appendCategoryIfNotEmpty(gpuDev, gpuDev.id, SensorType::Voltage,     voltages);
        appendCategoryIfNotEmpty(gpuDev, gpuDev.id, SensorType::Temperature, temperatures);
        appendCategoryIfNotEmpty(gpuDev, gpuDev.id, SensorType::Power,       powers);
        appendCategoryIfNotEmpty(gpuDev, gpuDev.id, SensorType::Fan,         fans);
        appendCategoryIfNotEmpty(gpuDev, gpuDev.id, SensorType::Utilization, utilization);
        appendCategoryIfNotEmpty(gpuDev, gpuDev.id, SensorType::Clock,       clocks);
        appendCategoryIfNotEmpty(gpuDev, gpuDev.id, SensorType::Current,     currents);

        if (!gpuDev.categories.isEmpty())
            outDevices.append(gpuDev);
    }
}

void Sensors::sampleDisks(QVector<SensorDevice> &outDevices, QSet<QString> &claimedHwmonDirs, qint64 dtMs)
{
    Q_UNUSED(dtMs);

    // 1. Parse /proc/self/mountinfo to map mounted filesystems to physical base block devices
    QHash<QString, QList<QPair<QString, double>>> mountsByBaseDisk;
    QSet<QString> seenMountPoints;
    QFile mountFile(QStringLiteral("/proc/self/mountinfo"));
    if (mountFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        for (;;)
        {
            const QByteArray line = mountFile.readLine();
            if (line.isNull())
                break;

            const QList<QByteArray> parts = line.trimmed().split(' ');
            const int sepIdx = parts.indexOf("-");
            if (sepIdx < 0 || sepIdx + 2 >= parts.size())
                continue;

            const QString source = QString::fromUtf8(parts.at(sepIdx + 2));
            if (!source.startsWith(QLatin1String("/dev/")))
                continue;

            const QString mountPoint = (parts.size() > 4) ? QString::fromUtf8(parts.at(4)) : QString();
            if (mountPoint.isEmpty() || seenMountPoints.contains(mountPoint))
                continue;
            if (mountPoint.startsWith(QLatin1String("/snap/")) || mountPoint.startsWith(QLatin1String("/run/")))
                continue;

            struct statvfs vfs{};
            if (::statvfs(mountPoint.toUtf8().constData(), &vfs) != 0 || vfs.f_blocks == 0)
                continue;

            // Match POSIX/GNU df percentage: ceil(used * 100.0 / (used + bavail))
            const quint64 usedBlocks    = (vfs.f_blocks >= vfs.f_bfree) ? (vfs.f_blocks - vfs.f_bfree) : 0;
            const quint64 nonRootBlocks = usedBlocks + vfs.f_bavail;
            if (nonRootBlocks == 0)
                continue;

            const double usedPct = qBound(0.0, static_cast<double>(qCeil(static_cast<double>(usedBlocks) * 100.0 / static_cast<double>(nonRootBlocks))), 100.0);
            seenMountPoints.insert(mountPoint);

            const QString rawDev = QFileInfo(source).fileName();
            const QSet<QString> baseDevs = resolveBaseBlockDevices(rawDev);
            for (const QString &base : baseDevs)
                mountsByBaseDisk[base].append(qMakePair(mountPoint, usedPct));
        }
        mountFile.close();
    }

    // 2. Parse /proc/diskstats for disk activity %
    const qint64 nowNs = this->m_elapsedTimer.nsecsElapsed();
    const double minDeltaSec = this->m_devices.isEmpty() ? 0.08 : 0.20;
    const double diskDtSec = (this->m_lastDiskIoNs >= 0 && nowNs > this->m_lastDiskIoNs)
                           ? (static_cast<double>(nowNs - this->m_lastDiskIoNs) / 1000000000.0)
                           : 0.0;
    const bool updateDiskIoDelta = (diskDtSec >= minDeltaSec) || this->m_diskIoStates.isEmpty();

    if (updateDiskIoDelta)
    {
        QFile diskStatsFile(QStringLiteral("/proc/diskstats"));
        if (diskStatsFile.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            this->m_lastDiskIoNs = nowNs;
            const double dtMsExact = diskDtSec * 1000.0;
            for (;;)
            {
                const QByteArray line = diskStatsFile.readLine();
                if (line.isNull())
                    break;
                const QList<QByteArray> parts = line.simplified().split(' ');
                if (parts.size() < 14)
                    continue;

                const QString devName      = QString::fromUtf8(parts.at(2));
                const quint64 readSectors  = parts.at(5).toULongLong();
                const quint64 writeSectors = parts.at(9).toULongLong();
                const quint64 ioMs         = parts.at(12).toULongLong();

                DiskIoState &st = this->m_diskIoStates[devName];
                if (st.initialized && dtMsExact > 0.0)
                {
                    const quint64 dIoMs = (ioMs >= st.prevIoMs) ? (ioMs - st.prevIoMs) : 0;
                    st.lastActivePct = qBound(0.0, static_cast<double>(dIoMs) * 100.0 / dtMsExact, 100.0);
                }
                st.prevReadSectors  = readSectors;
                st.prevWriteSectors = writeSectors;
                st.prevIoMs         = ioMs;
                st.initialized      = true;
            }
            diskStatsFile.close();
        }
    }

    QHash<QString, double> activePctByDisk;
    for (auto it = this->m_diskIoStates.cbegin(); it != this->m_diskIoStates.cend(); ++it)
        activePctByDisk.insert(it.key(), it.value().lastActivePct);

    // Pre-scan all hwmon directories and their canonical device paths for matching against block devices
    struct HwmonCandidate
    {
        QString hwmonPath;
        QString canonicalDevicePath;
        QString chipName;
    };
    QVector<HwmonCandidate> hwmonCandidates;
    const QDir hwmonRoot(QStringLiteral("/sys/class/hwmon"));
    for (const QString &hm : hwmonRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
    {
        const QString hwPath = hwmonRoot.filePath(hm);
        HwmonCandidate cand;
        cand.hwmonPath           = QFileInfo(hwPath).canonicalFilePath();
        cand.canonicalDevicePath = QFileInfo(hwPath + QStringLiteral("/device")).canonicalFilePath();
        cand.chipName            = Misc::ReadFile(hwPath + QStringLiteral("/name")).trimmed();
        hwmonCandidates.append(cand);
    }

    const qint64 nowMs = this->m_elapsedTimer.elapsed();
    const QDir sysBlockDir(QStringLiteral("/sys/block"));
    const QStringList blockEntries = sysBlockDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    for (const QString &devName : blockEntries)
    {
        if (shouldIgnoreBlockDevice(devName))
            continue;

        const QString blockPath          = sysBlockDir.filePath(devName);
        const QString canonicalBlockPath = QFileInfo(blockPath).canonicalFilePath();
        QString model                    = Misc::ReadFile(blockPath + QStringLiteral("/device/model")).trimmed();
        const UDisks2DriveCache udisks   = queryUDisks2ForBlock(devName, nowMs);
        if (udisks.model.size() > model.size())
            model = udisks.model;
        if (model.isEmpty())
            model = devName;

        SensorDevice diskDev;
        diskDev.id   = QStringLiteral("disk:") + devName;
        diskDev.name = model;
        diskDev.type = SensorDeviceType::Disk;

        QVector<SensorItem> temperatures;
        QVector<SensorItem> utilization;

        // Match any hwmon device belonging to this block device (e.g. nvme, drivetemp)
        for (const HwmonCandidate &cand : std::as_const(hwmonCandidates))
        {
            if (cand.canonicalDevicePath.isEmpty())
                continue;
            if (canonicalBlockPath.startsWith(cand.canonicalDevicePath + QLatin1Char('/'))
                || cand.canonicalDevicePath.startsWith(canonicalBlockPath + QLatin1Char('/'))
                || canonicalBlockPath == cand.canonicalDevicePath)
            {
                claimedHwmonDirs.insert(cand.hwmonPath);
                const QDir dir(cand.hwmonPath);
                const QString suffix = QStringLiteral("_input");
                const QStringList tempInputs = sortedSensorInputs(dir, QStringLiteral("temp"));
                for (const QString &tempFile : tempInputs)
                {
                    const int idx = extractSensorIndex(tempFile, QStringLiteral("temp"), suffix);
                    if (idx < 0)
                        continue;
                    bool ok = false;
                    const double milliC = Misc::ReadFile(dir.filePath(tempFile)).trimmed().toDouble(&ok);
                    if (!ok || milliC <= 0.0 || milliC >= 150000.0)
                        continue;

                    QString label = Misc::ReadFile(dir.filePath(QStringLiteral("temp%1_label").arg(idx))).trimmed();
                    if (label.isEmpty() || (tempInputs.size() == 1 && label.compare(QLatin1String("Composite"), Qt::CaseInsensitive) == 0))
                        label = QStringLiteral("Assembly");

                    temperatures.append(this->makeSensorItem(
                        QStringLiteral("%1:temp%2").arg(diskDev.id).arg(idx),
                        label,
                        SensorType::Temperature,
                        milliC / 1000.0));
                }
            }
        }

        // Fallback to UDisks2 SMART temperature if no hwmon temperature sensor was exposed
        if (temperatures.isEmpty() && udisks.temperatureC > 0.0)
        {
            temperatures.append(this->makeSensorItem(
                diskDev.id + QStringLiteral(":temp:smart"),
                QStringLiteral("Assembly"),
                SensorType::Temperature,
                udisks.temperatureC));
        }

        // Mounted partition space utilization
        const auto mounts = mountsByBaseDisk.value(devName);
        for (const auto &mnt : mounts)
        {
            utilization.append(this->makeSensorItem(
                QStringLiteral("%1:space:%2").arg(diskDev.id, mnt.first),
                QStringLiteral("Space (%1)").arg(mnt.first),
                SensorType::Utilization,
                mnt.second));
        }

        // Disk I/O Activity %
        if (activePctByDisk.contains(devName))
        {
            utilization.append(this->makeSensorItem(
                diskDev.id + QStringLiteral(":util:activity"),
                QStringLiteral("Activity"),
                SensorType::Utilization,
                activePctByDisk.value(devName)));
        }

        appendCategoryIfNotEmpty(diskDev, diskDev.id, SensorType::Temperature, temperatures);
        appendCategoryIfNotEmpty(diskDev, diskDev.id, SensorType::Utilization, utilization);

        if (!diskDev.categories.isEmpty())
            outDevices.append(diskDev);
    }
}

void Sensors::sampleBatteries(QVector<SensorDevice> &outDevices, QSet<QString> &claimedHwmonDirs)
{
    Q_UNUSED(claimedHwmonDirs);

    const QDir powerSupplyDir(QStringLiteral("/sys/class/power_supply"));
    if (!powerSupplyDir.exists())
        return;

    const QStringList entries = powerSupplyDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries)
    {
        const QString sysfsPath = powerSupplyDir.filePath(entry);
        const QString type      = Misc::ReadFile(sysfsPath + QStringLiteral("/type")).trimmed();
        if (type != QLatin1String("Battery"))
            continue;

        const QString presentStr = Misc::ReadFile(sysfsPath + QStringLiteral("/present")).trimmed();
        if (presentStr == QLatin1String("0"))
            continue;

        const QString vendor = Misc::ReadFile(sysfsPath + QStringLiteral("/manufacturer")).trimmed();
        const QString model  = Misc::ReadFile(sysfsPath + QStringLiteral("/model_name")).trimmed();
        QString displayName  = (vendor + QLatin1Char(' ') + model).trimmed();
        if (displayName.isEmpty())
            displayName = entry;
        else
            displayName = QStringLiteral("%1 (%2)").arg(displayName, entry);

        SensorDevice batDev;
        batDev.id   = QStringLiteral("bat:") + entry;
        batDev.name = displayName;
        batDev.type = SensorDeviceType::Battery;

        QVector<SensorItem> voltages;
        QVector<SensorItem> powers;
        QVector<SensorItem> currents;
        QVector<SensorItem> utilization;

        bool okVolt = false;
        double voltUv = Misc::ReadFile(sysfsPath + QStringLiteral("/voltage_now")).trimmed().toDouble(&okVolt);
        if (!okVolt || voltUv <= 0.0)
            voltUv = Misc::ReadFile(sysfsPath + QStringLiteral("/voltage_min_design")).trimmed().toDouble(&okVolt);
        const double volts = (okVolt && voltUv > 0.0) ? (voltUv / 1000000.0) : 0.0;
        if (volts > 0.0)
        {
            voltages.append(this->makeSensorItem(
                batDev.id + QStringLiteral(":voltage"),
                QStringLiteral("Battery Voltage"),
                SensorType::Voltage,
                volts));
        }

        bool okPwr = false;
        const double pwrUw = Misc::ReadFile(sysfsPath + QStringLiteral("/power_now")).trimmed().toDouble(&okPwr);
        bool okCur = false;
        const double curUa = Misc::ReadFile(sysfsPath + QStringLiteral("/current_now")).trimmed().toDouble(&okCur);

        if (okPwr && pwrUw >= 0.0)
        {
            powers.append(this->makeSensorItem(
                batDev.id + QStringLiteral(":power"),
                QStringLiteral("Charge/Discharge Rate"),
                SensorType::Power,
                pwrUw / 1000000.0));
        }
        else if (okCur && curUa >= 0.0 && volts > 0.0)
        {
            powers.append(this->makeSensorItem(
                batDev.id + QStringLiteral(":power"),
                QStringLiteral("Charge/Discharge Rate"),
                SensorType::Power,
                (curUa / 1000000.0) * volts));
        }

        if (okCur && curUa > 0.0)
        {
            currents.append(this->makeSensorItem(
                batDev.id + QStringLiteral(":current"),
                QStringLiteral("Current"),
                SensorType::Current,
                curUa / 1000000.0));
        }

        bool okCap = false;
        const double capPct = Misc::ReadFile(sysfsPath + QStringLiteral("/capacity")).trimmed().toDouble(&okCap);
        if (okCap)
        {
            utilization.append(this->makeSensorItem(
                batDev.id + QStringLiteral(":capacity"),
                QStringLiteral("Charge Level"),
                SensorType::Utilization,
                qBound(0.0, capPct, 100.0)));
        }

        appendCategoryIfNotEmpty(batDev, batDev.id, SensorType::Voltage,     voltages);
        appendCategoryIfNotEmpty(batDev, batDev.id, SensorType::Power,       powers);
        appendCategoryIfNotEmpty(batDev, batDev.id, SensorType::Current,     currents);
        appendCategoryIfNotEmpty(batDev, batDev.id, SensorType::Utilization, utilization);

        if (!batDev.categories.isEmpty())
            outDevices.append(batDev);
    }
}

void Sensors::sampleMotherboardAndMisc(QVector<SensorDevice> &outDevices, QSet<QString> &claimedHwmonDirs)
{
    SensorDevice mbDev;
    mbDev.id   = QStringLiteral("mb:0");
    mbDev.name = this->m_boardDisplayName;
    mbDev.type = SensorDeviceType::Motherboard;

    QVector<SensorItem> voltages;
    QVector<SensorItem> temperatures;
    QVector<SensorItem> powers;
    QVector<SensorItem> fans;
    QVector<SensorItem> currents;

    const QDir hwmonRoot(QStringLiteral("/sys/class/hwmon"));
    const QStringList hwmons = hwmonRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    QSet<QString> seenThermalSensorValues;

    for (const QString &hwmon : hwmons)
    {
        const QString hwPath        = hwmonRoot.filePath(hwmon);
        const QString canonicalPath = QFileInfo(hwPath).canonicalFilePath();
        if (claimedHwmonDirs.contains(canonicalPath))
            continue;

        claimedHwmonDirs.insert(canonicalPath);
        const QString chipName = Misc::ReadFile(hwPath + QStringLiteral("/name")).trimmed();

        this->sampleHwmonDirIntoCategories(
            hwPath,
            QStringLiteral("mb:0:") + (chipName.isEmpty() ? hwmon : chipName),
            chipName,
            voltages,
            temperatures,
            powers,
            fans,
            currents);

        if (!chipName.isEmpty())
            seenThermalSensorValues.insert(chipName.toLower());
    }

    // Also inspect /sys/class/thermal/thermal_zone* for ACPI / chipset temperatures not already in hwmon
    const QDir thermalRoot(QStringLiteral("/sys/class/thermal"));
    if (thermalRoot.exists())
    {
        const QStringList zones = thermalRoot.entryList({QStringLiteral("thermal_zone[0-9]*")}, QDir::Dirs, QDir::Name);
        for (const QString &zone : zones)
        {
            const QString zonePath = thermalRoot.filePath(zone);
            const QString zoneType = Misc::ReadFile(zonePath + QStringLiteral("/type")).trimmed();
            if (!zoneType.isEmpty() && seenThermalSensorValues.contains(zoneType.toLower()))
                continue;

            bool ok = false;
            const double milliC = Misc::ReadFile(zonePath + QStringLiteral("/temp")).trimmed().toDouble(&ok);
            if (!ok || milliC <= 0.0 || milliC >= 150000.0)
                continue;

            temperatures.append(this->makeSensorItem(
                QStringLiteral("mb:0:thermal:") + zone,
                zoneType.isEmpty() ? zone.toUpper() : zoneType,
                SensorType::Temperature,
                milliC / 1000.0));
        }
    }

    appendCategoryIfNotEmpty(mbDev, mbDev.id, SensorType::Voltage,     voltages);
    appendCategoryIfNotEmpty(mbDev, mbDev.id, SensorType::Temperature, temperatures);
    appendCategoryIfNotEmpty(mbDev, mbDev.id, SensorType::Power,       powers);
    appendCategoryIfNotEmpty(mbDev, mbDev.id, SensorType::Fan,         fans);
    appendCategoryIfNotEmpty(mbDev, mbDev.id, SensorType::Current,     currents);

    if (!mbDev.categories.isEmpty())
        outDevices.append(mbDev);
}
