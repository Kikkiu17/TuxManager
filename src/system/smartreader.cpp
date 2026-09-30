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

#include "smartreader.h"
#include "../misc.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QtMath>
#include <QMap>

using namespace System;

namespace
{
    struct AtaAttrInfo
    {
        const char *name;
        const char *displayName;
    };

    const QMap<QString, QString> &getAtaNameMap()
    {
        static const QMap<QString, QString> s_map = {
            { "raw-read-error-rate", "Read Error Rate" },
            { "throughput-performance", "Throughput Performance" },
            { "spin-up-time", "Spin-Up Time" },
            { "start-stop-count", "Start/Stop Count" },
            { "reallocated-sector-count", "Reallocated Sector Count" },
            { "read-channel-margin", "Read Channel Margin" },
            { "seek-error-rate", "Seek Error Rate" },
            { "seek-time-performance", "Seek Time Performance" },
            { "power-on-hours", "Power-On Hours" },
            { "spin-retry-count", "Spin-Up Retry Count" },
            { "calibration-retry-count", "Calibration Retry Count" },
            { "power-cycle-count", "Power Cycle Count" },
            { "read-soft-error-rate", "Soft Read Error Rate" },
            { "reported-uncorrect", "Reported Uncorrectable Errors" },
            { "high-fly-writes", "High Fly Writes" },
            { "airflow-temperature-celsius", "Airflow Temperature" },
            { "g-sense-error-rate", "G-Sense Error Rate" },
            { "power-off-retract-count", "Power-Off Retract Count" },
            { "load-cycle-count", "Load/Unload Cycle Count" },
            { "temperature-celsius-2", "Temperature" },
            { "temperature-celsius", "Temperature" },
            { "hardware-ecc-recovered", "Hardware ECC Recovered" },
            { "reallocated-event-count", "Reallocation Event Count" },
            { "current-pending-sector", "Current Pending Sector Count" },
            { "offline-uncorrectable", "Offline Uncorrectable Sector Count" },
            { "udma-crc-error-count", "UDMA CRC Error Count" },
            { "multi-zone-error-rate", "Write Error Rate" },
            { "soft-read-error-rate", "Soft Read Error Rate" },
            { "ta-increase-count", "Data Address Mark Errors" },
            { "run-out-cancel", "Run Out Cancel" },
            { "shock-count-write-open", "Soft ECC Correction" },
            { "shock-rate-write-open", "Thermal Asperity Rate" },
            { "flying-height", "Flying Height" },
            { "spin-high-current", "Spin High Current" },
            { "spin-buzz", "Spin Buzz" },
            { "offline-seek-performance", "Offline Seek Performance" },
            { "disk-shift", "Disk Shift" },
            { "g-sense-error-rate-2", "G-Sense Error Rate" },
            { "loaded-hours", "Loaded Hours" },
            { "load-retry-count", "Load/Unload Retry Count" },
            { "load-friction", "Load Friction" },
            { "load-cycle-count-2", "Load/Unload Cycle Count" },
            { "load-in-time", "Load-In Time" },
            { "torq-amp-count", "Torque Amplification Count" },
            { "power-off-retract-count-2", "Power-Off Retract Count" },
            { "head-amplitude", "GMR Head Amplitude" },
            { "endurance-remaining", "Endurance Remaining" },
            { "power-on-seconds-2", "Power-On Hours" },
            { "uncorrectable-ecc-count", "Uncorrectable ECC Count" },
            { "good-block-rate", "Good Block Rate" },
            { "head-flying-hours", "Head Flying Hours" },
            { "read-error-retry-rate", "Read Error Retry Rate" },
            { "total-lbas-written", "Total Data Written (TBW)" },
            { "total-lbas-read", "Total Data Read (TBR)" },
            { "command-timeout", "Command Timeout" },
            { "runtime-bad-block-total", "Runtime Bad Block Total" },
            { "end-to-end-error", "End-to-End Error" }
        };
        return s_map;
    }

