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

#include "batterydetailwidget.h"
#include "ui_batterydetailwidget.h"
#include "../colorscheme.h"
#include "../metrics.h"
#include "../misc.h"
#include "../ui/uihelper.h"
#include "../ui/widgetstyle.h"

#include <QGridLayout>
#include <QLabel>
#include <cmath>

using namespace Perf;

namespace
{
    QString FormatRemainingTime(qint64 totalSec)
    {
        if (totalSec <= 0)
            return QStringLiteral("—");
        qint64 hours = totalSec / 3600;
        qint64 minutes = (totalSec % 3600) / 60;
        if (hours > 0)
            return QObject::tr("%1 hr %2 min").arg(hours).arg(minutes);
        return QObject::tr("%1 min").arg(qMax<qint64>(1, minutes));
    }
}

BatteryDetailWidget::BatteryDetailWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::BatteryDetailWidget)
{
    this->ui->setupUi(this);

    const ColorScheme *scheme = ColorScheme::GetCurrent();

    // Capacity graph
    this->ui->capacityGraphWidget->SetColor(scheme->BatteryGraphLineColor, scheme->BatteryGraphFillColor, scheme->BatteryGraphSecondaryFillColor);
    this->ui->capacityGraphWidget->SetSampleCapacity(TUX_MANAGER_HISTORY_SIZE);
    this->ui->capacityGraphWidget->SetGridColumns(6);
    this->ui->capacityGraphWidget->SetGridRows(4);
    this->ui->capacityGraphWidget->SetValueFormat(GraphWidget::ValueFormat::Percent);
    this->ui->capacityGraphWidget->SetSeriesNames(tr("Charge level"));
    UIHelper::EnableGraphContextMenu(this->ui->capacityGraphWidget);

    // Power draw graph
    this->ui->powerGraphWidget->SetColor(scheme->BatteryGraphLineColor, scheme->BatteryGraphFillColor, scheme->BatteryGraphSecondaryFillColor);
    this->ui->powerGraphWidget->SetSampleCapacity(TUX_MANAGER_HISTORY_SIZE);
    this->ui->powerGraphWidget->SetGridColumns(6);
    this->ui->powerGraphWidget->SetGridRows(4);
    this->ui->powerGraphWidget->SetValueFormat(GraphWidget::ValueFormat::Raw);
    this->ui->powerGraphWidget->SetSeriesNames(tr("Power draw"));
    UIHelper::EnableGraphContextMenu(this->ui->powerGraphWidget);

    UIHelper::EnableCopyLabelContextMenu(this->ui->statStatusValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statPowerDrawValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statTotalPowerValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statVoltageValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statTimeRemainingValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statTechValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statEnergyFullValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statEnergyNowValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statHealthValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statCycleCountValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statDeviceValue);

    WidgetStyle::ApplyTextStyle(this->ui->titleLabel, scheme->BatteryTitleColor, 18, true);
    WidgetStyle::ApplyTextStyle(this->ui->modelNameLabel, scheme->MutedTextColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->statusHeaderLabel, scheme->BatteryTitleColor, 18);
    WidgetStyle::ApplyTextStyle(this->ui->capacityGraphLabel, scheme->StatLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->capacityGraphMaxLabel, scheme->StatLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->powerGraphLabel, scheme->StatLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->powerGraphMaxLabel, scheme->StatLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->capacityTimeLeftLabel, scheme->AxisLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->capacityTimeRightLabel, scheme->AxisLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->powerTimeLeftLabel, scheme->AxisLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->powerTimeRightLabel, scheme->AxisLabelColor, 8);

    if (QGridLayout *statsGrid = this->findChild<QGridLayout *>(QStringLiteral("statsGrid")))
    {
        for (int row = 0; row < statsGrid->rowCount(); ++row)
        {
            for (int column = 0; column < statsGrid->columnCount(); ++column)
            {
                if (QLayoutItem *item = statsGrid->itemAtPosition(row, column))
                {
                    if (QLabel *label = qobject_cast<QLabel *>(item->widget()))
                    {
                        if (column % 2 == 0)
                            WidgetStyle::ApplyTextStyle(label, scheme->StatLabelColor, 10);
                        else
                            WidgetStyle::ApplyTextStyle(label, scheme->DarkMode ? QColor(0xee, 0xee, 0xee) : QColor(0x22, 0x22, 0x22), 11);
                    }
                }
            }
        }
    }
}

BatteryDetailWidget::~BatteryDetailWidget()
{
    delete this->ui;
}

void BatteryDetailWidget::SetBattery(int index)
{
    this->m_batteryIndex = index;
    connect(Metrics::Get(), &Metrics::updated, this, &BatteryDetailWidget::onUpdated);

    if (this->m_batteryIndex >= 0 && this->m_batteryIndex < Metrics::GetBattery()->BatteryCount())
    {
        const Battery::BatteryInfo &bat = Metrics::GetBattery()->FromIndex(this->m_batteryIndex);
        this->bindBatteryIdentity();
        this->ui->capacityGraphWidget->SetDataSource(bat.CapacityHistory, 100.0);
        this->ui->powerGraphWidget->SetDataSource(bat.PowerDrawHistory, this->m_maxObservedPower);
    }
    this->onUpdated();
}

