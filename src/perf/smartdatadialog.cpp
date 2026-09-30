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

#include "smartdatadialog.h"
#include "ui_smartdatadialog.h"
#include "../colorscheme.h"
#include "../ui/uihelper.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QTableWidgetItem>

using namespace Perf;

SmartDataDialog::SmartDataDialog(const QString &diskName, const QString &diskModel, QWidget *parent)
    : QDialog(parent),
      ui(new Ui::SmartDataDialog),
      m_diskName(diskName),
      m_diskModel(diskModel)
{
    this->ui->setupUi(this);

    if (this->m_diskName.startsWith("/dev/"))
        this->m_diskName = this->m_diskName.mid(5);

    const QString titleName = this->m_diskModel.isEmpty() ? this->m_diskName : this->m_diskModel;
    this->setWindowTitle(tr("SMART Data — %1 (/dev/%2)").arg(titleName, this->m_diskName));

    // Connect UI actions
    connect(this->ui->refreshButton, &QPushButton::clicked, this, [this]() {
        this->refreshData(true);
    });
    connect(this->ui->searchEdit, &QLineEdit::textChanged, this, &SmartDataDialog::onFilterChanged);
    connect(this->ui->modeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SmartDataDialog::onModeChanged);
    connect(this->ui->copyButton, &QPushButton::clicked, this, &SmartDataDialog::onCopyClicked);
    connect(this->ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Setup attributes table
    this->ui->attributesTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this->ui->attributesTable, &QTableWidget::customContextMenuRequested, this, &SmartDataDialog::showContextMenu);
    connect(this->ui->attributesTable, &QTableWidget::itemDoubleClicked, this, [](QTableWidgetItem *item) {
        if (item)
            QGuiApplication::clipboard()->setText(item->text());
    });

    UIHelper::EnableCopyLabelContextMenu(this->ui->poweredOnLabel);
    UIHelper::EnableCopyLabelContextMenu(this->ui->healthLabel);
    UIHelper::EnableCopyLabelContextMenu(this->ui->testStatusLabel);
    UIHelper::EnableCopyLabelContextMenu(this->ui->tbwLabel);
    UIHelper::EnableCopyLabelContextMenu(this->ui->tbrLabel);

    // Initial load
    this->refreshData(false);

    // Timer to update relative time ("1 minute ago")
    this->m_timer = new QTimer(this);
    connect(this->m_timer, &QTimer::timeout, this, &SmartDataDialog::updateRelativeTime);
    this->m_timer->start(10000);
}

SmartDataDialog::~SmartDataDialog()
{
    if (this->m_timer)
    {
        this->m_timer->stop();
        delete this->m_timer;
        this->m_timer = nullptr;
    }
    delete this->ui;
}

