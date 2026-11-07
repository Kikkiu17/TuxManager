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

#ifndef WIDGETSTYLE_H
#define WIDGETSTYLE_H

#include <QColor>

class QWidget;
class QLayout;
class ColorScheme;

// Helper that creates a style string from input values, so that we don't need to use regex hacks or string manipulation
namespace WidgetStyle
{
    QString TextStyle(const QColor &color, int fontSizePt = -1, bool bold = false);
    void ApplyTextStyle(QWidget *widget, const QColor &color, int fontSizePt = -1, bool bold = false);
    void ApplyStatsGridStyle(QWidget *root, const ColorScheme *scheme);
    void ApplyAccentBarStyle(QWidget *bar, const QColor &color, bool dashed = false);
    void SetLayoutWidgetsVisible(QLayout *layout, bool visible);
}

#endif // WIDGETSTYLE_H