void BatteryDetailWidget::ApplyColorScheme()
{
    const ColorScheme *scheme = ColorScheme::GetCurrent();
    WidgetStyle::ApplyTextStyle(this->ui->titleLabel, scheme->BatteryTitleColor, 18, true);
    WidgetStyle::ApplyTextStyle(this->ui->modelNameLabel, scheme->MutedTextColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->statusHeaderLabel, scheme->BatteryTitleColor, 18);
    WidgetStyle::ApplyTextStyle(this->ui->capacityGraphLabel, scheme->StatLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->capacityGraphMaxLabel, scheme->StatLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->powerGraphLabel, scheme->StatLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->powerGraphMaxLabel, scheme->StatLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->capacityTimeLeftLabel, scheme->AxisLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->capacityTimeRightLabel, scheme->AxisLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->powerTimeLeftLabel, scheme->AxisLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->powerTimeRightLabel, scheme->AxisLabelColor, 8);

    this->ui->capacityGraphWidget->SetColor(scheme->BatteryGraphLineColor, scheme->BatteryGraphFillColor, scheme->BatteryGraphSecondaryFillColor);
    this->ui->powerGraphWidget->SetColor(scheme->BatteryGraphLineColor, scheme->BatteryGraphFillColor, scheme->BatteryGraphSecondaryFillColor);

    if (QGridLayout *statsGrid = this->findChild<QGridLayout *>(QStringLiteral("statsGrid")))
    {
        for (int row = 0; row < statsGrid->rowCount(); ++row)
        {
            for (int column = 0; column < statsGrid->columnCount(); ++column)
            {
                if (QLayoutItem *item = statsGrid->itemAtPosition(row, column))
                {
                    if (QLabel *label = qobject_cast<QLabel *>(item->widget()))
                    {
                        if (column % 2 == 0)
                            WidgetStyle::ApplyTextStyle(label, scheme->StatLabelColor, 10);
                        else
                            WidgetStyle::ApplyTextStyle(label, scheme->DarkMode ? QColor(0xee, 0xee, 0xee) : QColor(0x22, 0x22, 0x22), 11);
                    }
                }
            }
        }
    }

    this->update();
}

void BatteryDetailWidget::bindBatteryIdentity()
{
    if (this->m_batteryIndex < 0 || this->m_batteryIndex >= Metrics::GetBattery()->BatteryCount())
        return;

    const Battery::BatteryInfo &bat = Metrics::GetBattery()->FromIndex(this->m_batteryIndex);
    this->ui->titleLabel->setText(Metrics::GetBattery()->BatteryCount() > 1
                                  ? tr("Battery %1").arg(this->m_batteryIndex)
                                  : tr("Battery"));

    QString model = bat.Manufacturer;
    if (!bat.ModelName.isEmpty())
    {
        if (!model.isEmpty())
            model += QLatin1Char(' ');
        model += bat.ModelName;
    }
    if (!bat.Technology.isEmpty())
    {
        if (!model.isEmpty())
            model += QStringLiteral(" (%1)").arg(bat.Technology);
        else
            model = bat.Technology;
    }
    this->ui->modelNameLabel->setText(model);
}