    QString formatAtaAttributeName(const QString &rawName, int id)
    {
        if (id == 241 || rawName == "total-lbas-written" || rawName == "host-writes-gib" || rawName == "lifetime-writes-gib")
            return "Total Data Written (TBW)";
        if (id == 242 || rawName == "total-lbas-read" || rawName == "host-reads-gib" || rawName == "lifetime-reads-gib")
            return "Total Data Read (TBR)";

        const auto &map = getAtaNameMap();
        if (map.contains(rawName))
            return map.value(rawName);

        if (rawName.startsWith("attribute-"))
            return QString("Attribute %1").arg(rawName.mid(10));

        // Fallback: title case replacing hyphens with spaces
        QStringList words = rawName.split('-');
        for (QString &word : words)
        {
            if (!word.isEmpty())
                word[0] = word[0].toUpper();
        }
        QString result = words.join(' ');
        if (result.isEmpty())
            result = QString("Attribute %1").arg(id);
        return result;
    }

    QString formatTiBBytes(quint64 bytes)
    {
        if (bytes == 0)
            return "0 B";

        const double tib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0 * 1024.0);
        if (tib >= 1.0)
        {
            if (tib >= 100.0)
                return QString("%1 TiB").arg(qRound(tib));
            return QString("%1 TiB").arg(QString::number(tib, 'f', 1));
        }
        const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
        if (gib >= 1.0)
            return QString("%1 GiB").arg(QString::number(gib, 'f', 1));
        const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
        if (mib >= 1.0)
            return QString("%1 MiB").arg(QString::number(mib, 'f', 1));
        return Misc::FormatBytes(bytes, 1);
    }
}

QString SmartReader::FormatBytes(quint64 bytes)
{
    return formatTiBBytes(bytes);
}

QString SmartReader::getDrivePathForDisk(const QString &diskName)
{
    QString name = diskName;
    if (name.startsWith("/dev/"))
        name = name.mid(5);

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected())
        return {};

    QDBusInterface blockIface("org.freedesktop.UDisks2",
                              "/org/freedesktop/UDisks2/block_devices/" + name,
                              "org.freedesktop.UDisks2.Block",
                              bus);
    if (!blockIface.isValid())
        return {};

    const QDBusObjectPath drivePath = blockIface.property("Drive").value<QDBusObjectPath>();
    const QString path = drivePath.path();
    if (path.isEmpty() || path == "/")
        return {};

    return path;
}

bool SmartReader::IsSupported(const QString &diskName)
{
    const QString drivePath = getDrivePathForDisk(diskName);
    if (drivePath.isEmpty())
        return false;

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected())
        return false;

    // Check NVMe Controller
    QDBusInterface nvmeIface("org.freedesktop.UDisks2", drivePath, "org.freedesktop.UDisks2.NVMe.Controller", bus);
    if (nvmeIface.isValid())
        return true;

    // Check ATA Drive
    QDBusInterface ataIface("org.freedesktop.UDisks2", drivePath, "org.freedesktop.UDisks2.Drive.Ata", bus);
    if (ataIface.isValid())
    {
        const QVariant supported = ataIface.property("SmartSupported");
        return supported.isValid() && supported.toBool();
    }

    return false;
}

bool SmartReader::TriggerUpdate(const QString &diskName, QString *errorMessage)
{
    const QString drivePath = getDrivePathForDisk(diskName);
    if (drivePath.isEmpty())
    {
        if (errorMessage)
            *errorMessage = "No UDisks2 drive object found for disk";
        return false;
    }

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected())
    {
        if (errorMessage)
            *errorMessage = "Could not connect to system D-Bus";
        return false;
    }

    // Try NVMe Controller
    QDBusInterface nvmeIface("org.freedesktop.UDisks2", drivePath, "org.freedesktop.UDisks2.NVMe.Controller", bus);
    if (nvmeIface.isValid())
    {
        QDBusMessage reply = nvmeIface.call("SmartUpdate", QVariantMap());
        if (reply.type() == QDBusMessage::ErrorMessage)
        {
            if (errorMessage)
                *errorMessage = reply.errorMessage();
            return false;
        }
        return true;
    }

    // Try ATA Drive
    QDBusInterface ataIface("org.freedesktop.UDisks2", drivePath, "org.freedesktop.UDisks2.Drive.Ata", bus);
    if (ataIface.isValid())
    {
        QDBusMessage reply = ataIface.call("SmartUpdate", QVariantMap());
        if (reply.type() == QDBusMessage::ErrorMessage)
        {
            if (errorMessage)
                *errorMessage = reply.errorMessage();
            return false;
        }
        return true;
    }

    if (errorMessage)
        *errorMessage = "Neither NVMe nor ATA SMART interface is available on this drive";
    return false;
}

