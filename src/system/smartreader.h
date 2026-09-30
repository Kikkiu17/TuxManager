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

#ifndef SYSTEM_SMARTREADER_H
#define SYSTEM_SMARTREADER_H

#include <QString>
#include <QList>
#include <QDateTime>
#include <QVariantMap>

namespace System
{
    struct SmartAttribute
    {
        QString id;              ///< Numeric ID or key name
        QString name;            ///< Human-readable attribute name
        QString decodedValue;    ///< Human-readable/decoded value (e.g. "7%", "120 TiB", "81 °C")
        QString rawValue;        ///< Raw numeric or hex representation
        QString normalizedValue; ///< Normalized value (0-100 or 0-255) for ATA
        QString worstValue;      ///< Worst recorded value for ATA
        QString thresholdValue;  ///< Failure threshold for ATA
        QString status;          ///< "OK", "Failing", etc.
        bool isFailing { false };
    };

    struct SmartData
    {
        bool isSupported { false };
        bool isNvme { false };
        QString deviceName;       ///< e.g. "nvme0n1" or "sda"
        QString model;            ///< e.g. "Lexar SSD NM620 1TB"
        QString serial;           ///< Serial number
        QString drivePath;        ///< UDisks2 object path if available
        qint64 lastUpdatedSecs { 0 };
        quint64 powerOnSeconds { 0 };
        quint64 totalBytesWritten { 0 };
        quint64 totalBytesRead { 0 };
        double temperatureC { 0.0 };
        QString healthStatus;     ///< "Good", "Normal", "Warning", "Failing"
        bool isFailing { false };
        QString selfTestStatus;   ///< "Success", "Passed", etc.
        QString errorMessage;

        QList<SmartAttribute> attributes;
    };

    class SmartReader
    {
        public:
            static bool IsSupported(const QString &diskName);
            static SmartData ReadSmartData(const QString &diskName);
            static bool TriggerUpdate(const QString &diskName, QString *errorMessage = nullptr);

            static QString FormatDuration(quint64 totalSeconds);
            static QString FormatRelativeTime(qint64 secsSinceEpoch);
            static QString FormatBytes(quint64 bytes);

        private:
            static QString getDrivePathForDisk(const QString &diskName);
            static SmartData readFromUDisks2(const QString &diskName, const QString &drivePath);
            static SmartData readFromSmartctl(const QString &diskName);
    };
} // namespace System

#endif // SYSTEM_SMARTREADER_H