void SmartDataDialog::refreshData(bool triggerDeviceUpdate)
{
    if (triggerDeviceUpdate)
    {
        QString errMsg;
        System::SmartReader::TriggerUpdate(this->m_diskName, &errMsg);
    }

    this->m_smartData = System::SmartReader::ReadSmartData(this->m_diskName);

    // Update Header labels
    this->updateRelativeTime();

    if (this->m_smartData.powerOnSeconds > 0)
    {
        this->ui->poweredOnLabel->setText(tr("<b>Powered On:</b> %1")
            .arg(System::SmartReader::FormatDuration(this->m_smartData.powerOnSeconds)));
    }
    else
    {
        this->ui->poweredOnLabel->setText(tr("<b>Powered On:</b> —"));
    }

    const QString healthStatus = this->m_smartData.healthStatus.isEmpty() ? tr("Unknown") : this->m_smartData.healthStatus;
    QString healthColor = "#2ecc71"; // green
    if (this->m_smartData.isFailing || healthStatus.compare("Failing", Qt::CaseInsensitive) == 0)
        healthColor = "#e74c3c"; // red
    else if (healthStatus.compare("Warning", Qt::CaseInsensitive) == 0)
        healthColor = "#f39c12"; // yellow/orange

    this->ui->healthLabel->setText(tr("<b>Drive Health:</b> <span style='color: %1; font-weight: bold;'>%2</span>")
        .arg(healthColor, healthStatus));

    const QString testStatus = this->m_smartData.selfTestStatus.isEmpty() ? tr("Not available") : this->m_smartData.selfTestStatus;
    this->ui->testStatusLabel->setText(tr("<b>SMART Test Status:</b> %1").arg(testStatus));

    const bool hasTbwOrTbr = (this->m_smartData.totalBytesWritten > 0 || this->m_smartData.totalBytesRead > 0);
    this->ui->tbwRowWidget->setVisible(hasTbwOrTbr);
    if (hasTbwOrTbr)
    {
        if (this->m_smartData.totalBytesWritten > 0)
        {
            this->ui->tbwLabel->setText(tr("<b>Total Written (TBW):</b> %1")
                .arg(System::SmartReader::FormatBytes(this->m_smartData.totalBytesWritten)));
        }
        else
        {
            this->ui->tbwLabel->setText(tr("<b>Total Written (TBW):</b> —"));
        }

        if (this->m_smartData.totalBytesRead > 0)
        {
            this->ui->tbrLabel->setText(tr("<b>Total Read (TBR):</b> %1")
                .arg(System::SmartReader::FormatBytes(this->m_smartData.totalBytesRead)));
        }
        else
        {
            this->ui->tbrLabel->setText(tr("<b>Total Read (TBR):</b> —"));
        }
    }

    this->populateTable();
}

void SmartDataDialog::updateRelativeTime()
{
    if (this->m_smartData.lastUpdatedSecs > 0)
    {
        this->ui->refreshButton->setText(tr("Last Updated: %1 ⟳")
            .arg(System::SmartReader::FormatRelativeTime(this->m_smartData.lastUpdatedSecs)));
    }
    else
    {
        this->ui->refreshButton->setText(tr("Refresh SMART Data ⟳"));
    }
}

void SmartDataDialog::populateTable()
{
    this->ui->attributesTable->setSortingEnabled(false);
    this->ui->attributesTable->clear();

    const bool isDecodedMode = (this->ui->modeComboBox->currentIndex() == 0);

    if (isDecodedMode)
    {
        // 2 columns: Attribute | Value (matching Mission Center exactly)
        this->ui->attributesTable->setColumnCount(2);
        this->ui->attributesTable->setHorizontalHeaderLabels({ tr("Attribute"), tr("Value") });
        this->ui->attributesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        this->ui->attributesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    }
    else
    {
        // All details: ID | Attribute | Value | Raw | Normalized | Worst | Threshold | Status
        this->ui->attributesTable->setColumnCount(8);
        this->ui->attributesTable->setHorizontalHeaderLabels({
            tr("ID"), tr("Attribute"), tr("Value"), tr("Raw"), tr("Normalized"), tr("Worst"), tr("Threshold"), tr("Status")
        });
        for (int c = 0; c < 8; ++c)
        {
            if (c == 1)
                this->ui->attributesTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Stretch);
            else
                this->ui->attributesTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
        }
    }

    this->ui->attributesTable->setRowCount(this->m_smartData.attributes.size());

    for (int row = 0; row < this->m_smartData.attributes.size(); ++row)
    {
        const auto &attr = this->m_smartData.attributes.at(row);

        if (isDecodedMode)
        {
            auto *nameItem = new QTableWidgetItem(attr.name);
            auto *valItem = new QTableWidgetItem(attr.decodedValue);

            if (attr.isFailing)
            {
                nameItem->setForeground(QBrush(QColor("#e74c3c")));
                valItem->setForeground(QBrush(QColor("#e74c3c")));
            }

            this->ui->attributesTable->setItem(row, 0, nameItem);
            this->ui->attributesTable->setItem(row, 1, valItem);
        }
        else
        {
            auto *idItem = new QTableWidgetItem(attr.id);
            auto *nameItem = new QTableWidgetItem(attr.name);
            auto *valItem = new QTableWidgetItem(attr.decodedValue);
            auto *rawItem = new QTableWidgetItem(attr.rawValue);
            auto *normItem = new QTableWidgetItem(attr.normalizedValue.isEmpty() ? "—" : attr.normalizedValue);
            auto *worstItem = new QTableWidgetItem(attr.worstValue.isEmpty() ? "—" : attr.worstValue);
            auto *threshItem = new QTableWidgetItem(attr.thresholdValue.isEmpty() ? "—" : attr.thresholdValue);
            auto *statusItem = new QTableWidgetItem(attr.status.isEmpty() ? "OK" : attr.status);

            if (attr.isFailing)
            {
                statusItem->setForeground(QBrush(QColor("#e74c3c")));
                nameItem->setForeground(QBrush(QColor("#e74c3c")));
            }

            this->ui->attributesTable->setItem(row, 0, idItem);
            this->ui->attributesTable->setItem(row, 1, nameItem);
            this->ui->attributesTable->setItem(row, 2, valItem);
            this->ui->attributesTable->setItem(row, 3, rawItem);
            this->ui->attributesTable->setItem(row, 4, normItem);
            this->ui->attributesTable->setItem(row, 5, worstItem);
            this->ui->attributesTable->setItem(row, 6, threshItem);
            this->ui->attributesTable->setItem(row, 7, statusItem);
        }
    }

    this->applyFilter();
}