QString SmartReader::FormatDuration(quint64 totalSeconds)
{
    if (totalSeconds == 0)
        return "0 seconds";

    // Standard year = 365.25 days, month = 30.4375 days
    constexpr quint64 SECS_PER_MINUTE = 60;
    constexpr quint64 SECS_PER_HOUR = 3600;
    constexpr quint64 SECS_PER_DAY = 86400;
    constexpr double SECS_PER_MONTH = 30.4375 * 86400.0;
    constexpr double SECS_PER_YEAR = 365.25 * 86400.0;

    if (totalSeconds >= static_cast<quint64>(SECS_PER_YEAR))
    {
        const int years = static_cast<int>(totalSeconds / SECS_PER_YEAR);
        const double remYears = totalSeconds - (years * SECS_PER_YEAR);
        const int months = static_cast<int>(remYears / SECS_PER_MONTH);
        const double remMonths = remYears - (months * SECS_PER_MONTH);
        const int days = static_cast<int>(remMonths / SECS_PER_DAY);

        QStringList parts;
        parts.append(QString("%1 year%2").arg(years).arg(years == 1 ? "" : "s"));
        if (months > 0)
            parts.append(QString("%1 month%2").arg(months).arg(months == 1 ? "" : "s"));
        if (days > 0)
            parts.append(QString("%1 day%2").arg(days).arg(days == 1 ? "" : "s"));
        return parts.join(", ");
    }

    if (totalSeconds >= static_cast<quint64>(SECS_PER_MONTH))
    {
        const int months = static_cast<int>(totalSeconds / SECS_PER_MONTH);
        const double remMonths = totalSeconds - (months * SECS_PER_MONTH);
        const int days = static_cast<int>(remMonths / SECS_PER_DAY);
        const int hours = static_cast<int>(fmod(remMonths, SECS_PER_DAY) / SECS_PER_HOUR);

        QStringList parts;
        parts.append(QString("%1 month%2").arg(months).arg(months == 1 ? "" : "s"));
        if (days > 0)
            parts.append(QString("%1 day%2").arg(days).arg(days == 1 ? "" : "s"));
        if (hours > 0 && parts.size() < 2)
            parts.append(QString("%1 hour%2").arg(hours).arg(hours == 1 ? "" : "s"));
        return parts.join(", ");
    }

    if (totalSeconds >= SECS_PER_DAY)
    {
        const quint64 days = totalSeconds / SECS_PER_DAY;
        const quint64 rem = totalSeconds % SECS_PER_DAY;
        const quint64 hours = rem / SECS_PER_HOUR;
        const quint64 mins = (rem % SECS_PER_HOUR) / SECS_PER_MINUTE;

        QStringList parts;
        parts.append(QString("%1 day%2").arg(days).arg(days == 1 ? "" : "s"));
        if (hours > 0)
            parts.append(QString("%1 hour%2").arg(hours).arg(hours == 1 ? "" : "s"));
        if (mins > 0 && days < 7)
            parts.append(QString("%1 minute%2").arg(mins).arg(mins == 1 ? "" : "s"));
        return parts.join(", ");
    }

    if (totalSeconds >= SECS_PER_HOUR)
    {
        const quint64 hours = totalSeconds / SECS_PER_HOUR;
        const quint64 mins = (totalSeconds % SECS_PER_HOUR) / SECS_PER_MINUTE;
        if (mins > 0)
            return QString("%1 hour%2, %3 minute%4")
                .arg(hours).arg(hours == 1 ? "" : "s")
                .arg(mins).arg(mins == 1 ? "" : "s");
        return QString("%1 hour%2").arg(hours).arg(hours == 1 ? "" : "s");
    }

    if (totalSeconds >= SECS_PER_MINUTE)
    {
        const quint64 mins = totalSeconds / SECS_PER_MINUTE;
        const quint64 secs = totalSeconds % SECS_PER_MINUTE;
        if (secs > 0)
            return QString("%1 minute%2, %3 second%4")
                .arg(mins).arg(mins == 1 ? "" : "s")
                .arg(secs).arg(secs == 1 ? "" : "s");
        return QString("%1 minute%2").arg(mins).arg(mins == 1 ? "" : "s");
    }

    return QString("%1 second%2").arg(totalSeconds).arg(totalSeconds == 1 ? "" : "s");
}

