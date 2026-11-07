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

#include "widgetstyle.h"
#include "../colorscheme.h"

#include <QBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLayout>
#include <QWidget>

namespace WidgetStyle
{
    namespace
    {
        void styleMetricBox(QVBoxLayout *vbox, const ColorScheme *scheme, const QColor &valueColor)
        {
            if (!vbox)
                return;
            if (vbox->count() >= 1)
            {
                if (QLabel *label = qobject_cast<QLabel *>(vbox->itemAt(0)->widget()))
                    ApplyTextStyle(label, scheme->StatLabelColor, 9);
            }
            if (vbox->count() >= 2)
            {
                if (QLabel *valLabel = qobject_cast<QLabel *>(vbox->itemAt(1)->widget()))
                    ApplyTextStyle(valLabel, valueColor, 15);
            }
        }

        void styleSecondaryGrid(QGridLayout *grid, const ColorScheme *scheme, const QColor &valueColor)
        {
            if (!grid)
                return;
            for (int r = 0; r < grid->rowCount(); ++r)
            {
                if (QLayoutItem *labelItem = grid->itemAtPosition(r, 0))
                {
                    if (QLabel *label = qobject_cast<QLabel *>(labelItem->widget()))
                        ApplyTextStyle(label, scheme->StatLabelColor, 9);
                }
                if (QLayoutItem *valItem = grid->itemAtPosition(r, 1))
                {
                    if (QLabel *valLabel = qobject_cast<QLabel *>(valItem->widget()))
                        ApplyTextStyle(valLabel, valueColor, 9);
                }
            }
        }
    }

    QString TextStyle(const QColor &color, int fontSizePt, bool bold)
    {
        QString style;
        if (color.isValid())
            style += QString("color: %1;").arg(color.name(QColor::HexArgb));
        if (fontSizePt > 0)
            style += QString(" font-size: %1pt;").arg(fontSizePt);
        if (bold)
            style += " font-weight: bold;";
        return style.trimmed();
    }

    void ApplyTextStyle(QWidget *widget, const QColor &color, int fontSizePt, bool bold)
    {
        if (!widget)
            return;
        widget->setStyleSheet(TextStyle(color, fontSizePt, bold));
    }

    void ApplyStatsGridStyle(QWidget *root, const ColorScheme *scheme)
    {
        if (!root || !scheme)
            return;

        const QColor valueColor = scheme->DarkMode ? QColor(0xf2, 0xf2, 0xf2) : QColor(0x1a, 0x1a, 0x1a);

        if (QGridLayout *statsGrid = root->findChild<QGridLayout *>("statsGrid"))
        {
            for (int i = 0; i < statsGrid->count(); ++i)
            {
                QLayoutItem *item = statsGrid->itemAt(i);
                if (!item)
                    continue;

                if (QVBoxLayout *vbox = qobject_cast<QVBoxLayout *>(item->layout()))
                {
                    styleMetricBox(vbox, scheme, valueColor);
                } else if (QHBoxLayout *hbox = qobject_cast<QHBoxLayout *>(item->layout()))
                {
                    for (int j = 0; j < hbox->count(); ++j)
                    {
                        if (QVBoxLayout *innerVBox = qobject_cast<QVBoxLayout *>(hbox->itemAt(j)->layout()))
                            styleMetricBox(innerVBox, scheme, valueColor);
                    }
                } else if (QGridLayout *subGrid = qobject_cast<QGridLayout *>(item->layout()))
                {
                    styleSecondaryGrid(subGrid, scheme, valueColor);
                } else if (QLabel *label = qobject_cast<QLabel *>(item->widget()))
                {
                    ApplyTextStyle(label, scheme->StatLabelColor, 9);
                }
            }
        }

        if (QGridLayout *rightGrid = root->findChild<QGridLayout *>("rightStatsGrid"))
        {
            styleSecondaryGrid(rightGrid, scheme, valueColor);
        }
    }

    void ApplyAccentBarStyle(QWidget *bar, const QColor &color, bool dashed)
    {
        if (!bar)
            return;
        const QString borderStyle = dashed ? QStringLiteral("dashed") : QStringLiteral("solid");
        bar->setStyleSheet(QString("background: transparent; border: none; border-left: 3px %1 %2;")
                               .arg(borderStyle, color.name(QColor::HexArgb)));
    }

    void SetLayoutWidgetsVisible(QLayout *layout, bool visible)
    {
        if (!layout)
            return;
        for (int i = 0; i < layout->count(); ++i)
        {
            QLayoutItem *item = layout->itemAt(i);
            if (!item)
                continue;
            if (QWidget *w = item->widget())
                w->setVisible(visible);
            if (QLayout *childLayout = item->layout())
                SetLayoutWidgetsVisible(childLayout, visible);
        }
    }
}
