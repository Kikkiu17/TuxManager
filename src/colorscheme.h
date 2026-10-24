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

#ifndef COLORSCHEME_H
#define COLORSCHEME_H

#include <QColor>
#include <QVector>
#include <QVariantMap>

class ColorScheme
{
    public:
        struct ColorField
        {
            const char *Name;
            QColor ColorScheme::*Member;
        };

        static ColorScheme *GetCurrent();
        static ColorScheme DefaultLight();
        static ColorScheme DefaultDark();
        static bool DetectDarkMode();
        static void Install(ColorScheme *scheme);
        static const QVector<ColorField> &Fields();

        ColorScheme();
        QVariantMap ToVariantMap() const;
        void ApplyVariantMap(const QVariantMap &map);

        bool DarkMode { false };

        QColor CpuGraphLineColor;
        QColor CpuGraphFillColor;
        QColor CpuGraphSecondaryFillColor;
        QColor MemoryGraphLineColor;
        QColor MemoryGraphFillColor;
        QColor DiskGraphLineColor;
        QColor DiskGraphFillColor;
        QColor DiskTransferGraphLineColor;
        QColor DiskTransferGraphFillColor;
        QColor DiskTransferGraphSecondaryFillColor;
        QColor NetworkGraphLineColor;
        QColor NetworkGraphFillColor;
        QColor NetworkGraphSecondaryFillColor;
        QColor GpuGraphLineColor;
        QColor GpuGraphFillColor;
        QColor GpuGraphSecondaryFillColor;
        QColor NpuGraphLineColor;
        QColor NpuGraphFillColor;
        QColor NpuGraphSecondaryFillColor;
        QColor BatteryGraphLineColor;
        QColor BatteryGraphFillColor;
        QColor BatteryGraphSecondaryFillColor;
        QColor SwapUsageGraphLineColor;
        QColor SwapUsageGraphFillColor;
        QColor SwapActivityGraphLineColor;
        QColor SwapActivityGraphFillColor;
        QColor SwapActivityGraphSecondaryFillColor;
        QColor GraphGridColor;
        QColor GraphOverlayTextColor;
        QColor SidePanelBackgroundColor;
        QColor SidePanelItemSelectedBackgroundColor;
        QColor SidePanelItemHoverBackgroundColor;
        QColor SidePanelItemBackgroundColor;
        QColor SidePanelItemSelectedTextColor;
        QColor SidePanelItemTextColor;
        QColor SidePanelItemSubtitleColor;
        QColor SidePanelItemSelectedBorderColor;
        QColor CpuTitleColor;
        QColor CpuHeaderValueColor;
        QColor MemoryTitleColor;
        QColor MemoryHeaderValueColor;
        QColor DiskTitleColor;
        QColor DiskHeaderValueColor;
        QColor NetworkTitleColor;
        QColor GpuTitleColor;
        QColor NpuTitleColor;
        QColor BatteryTitleColor;
        QColor MutedTextColor;
        QColor StatLabelColor;
        QColor AxisLabelColor;
        QColor MemoryLegendTextColor;
        QColor MemoryLegendUsedColor;
        QColor MemoryLegendCompressedColor;
        QColor MemoryLegendDirtyColor;
        QColor MemoryLegendCachedColor;
        QColor MemoryLegendFreeColor;
        QColor MemoryBarUsedColor;
        QColor MemoryBarCompressedColor;
        QColor MemoryBarDirtyColor;
        QColor MemoryBarCachedColor;
        QColor MemoryBarFreeColor;
        QColor MemoryBarBorderColor;

    private:
        static ColorScheme *current;
};

#endif // COLORSCHEME_H