QString SmartReader::FormatRelativeTime(qint64 secsSinceEpoch)
{
    if (secsSinceEpoch <= 0)
        return "Never";

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 diff = now - secsSinceEpoch;

    if (diff < 5)
        return "Just now";
    if (diff < 60)
        return QString("%1 seconds ago").arg(diff);
    if (diff < 3600)
    {
        const qint64 mins = diff / 60;
        return QString("%1 minute%2 ago").arg(mins).arg(mins == 1 ? "" : "s");
    }
    if (diff < 86400)
    {
        const qint64 hours = diff / 3600;
        return QString("%1 hour%2 ago").arg(hours).arg(hours == 1 ? "" : "s");
    }

    const qint64 days = diff / 86400;
    return QString("%1 day%2 ago").arg(days).arg(days == 1 ? "" : "s");
}

SmartData SmartReader::ReadSmartData(const QString &diskName)
{
    const QString drivePath = getDrivePathForDisk(diskName);
    if (!drivePath.isEmpty())
    {
        SmartData data = readFromUDisks2(diskName, drivePath);
        if (data.isSupported)
            return data;
    }

    // Fallback to smartctl if UDisks2 did not yield SMART data
    return readFromSmartctl(diskName);
}

SmartData SmartReader::readFromUDisks2(const QString &diskName, const QString &drivePath)
{
    SmartData data;
    data.deviceName = diskName;
    if (data.deviceName.startsWith("/dev/"))
        data.deviceName = data.deviceName.mid(5);
    data.drivePath = drivePath;

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected())
    {
        data.errorMessage = "Could not connect to system D-Bus";
        return data;
    }

    // Drive generic properties
    QDBusInterface driveIface("org.freedesktop.UDisks2", drivePath, "org.freedesktop.UDisks2.Drive", bus);
    if (driveIface.isValid())
    {
        data.model = driveIface.property("Model").toString();
        data.serial = driveIface.property("Serial").toString();
    }

    // 1. Check NVMe Controller
    QDBusInterface nvmeIface("org.freedesktop.UDisks2", drivePath, "org.freedesktop.UDisks2.NVMe.Controller", bus);
    if (nvmeIface.isValid())
    {
        data.isNvme = true;
        data.isSupported = true;

        data.lastUpdatedSecs = nvmeIface.property("SmartUpdated").toLongLong();
        const quint64 powerHours = nvmeIface.property("SmartPowerOnHours").toULongLong();
        data.powerOnSeconds = powerHours * 3600ULL;

        const quint16 tempK = nvmeIface.property("SmartTemperature").toUInt();
        if (tempK > 0)
            data.temperatureC = static_cast<double>(tempK) - 273.15;

        const QStringList criticalWarnings = nvmeIface.property("SmartCriticalWarning").toStringList();
        if (!criticalWarnings.isEmpty())
        {
            data.isFailing = true;
            data.healthStatus = "Warning";
        }
        else
        {
            data.healthStatus = "Good";
        }

        QString selftest = nvmeIface.property("SmartSelftestStatus").toString();
        if (selftest.isEmpty())
            data.selfTestStatus = "Not available";
        else
        {
            selftest[0] = selftest[0].toUpper();
            data.selfTestStatus = selftest;
        }

        // Call SmartGetAttributes
        QDBusMessage reply = nvmeIface.call("SmartGetAttributes", QVariantMap());
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
        {
            QMap<QString, QVariant> nvmeMap;
            const QDBusArgument arg = reply.arguments().at(0).value<QDBusArgument>();
            arg.beginMap();
            while (!arg.atEnd())
            {
                QString key;
                QVariant val;
                arg.beginMapEntry();
                arg >> key >> val;
                arg.endMapEntry();
                nvmeMap.insert(key, val);
            }
            arg.endMap();

            // Order attributes logically matching Mission Center
            const struct NvmeField {
                const char *key;
                const char *name;
            } fields[] = {
                { "percent_used", "Percentage Used" },
                { "avail_spare", "Available Spare" },
                { "spare_thresh", "Spare Threshold" },
                { "total_data_read", "Total Data Read (TBR)" },
                { "total_data_written", "Total Data Written (TBW)" },
                { "warning_temp_time", "Warning Temp Time" },
                { "critical_temp_time", "Critical Temp Time" },
                { "ctrl_busy_time", "Controller Busy Time" },
                { "wctemp", "Warning Temperature" },
                { "cctemp", "Critical Temperature" },
                { "temp_sensors", "Temperature Sensors" },
                { "unsafe_shutdowns", "Unsafe Shutdowns" },
                { "media_errors", "Media Errors" },
                { "num_err_log_entries", "Number Error Log Entries" },
                { "power_cycles", "Power Cycles" }
            };

            for (const auto &field : fields)
            {
                if (!nvmeMap.contains(field.key))
                    continue;

                const QVariant raw = nvmeMap.value(field.key);
                SmartAttribute attr;
                attr.id = field.key;
                attr.name = field.name;
                attr.status = "OK";

                if (strcmp(field.key, "percent_used") == 0)
                {
                    const uchar uval = raw.value<uchar>();
                    attr.rawValue = QString::number(uval);
                    attr.decodedValue = QString("%1%").arg(uval);
                    if (uval >= 100)
                    {
                        attr.status = "Pre-fail";
                        attr.isFailing = true;
                    }
                }
                else if (strcmp(field.key, "avail_spare") == 0)
                {
                    const uchar uval = raw.value<uchar>();
                    attr.rawValue = QString::number(uval);
                    attr.decodedValue = QString::number(uval); // Mission Center displays number e.g. 100
                    const uchar thresh = nvmeMap.value("spare_thresh").value<uchar>();
                    if (thresh > 0 && uval <= thresh)
                    {
                        attr.status = "FAILING";
                        attr.isFailing = true;
                    }
                }
                else if (strcmp(field.key, "spare_thresh") == 0)
                {
                    const uchar uval = raw.value<uchar>();
                    attr.rawValue = QString::number(uval);
                    attr.decodedValue = QString::number(uval);
                }
                else if (strcmp(field.key, "total_data_read") == 0 || strcmp(field.key, "total_data_written") == 0)
                {
                    const quint64 bytes = raw.toULongLong();
                    attr.rawValue = QString::number(bytes);
                    attr.decodedValue = formatTiBBytes(bytes);
                    if (strcmp(field.key, "total_data_written") == 0)
                        data.totalBytesWritten = bytes;
                    else
                        data.totalBytesRead = bytes;
                }
                else if (strcmp(field.key, "ctrl_busy_time") == 0 ||
                         strcmp(field.key, "warning_temp_time") == 0 ||
                         strcmp(field.key, "critical_temp_time") == 0)
                {
                    const quint64 secs = raw.toULongLong();
                    attr.rawValue = QString::number(secs);
                    attr.decodedValue = FormatDuration(secs);
                }
                else if (strcmp(field.key, "wctemp") == 0 || strcmp(field.key, "cctemp") == 0)
                {
                    const quint16 kelvin = raw.value<quint16>();
                    attr.rawValue = QString::number(kelvin);
                    if (kelvin > 0)
                        attr.decodedValue = QString("%1 °C").arg(qRound(static_cast<double>(kelvin) - 273.15));
                    else
                        attr.decodedValue = "N/A";
                }
                else if (strcmp(field.key, "temp_sensors") == 0)
                {
                    QStringList temps;
                    if (raw.canConvert<QDBusArgument>())
                    {
                        const QDBusArgument sArg = raw.value<QDBusArgument>();
                        sArg.beginArray();
                        while (!sArg.atEnd())
                        {
                            quint16 sk = 0;
                            sArg >> sk;
                            if (sk > 0)
                                temps.append(QString("%1 °C").arg(qRound(static_cast<double>(sk) - 273.15)));
                        }
                        sArg.endArray();
                    }
                    attr.rawValue = raw.toString();
                    attr.decodedValue = temps.isEmpty() ? "[]" : QString("[%1]").arg(temps.join(", "));
                }
                else if (strcmp(field.key, "media_errors") == 0)
                {
                    const quint64 count = raw.toULongLong();
                    attr.rawValue = QString::number(count);
                    attr.decodedValue = QString::number(count);
                    if (count > 0)
                    {
                        attr.status = "Warning";
                        attr.isFailing = true;
                    }
                }
                else
                {
                    attr.rawValue = raw.toString();
                    attr.decodedValue = raw.toString();
                }

                data.attributes.append(attr);
            }

            return data;
        }
    }

    // 2. Check ATA Drive
    QDBusInterface ataIface("org.freedesktop.UDisks2", drivePath, "org.freedesktop.UDisks2.Drive.Ata", bus);
    if (ataIface.isValid())
    {
        const QVariant supported = ataIface.property("SmartSupported");
        if (supported.isValid() && supported.toBool())
        {
            data.isNvme = false;
            data.isSupported = true;

            data.lastUpdatedSecs = ataIface.property("SmartUpdated").toLongLong();
            data.powerOnSeconds = ataIface.property("SmartPowerOnSeconds").toULongLong();

            const double tempK = ataIface.property("SmartTemperature").toDouble();
            if (tempK > 0)
                data.temperatureC = tempK - 273.15;

            const bool failing = ataIface.property("SmartFailing").toBool();
            const int numFailing = ataIface.property("SmartNumAttributesFailing").toInt();
            const int numFailedPast = ataIface.property("SmartNumAttributesFailedInThePast").toInt();
            const int numBadSectors = ataIface.property("SmartNumBadSectors").toInt();

            if (failing || numFailing > 0)
            {
                data.isFailing = true;
                data.healthStatus = "Failing";
            }
            else if (numBadSectors > 0 || numFailedPast > 0)
            {
                data.healthStatus = "Warning";
            }
            else
            {
                data.healthStatus = "Good";
            }

            QString selftest = ataIface.property("SmartSelftestStatus").toString();
            if (selftest.isEmpty())
                data.selfTestStatus = "Not available";
            else
            {
                selftest[0] = selftest[0].toUpper();
                data.selfTestStatus = selftest;
            }

            // Call SmartGetAttributes
            QDBusMessage reply = ataIface.call("SmartGetAttributes", QVariantMap());
            if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
            {
                const QDBusArgument arg = reply.arguments().at(0).value<QDBusArgument>();
                arg.beginArray();
                while (!arg.atEnd())
                {
                    uchar id = 0;
                    QString name;
                    quint16 flags = 0;
                    qint32 val = 0;
                    qint32 worst = 0;
                    qint32 thresh = 0;
                    qint64 pretty = 0;
                    qint32 prettyUnit = 0;
                    QVariantMap expansion;

                    arg.beginStructure();
                    arg >> id >> name >> flags >> val >> worst >> thresh >> pretty >> prettyUnit;
                    if (!arg.atEnd())
                    {
                        arg >> expansion;
                    }
                    arg.endStructure();

                    SmartAttribute attr;
                    attr.id = QString::number(id);
                    attr.name = formatAtaAttributeName(name, id);
                    attr.normalizedValue = QString::number(val);
                    attr.worstValue = QString::number(worst);
                    attr.thresholdValue = QString::number(thresh);
                    attr.rawValue = QString::number(pretty);

                    // Decode pretty value according to prettyUnit
                    switch (prettyUnit)
                    {
                        case 2: // Milliseconds
                            if (pretty > 0 && pretty < 1000)
                                attr.decodedValue = QString("%1 ms").arg(pretty);
                            else
                                attr.decodedValue = FormatDuration(pretty / 1000ULL);
                            break;
                        case 3: // Sectors
                            attr.decodedValue = (pretty == 1) ? "1 sector" : QString("%1 sectors").arg(pretty);
                            break;
                        case 4: // Millikelvin
                            attr.decodedValue = QString("%1 °C").arg(qRound(static_cast<double>(pretty) / 1000.0 - 273.15));
                            break;
                        case 5: // Small percent
                        case 6: // Percent
                            attr.decodedValue = QString("%1%").arg(pretty);
                            break;
                        case 7: // SK_SMART_ATTRIBUTE_UNIT_MB (Megabytes)
                            attr.decodedValue = formatTiBBytes(static_cast<quint64>(pretty) * 1024ULL * 1024ULL);
                            break;
                        case 1: // Dimensionless
                            attr.decodedValue = QString::number(pretty);
                            break;
                        default:
                            if (id == 241 || id == 242)
                                attr.decodedValue = formatTiBBytes(static_cast<quint64>(pretty) * 1024ULL * 1024ULL);
                            else if (pretty != 0)
                                attr.decodedValue = QString::number(pretty);
                            else
                                attr.decodedValue = "0";
                            break;
                    }

                    if (id == 241 || name == "total-lbas-written" || name == "host-writes-gib" || name == "lifetime-writes-gib")
                    {
                        data.totalBytesWritten = static_cast<quint64>(pretty) * 1024ULL * 1024ULL;
                        attr.decodedValue = formatTiBBytes(data.totalBytesWritten);
                    }
                    else if (id == 242 || name == "total-lbas-read" || name == "host-reads-gib" || name == "lifetime-reads-gib")
                    {
                        data.totalBytesRead = static_cast<quint64>(pretty) * 1024ULL * 1024ULL;
                        attr.decodedValue = formatTiBBytes(data.totalBytesRead);
                    }

                    if (val > 0 && thresh > 0 && val <= thresh)
                    {
                        attr.status = "FAILING";
                        attr.isFailing = true;
                    }
                    else if (worst > 0 && thresh > 0 && worst <= thresh)
                    {
                        attr.status = "Pre-fail";
                    }
                    else
                    {
                        attr.status = "OK";
                    }

                    data.attributes.append(attr);
                }
                arg.endArray();
            }

            return data;
        }
    }

    return data;
}