void BatteryDetailWidget::onUpdated()
{
    if (this->m_batteryIndex < 0 || this->m_batteryIndex >= Metrics::GetBattery()->BatteryCount())
        return;

    const Battery::BatteryInfo &bat = Metrics::GetBattery()->FromIndex(this->m_batteryIndex);

    // Top Header: e.g. "98% (Charging)"
    QString statusHeader = QString::number(bat.CapacityPct, 'f', 0) + QLatin1String("%");
    if (!bat.StatusStr.isEmpty())
        statusHeader += QStringLiteral(" (%1)").arg(bat.StatusStr);
    this->ui->statusHeaderLabel->setText(statusHeader);

    // Stat Grid: Status
    this->ui->statStatusValue->setText(bat.StatusStr.isEmpty() ? tr("—") : bat.StatusStr);

    // Stat Grid: Power draw (+ / - W)
    QString powerStr;
    if (bat.PowerDrawW > 0.05)
        powerStr = QString::asprintf("+%.2f W", bat.PowerDrawW);
    else if (bat.PowerDrawW < -0.05)
        powerStr = QString::asprintf("%.2f W", bat.PowerDrawW);
    else
        powerStr = QStringLiteral("0.00 W");
    this->ui->statPowerDrawValue->setText(powerStr);

    // Stat Grid: Total power (all power absorbed by the PC)
    if (bat.TotalPowerW > 0.05)
    {
        this->ui->statTotalPowerValue->setText(QString::asprintf("%.2f W", bat.TotalPowerW));
        QString tooltip;
        if (bat.Status == Battery::State::Charging)
        {
            tooltip = tr("Total power absorbed from AC adapter:\n  • Battery charging: %1 W\n  • System components: %2 W")
                          .arg(QString::number(bat.PowerRateW, 'f', 2), QString::number(bat.PlatformPowerW, 'f', 2));
        }
        else if (bat.Status == Battery::State::Discharging)
        {
            tooltip = tr("Total power drawn from battery: %1 W\n(Supplies CPU, GPU, display, and all components)")
                          .arg(QString::number(bat.TotalPowerW, 'f', 2));
        }
        else
        {
            tooltip = tr("Total power absorbed from AC adapter: %1 W\n(Supplies CPU, GPU, display, and all components)")
                          .arg(QString::number(bat.TotalPowerW, 'f', 2));
        }
        this->ui->statTotalPowerValue->setToolTip(tooltip);
    }
    else
    {
        this->ui->statTotalPowerValue->setText(tr("—"));
        this->ui->statTotalPowerValue->setToolTip({});
    }

    // Stat Grid: Voltage
    if (bat.VoltageV > 0.0)
        this->ui->statVoltageValue->setText(QString::asprintf("%.2f V", bat.VoltageV));
    else
        this->ui->statVoltageValue->setText(tr("—"));

    // Stat Grid: Estimated time
    if (bat.Status == Battery::State::Charging && bat.TimeToFullSec > 0)
        this->ui->statTimeRemainingValue->setText(FormatRemainingTime(bat.TimeToFullSec) + tr(" until full"));
    else if (bat.Status == Battery::State::Discharging && bat.TimeToEmptySec > 0)
        this->ui->statTimeRemainingValue->setText(FormatRemainingTime(bat.TimeToEmptySec) + tr(" remaining"));
    else if (bat.Status == Battery::State::Full)
        this->ui->statTimeRemainingValue->setText(tr("Fully charged"));
    else
        this->ui->statTimeRemainingValue->setText(tr("—"));

    // Stat Grid: Technology
    this->ui->statTechValue->setText(bat.Technology.isEmpty() ? tr("—") : bat.Technology);

    // Stat Grid: Full capacity
    if (bat.EnergyFullWh > 0.0)
    {
        if (bat.EnergyFullDesignWh > 0.0)
            this->ui->statEnergyFullValue->setText(QString::asprintf("%.1f Wh (Design: %.1f Wh)", bat.EnergyFullWh, bat.EnergyFullDesignWh));
        else
            this->ui->statEnergyFullValue->setText(QString::asprintf("%.1f Wh", bat.EnergyFullWh));
    }
    else
    {
        this->ui->statEnergyFullValue->setText(tr("—"));
    }

    // Stat Grid: Remaining energy
    if (bat.EnergyNowWh > 0.0)
        this->ui->statEnergyNowValue->setText(QString::asprintf("%.2f Wh", bat.EnergyNowWh));
    else
        this->ui->statEnergyNowValue->setText(tr("—"));

    // Stat Grid: Health
    if (bat.HealthPct >= 0.0)
        this->ui->statHealthValue->setText(QString::asprintf("%.1f%%", bat.HealthPct));
    else
        this->ui->statHealthValue->setText(tr("—"));

    // Stat Grid: Cycle count
    if (bat.CycleCount >= 0)
        this->ui->statCycleCountValue->setText(QString::number(bat.CycleCount));
    else
        this->ui->statCycleCountValue->setText(tr("—"));

    // Stat Grid: Device / Serial
    if (!bat.SerialNumber.isEmpty())
        this->ui->statDeviceValue->setText(QStringLiteral("%1 (S/N: %2)").arg(bat.ID, bat.SerialNumber));
    else
        this->ui->statDeviceValue->setText(bat.ID.isEmpty() ? tr("—") : bat.ID);

    // Update power max scale if needed
    if (bat.PowerRateW > this->m_maxObservedPower)
    {
        this->m_maxObservedPower = std::ceil(bat.PowerRateW * 1.25);
        this->ui->powerGraphWidget->SetMax(this->m_maxObservedPower);
    }

    this->ui->capacityGraphMaxLabel->setText(QString::number(qRound(bat.CapacityPct)) + QLatin1String("%"));
    this->ui->powerGraphMaxLabel->setText(QString::asprintf("%.1f W", qMax(this->m_maxObservedPower, bat.PowerRateW)));

    this->ui->capacityGraphWidget->Tick();
    this->ui->powerGraphWidget->Tick();
}

void BatteryDetailWidget::SetCompactMode(bool compact)
{
    if (QGridLayout *statsGrid = this->findChild<QGridLayout *>(QStringLiteral("statsGrid")))
    {
        for (int i = 0; i < statsGrid->count(); ++i)
        {
            if (QWidget *w = statsGrid->itemAt(i)->widget())
                w->setVisible(!compact);
        }
    }
}