void SmartDataDialog::onModeChanged(int)
{
    this->populateTable();
}

void SmartDataDialog::onFilterChanged(const QString &)
{
    this->applyFilter();
}

void SmartDataDialog::applyFilter()
{
    const QString filter = this->ui->searchEdit->text().trimmed();
    const int rowCount = this->ui->attributesTable->rowCount();
    const int colCount = this->ui->attributesTable->columnCount();

    for (int row = 0; row < rowCount; ++row)
    {
        if (filter.isEmpty())
        {
            this->ui->attributesTable->setRowHidden(row, false);
            continue;
        }

        bool match = false;
        for (int col = 0; col < colCount; ++col)
        {
            if (QTableWidgetItem *item = this->ui->attributesTable->item(row, col))
            {
                if (item->text().contains(filter, Qt::CaseInsensitive))
                {
                    match = true;
                    break;
                }
            }
        }
        this->ui->attributesTable->setRowHidden(row, !match);
    }
}

void SmartDataDialog::onCopyClicked()
{
    QStringList lines;

    // Header line
    QStringList headerCells;
    for (int col = 0; col < this->ui->attributesTable->columnCount(); ++col)
    {
        headerCells.append(this->ui->attributesTable->horizontalHeaderItem(col)->text());
    }
    lines.append(headerCells.join("\t"));

    // Visible rows
    for (int row = 0; row < this->ui->attributesTable->rowCount(); ++row)
    {
        if (this->ui->attributesTable->isRowHidden(row))
            continue;

        QStringList rowCells;
        for (int col = 0; col < this->ui->attributesTable->columnCount(); ++col)
        {
            QTableWidgetItem *item = this->ui->attributesTable->item(row, col);
            rowCells.append(item ? item->text() : "");
        }
        lines.append(rowCells.join("\t"));
    }

    QGuiApplication::clipboard()->setText(lines.join("\n"));
}

void SmartDataDialog::showContextMenu(const QPoint &pos)
{
    QTableWidgetItem *clickedItem = this->ui->attributesTable->itemAt(pos);
    QMenu menu(this);

    if (clickedItem)
    {
        menu.addAction(tr("Copy Value"), [clickedItem]() {
            QGuiApplication::clipboard()->setText(clickedItem->text());
        });

        const int row = clickedItem->row();
        menu.addAction(tr("Copy Row"), [this, row]() {
            QStringList cells;
            for (int col = 0; col < this->ui->attributesTable->columnCount(); ++col)
            {
                QTableWidgetItem *item = this->ui->attributesTable->item(row, col);
                cells.append(item ? item->text() : "");
            }
            QGuiApplication::clipboard()->setText(cells.join("\t"));
        });
    }

    menu.addSeparator();
    menu.addAction(tr("Copy Entire Table"), this, &SmartDataDialog::onCopyClicked);

    menu.exec(this->ui->attributesTable->viewport()->mapToGlobal(pos));
}