SmartData SmartReader::readFromSmartctl(const QString &diskName)
{
    SmartData data;
    data.deviceName = diskName;
    if (data.deviceName.startsWith("/dev/"))
        data.deviceName = data.deviceName.mid(5);

    QProcess proc;
    proc.start("smartctl", { "-a", "-j", "/dev/" + data.deviceName });
    if (!proc.waitForFinished(3000))
    {
        data.errorMessage = "smartctl timed out";
        return data;
    }

    const QByteArray output = proc.readAllStandardOutput();
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(output, &parseErr);
    if (doc.isNull() || !doc.isObject())
    {
        data.errorMessage = "smartctl did not return valid JSON";
        return data;
    }

    const QJsonObject root = doc.object();
    if (root.contains("messages"))
    {
        const QJsonArray msgs = root.value("messages").toArray();
        for (const auto &m : msgs)
        {
            if (m.toObject().value("severity").toString() == "error")
                data.errorMessage = m.toObject().value("string").toString();
        }
    }

    const QJsonObject smartStatus = root.value("smart_status").toObject();
    if (smartStatus.contains("passed"))
    {
        data.isSupported = true;
        const bool passed = smartStatus.value("passed").toBool();
        data.healthStatus = passed ? "Good" : "Failing";
        data.isFailing = !passed;
        data.selfTestStatus = passed ? "Success" : "Failed";
    }

    if (root.contains("model_name"))
        data.model = root.value("model_name").toString();
    if (root.contains("serial_number"))
        data.serial = root.value("serial_number").toString();

    if (root.contains("power_on_time"))
    {
        const quint64 hours = root.value("power_on_time").toObject().value("hours").toInteger();
        data.powerOnSeconds = hours * 3600ULL;
    }

    if (root.contains("temperature"))
    {
        data.temperatureC = root.value("temperature").toObject().value("current").toDouble();
    }

    data.lastUpdatedSecs = QDateTime::currentSecsSinceEpoch();

    // NVMe attributes
    if (root.contains("nvme_smart_health_information_log"))
    {
        data.isNvme = true;
        const QJsonObject nvme = root.value("nvme_smart_health_information_log").toObject();
        for (auto it = nvme.begin(); it != nvme.end(); ++it)
        {
            SmartAttribute attr;
            attr.id = it.key();
            attr.rawValue = it.value().toVariant().toString();
            attr.status = "OK";

            if (it.key() == "data_units_written")
            {
                attr.name = "Total Data Written (TBW)";
                data.totalBytesWritten = it.value().toVariant().toULongLong() * 1000ULL * 512ULL;
                attr.decodedValue = formatTiBBytes(data.totalBytesWritten);
            }
            else if (it.key() == "data_units_read")
            {
                attr.name = "Total Data Read (TBR)";
                data.totalBytesRead = it.value().toVariant().toULongLong() * 1000ULL * 512ULL;
                attr.decodedValue = formatTiBBytes(data.totalBytesRead);
            }
            else
            {
                attr.name = it.key();
                attr.decodedValue = attr.rawValue;
            }

            data.attributes.append(attr);
        }
    }

    // ATA attributes
    if (root.contains("ata_smart_attributes"))
    {
        data.isNvme = false;
        const QJsonArray table = root.value("ata_smart_attributes").toObject().value("table").toArray();
        for (const auto &item : table)
        {
            const QJsonObject obj = item.toObject();
            SmartAttribute attr;
            const int id = obj.value("id").toInt();
            const QString rawName = obj.value("name").toString();
            attr.id = QString::number(id);
            attr.name = formatAtaAttributeName(rawName, id);
            attr.normalizedValue = QString::number(obj.value("value").toInt());
            attr.worstValue = QString::number(obj.value("worst").toInt());
            attr.thresholdValue = QString::number(obj.value("thresh").toInt());
            const quint64 rawVal = obj.value("raw").toObject().value("value").toInteger();
            attr.rawValue = QString::number(rawVal);
            attr.decodedValue = obj.value("raw").toObject().value("string").toString();

            if (id == 241 || rawName == "total-lbas-written" || rawName == "host-writes-gib" || rawName == "lifetime-writes-gib")
            {
                data.totalBytesWritten = rawVal * 1024ULL * 1024ULL;
                attr.decodedValue = formatTiBBytes(data.totalBytesWritten);
            }
            else if (id == 242 || rawName == "total-lbas-read" || rawName == "host-reads-gib" || rawName == "lifetime-reads-gib")
            {
                data.totalBytesRead = rawVal * 1024ULL * 1024ULL;
                attr.decodedValue = formatTiBBytes(data.totalBytesRead);
            }

            if (attr.decodedValue.isEmpty())
                attr.decodedValue = attr.rawValue;
            attr.status = obj.value("when_failed").toString().isEmpty() ? "OK" : obj.value("when_failed").toString();
            data.attributes.append(attr);
        }
    }

    return data;
}
