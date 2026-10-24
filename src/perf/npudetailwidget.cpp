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

#include "npudetailwidget.h"
#include "ui_npudetailwidget.h"
#include "../colorscheme.h"
#include "../metrics.h"
#include "../misc.h"
#include "../ui/uihelper.h"
#include "../ui/widgetstyle.h"

using namespace Perf;

NpuDetailWidget::NpuDetailWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::NpuDetailWidget)
{
    this->ui->setupUi(this);

    const ColorScheme *scheme = ColorScheme::GetCurrent();

    this->ui->graphWidget->SetColor(scheme->NpuGraphLineColor, scheme->NpuGraphFillColor, scheme->NpuGraphSecondaryFillColor);
    this->ui->graphWidget->SetSampleCapacity(TUX_MANAGER_HISTORY_SIZE);
    this->ui->graphWidget->SetGridColumns(6);
    this->ui->graphWidget->SetGridRows(4);
    this->ui->graphWidget->SetValueFormat(GraphWidget::ValueFormat::Percent);
    this->ui->graphWidget->SetSeriesNames(tr("NPU Utilization"));
    UIHelper::EnableGraphContextMenu(this->ui->graphWidget);

    UIHelper::EnableCopyLabelContextMenu(this->ui->statUtilValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statFreqValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statMaxFreqValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statPowerStateValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statMemValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statDriverValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statDriverVerValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statDeviceValue);

    WidgetStyle::ApplyTextStyle(this->ui->titleLabel, scheme->NpuTitleColor, 18, true);
    WidgetStyle::ApplyTextStyle(this->ui->modelNameLabel, scheme->MutedTextColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->utilizationLabel, scheme->NpuTitleColor, 18);
    WidgetStyle::ApplyTextStyle(this->ui->timeLeftLabel, scheme->AxisLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->timeRightLabel, scheme->AxisLabelColor, 8);

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

NpuDetailWidget::~NpuDetailWidget()
{
    delete this->ui;
}

void NpuDetailWidget::SetNpu(int index)
{
    this->m_npuIndex = index;
    connect(Metrics::Get(), &Metrics::updated, this, &NpuDetailWidget::onUpdated);

    if (this->m_npuIndex >= 0 && this->m_npuIndex < Metrics::GetNPU()->NpuCount())
    {
        const NPU::NPUInfo &npu = Metrics::GetNPU()->FromIndex(this->m_npuIndex);
        this->bindNpuIdentity();
        this->ui->graphWidget->SetDataSource(npu.UtilHistory);
    }
    this->onUpdated();
}

void NpuDetailWidget::ApplyColorScheme()
{
    const ColorScheme *scheme = ColorScheme::GetCurrent();
    WidgetStyle::ApplyTextStyle(this->ui->titleLabel, scheme->NpuTitleColor, 18, true);
    WidgetStyle::ApplyTextStyle(this->ui->modelNameLabel, scheme->MutedTextColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->utilizationLabel, scheme->NpuTitleColor, 18);
    WidgetStyle::ApplyTextStyle(this->ui->timeLeftLabel, scheme->AxisLabelColor, 8);
    WidgetStyle::ApplyTextStyle(this->ui->timeRightLabel, scheme->AxisLabelColor, 8);

    this->ui->graphWidget->SetColor(scheme->NpuGraphLineColor, scheme->NpuGraphFillColor, scheme->NpuGraphSecondaryFillColor);

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

void NpuDetailWidget::bindNpuIdentity()
{
    if (this->m_npuIndex < 0 || this->m_npuIndex >= Metrics::GetNPU()->NpuCount())
        return;

    const NPU::NPUInfo &npu = Metrics::GetNPU()->FromIndex(this->m_npuIndex);
    this->ui->titleLabel->setText(Metrics::GetNPU()->NpuCount() > 1
                                  ? tr("NPU %1").arg(this->m_npuIndex)
                                  : tr("NPU"));
    this->ui->modelNameLabel->setText(npu.Name.isEmpty() ? npu.ID : npu.Name);
    this->ui->statDriverValue->setText(npu.Driver.isEmpty() ? tr("—") : npu.Driver);
    this->ui->statDriverVerValue->setText(npu.DriverVersion.isEmpty() ? tr("—") : npu.DriverVersion);
    this->ui->statDeviceValue->setText(npu.DeviceNode.isEmpty() ? (!npu.PCIAddress.isEmpty() ? npu.PCIAddress : tr("—")) : npu.DeviceNode);
}

void NpuDetailWidget::onUpdated()
{
    if (this->m_npuIndex < 0 || this->m_npuIndex >= Metrics::GetNPU()->NpuCount())
        return;

    const NPU::NPUInfo &npu = Metrics::GetNPU()->FromIndex(this->m_npuIndex);

    const QString utilText = QString::number(npu.UtilPct, 'f', 0) + QLatin1String("%");
    this->ui->utilizationLabel->setText(utilText);
    this->ui->statUtilValue->setText(utilText);

    if (npu.CurrentFreqMHz >= 0)
        this->ui->statFreqValue->setText(tr("%1 MHz").arg(npu.CurrentFreqMHz));
    else
        this->ui->statFreqValue->setText(tr("—"));

    if (npu.MaxFreqMHz >= 0)
        this->ui->statMaxFreqValue->setText(tr("%1 MHz").arg(npu.MaxFreqMHz));
    else
        this->ui->statMaxFreqValue->setText(tr("—"));

    if (!npu.PowerState.isEmpty())
        this->ui->statPowerStateValue->setText(npu.PowerState);
    else
        this->ui->statPowerStateValue->setText(tr("—"));

    if (npu.MemUsedBytes >= 0)
        this->ui->statMemValue->setText(Misc::FormatBytes(static_cast<quint64>(npu.MemUsedBytes)));
    else
        this->ui->statMemValue->setText(tr("—"));
}

void NpuDetailWidget::SetCompactMode(bool compact)
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
