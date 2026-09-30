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

#include "performancewidget.h"
#include "metrics.h"
#include "ui_performancewidget.h"

#include "colorschemedialog.h"
#include "configuration.h"
#include "colorscheme.h"
#include "logger.h"
#include "misc.h"
#include "perf/graphwidget.h"
#include "perf/sidepanelgroup.h"
#include "perf/sidepanelorderdialog.h"
#include "ui/uihelper.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPalette>
#include <QPainter>
#include <QRegularExpression>
#include <QSplitter>
#include <QSplitterHandle>

namespace
{
    class PerformanceSplitterHandle : public QSplitterHandle
    {
        public:
            explicit PerformanceSplitterHandle(Qt::Orientation orientation, QSplitter *parent)
                : QSplitterHandle(orientation, parent)
            {}

        protected:
            void paintEvent(QPaintEvent *) override
            {
                // This brings back the line effect that we lost when we moved to a splitter
                QPainter painter(this);
                const QColor lineColor = ColorScheme::DetectDarkMode()
                    ? QColor(0x55, 0x55, 0x55)
                    : QColor(0xb8, 0xb8, 0xb8);
                painter.setPen(lineColor);

                // The splitter handle is wider than the visible separator so it remains
                // easy to grab. Draw only a single centered pixel column; painting at x=0
                // can disappear after Qt clips/repositions the handle during resizing.
                const int x = this->width() / 2;
                painter.drawLine(x, 0, x, this->height());
            }
    };

    class PerformanceSplitter : public QSplitter
    {
        public:
            explicit PerformanceSplitter(QWidget *parent = nullptr) : QSplitter(Qt::Horizontal, parent)
            {
                // This is the draggable area, not the visual line width. The handle paints
                // its own 1 px divider in the middle of this 5 px hit target.
                this->setHandleWidth(5);
            }

        protected:
            QSplitterHandle *createHandle() override
            {
                return new PerformanceSplitterHandle(this->orientation(), this);
            }
    };

    QList<Perf::SidePanelGroup> sanitizeSidePanelGroupOrder(const QStringList &storedOrder)
    {
        const QList<Perf::SidePanelGroup> defaults = Perf::DefaultSidePanelGroupOrder();
        QList<Perf::SidePanelGroup> sanitized;
        for (const QString &id : storedOrder)
        {
            const auto group = Perf::SidePanelGroupFromId(id);
            if (group.has_value() && !sanitized.contains(*group))
                sanitized.append(*group);
        }

        for (Perf::SidePanelGroup group : defaults)
        {
            if (!sanitized.contains(group))
                sanitized.append(group);
        }

        return sanitized;
    }

    class PerformanceStackedWidget : public QStackedWidget
    {
        public:
            using QStackedWidget::QStackedWidget;

            QSize minimumSizeHint() const override
            {
                if (QWidget *w = this->currentWidget())
                    return w->minimumSizeHint();
                return QSize(200, 150);
            }
    };

    QStringList serializeSidePanelGroupOrder(const QList<Perf::SidePanelGroup> &order)
    {
        QStringList out;
        out.reserve(order.size());
        for (Perf::SidePanelGroup group : order)
            out.append(Perf::SidePanelGroupId(group));
        return out;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction
////////////////////////////////////////////////////////////////////////////////////////////////////

PerformanceWidget *PerformanceWidget::s_instance = nullptr;

PerformanceWidget::PerformanceWidget(QWidget *parent) : QWidget(parent), ui(new Ui::PerformanceWidget)
{
    s_instance = this;

    // Ensure the metrics are initialized before we start enumerating devices
    Metrics::Get();

    this->m_sidePanel = new Perf::SidePanel(this);
    this->m_stack = new PerformanceStackedWidget(this);
    this->m_cpuDetail = new Perf::CpuDetailWidget(this);
    this->m_memDetail = new Perf::MemoryDetailWidget(this);
    this->m_swapDetail = new Perf::SwapDetailWidget(this);

    this->ui->setupUi(this);

    this->setupLayout();
    this->setupSidePanel();
    this->applySidePanelOrder();

    // Wire detail widgets to the data provider
    this->m_cpuDetail->Init();
    this->m_memDetail->Init();
    this->m_swapDetail->Init();

    // Update side panel thumbnails on every sample
    connect(Metrics::Get(), &Metrics::updated, this, &PerformanceWidget::onProviderUpdated);

    // Expensive process/thread counting is only needed for CPU detail page.
    connect(this->m_sidePanel, &Perf::SidePanel::currentChanged, this, [this](Perf::SidePanelItem *item)
    {
        QWidget *detail = this->m_detailByItem.value(item, nullptr);
        if (detail)
            this->m_stack->setCurrentWidget(detail);
        Metrics::Get()->SetProcessStatsEnabled(item == this->m_cpuItem && CFG->PerfShowCpu);
    });
    connect(this->m_sidePanel, &Perf::SidePanel::itemContextMenuRequested, this, &PerformanceWidget::onSidePanelContextMenu);

    this->tagTimeAxisLabels();
    this->applyGraphWindowSeconds();
    this->applyPanelVisibility();
    this->updateSamplingPolicy();
    if (this->m_sidePanel->GetCurrentItem())
    {
        QWidget *detail = this->m_detailByItem.value(this->m_sidePanel->GetCurrentItem(), nullptr);
        if (detail)
            this->m_stack->setCurrentWidget(detail);
    }
    Metrics::Get()->SetProcessStatsEnabled(this->m_sidePanel->GetCurrentItem() == this->m_cpuItem && CFG->PerfShowCpu);

    this->SetActive(false);

    LOG_DEBUG("PerformanceWidget initialised");
}

PerformanceWidget::~PerformanceWidget()
{
    s_instance = nullptr;
    delete this->ui;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Private setup
////////////////////////////////////////////////////////////////////////////////////////////////////

void PerformanceWidget::setupLayout()
{
    // The .ui gives us a bare QHBoxLayout (horizontalLayout) — populate it.
    QHBoxLayout *lay = qobject_cast<QHBoxLayout *>(this->layout());

    this->m_splitter = new PerformanceSplitter(this);
    this->m_splitter->setChildrenCollapsible(false);
    this->m_splitter->addWidget(this->m_sidePanel);
    this->m_splitter->addWidget(this->m_stack);
    this->m_splitter->setStretchFactor(0, 0);
    this->m_splitter->setStretchFactor(1, 1);

    const QByteArray saved_state = CFG->PerformanceSplitterState;
    if (!saved_state.isEmpty())
        this->m_splitter->restoreState(saved_state);
    else
        this->m_splitter->setSizes({ 162, 680 });

    connect(this->m_splitter, &QSplitter::splitterMoved, this, [this](int, int)
    {
        CFG->PerformanceSplitterState = this->m_splitter->saveState();
    });

    lay->addWidget(this->m_splitter);
}

void PerformanceWidget::setupSidePanel()
{
    const ColorScheme *scheme = ColorScheme::GetCurrent();

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    // CPU item
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    this->m_cpuItem = new Perf::SidePanelItem(tr("CPU"), this);
    this->m_cpuItem->SetGraphColor(scheme->CpuGraphLineColor, scheme->CpuGraphFillColor);
    this->m_cpuItem->SetGraphSource(Metrics::GetCPU()->CpuHistory());
    this->m_cpuItem->setProperty("deviceId", "cpu");
    this->m_cpuItem->setProperty("deviceName", tr("CPU"));
    this->m_cpuItem->setProperty("deviceGroup", static_cast<int>(Perf::SidePanelGroup::Cpu));
    this->m_sidePanel->AddItem(this->m_cpuItem);
    this->m_stack->addWidget(this->m_cpuDetail);
    this->m_detailByItem.insert(this->m_cpuItem, this->m_cpuDetail);

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    // Memory item
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    this->m_memoryItem = new Perf::SidePanelItem(tr("Memory"), this);
    this->m_memoryItem->SetGraphColor(scheme->MemoryGraphLineColor, scheme->MemoryGraphFillColor);
    this->m_memoryItem->SetGraphSource(Metrics::GetMemory()->MemHistory());
    this->m_memoryItem->setProperty("deviceId", "memory");
    this->m_memoryItem->setProperty("deviceName", tr("Memory"));
    this->m_memoryItem->setProperty("deviceGroup", static_cast<int>(Perf::SidePanelGroup::Memory));
    this->m_sidePanel->AddItem(this->m_memoryItem);
    this->m_stack->addWidget(this->m_memDetail);
    this->m_detailByItem.insert(this->m_memoryItem, this->m_memDetail);

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    // Swap item
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    this->m_swapItem = new Perf::SidePanelItem(tr("Swap"), this);
    this->m_swapItem->SetGraphColor(scheme->SwapUsageGraphLineColor, scheme->SwapUsageGraphFillColor);
    this->m_swapItem->SetGraphSource(Metrics::GetSwap()->SwapUsageHistory());
    this->m_swapItem->setProperty("deviceId", "swap");
    this->m_swapItem->setProperty("deviceName", tr("Swap"));
    this->m_swapItem->setProperty("deviceGroup", static_cast<int>(Perf::SidePanelGroup::Swap));
    this->m_sidePanel->AddItem(this->m_swapItem);
    this->m_stack->addWidget(this->m_swapDetail);
    this->m_detailByItem.insert(this->m_swapItem, this->m_swapDetail);

    this->setupDiskPanels();
    this->setupNetworkPanels();
    this->setupGpuPanels();

    this->installSidebarToggle(this->m_cpuDetail);
    this->installSidebarToggle(this->m_memDetail);
    this->installSidebarToggle(this->m_swapDetail);
    for (auto *disk : this->m_diskDetails)
        this->installSidebarToggle(disk);
    for (auto *net : this->m_networkDetails)
        this->installSidebarToggle(net);
    for (auto *gpu : this->m_gpuDetails)
        this->installSidebarToggle(gpu);
}

void PerformanceWidget::setupDiskPanels()
{
    const ColorScheme *scheme = ColorScheme::GetCurrent();
    const int count = Metrics::GetStorage()->DiskCount();
    for (int i = 0; i < count; ++i)
    {
        const Storage::DiskInfo &disk = Metrics::GetStorage()->FromIndex(i);
        this->m_diskNames.append(disk.Name);

        auto *item = new Perf::SidePanelItem(tr("Disk (%1)").arg(disk.Name), this);
        item->SetGraphColor(scheme->DiskGraphLineColor, scheme->DiskGraphFillColor);
        item->SetGraphSource(disk.ActiveHistory);
        item->setProperty("deviceId", QString("disk:%1").arg(disk.Name));
        item->setProperty("deviceName", tr("Disk (%1)").arg(disk.Name));
        item->setProperty("deviceGroup", static_cast<int>(Perf::SidePanelGroup::Disks));
        this->m_sidePanel->AddItem(item);
        this->m_diskItems.append(item);

        auto *detail = new Perf::DiskDetailWidget(this);
        detail->SetDisk(i);
        this->m_stack->addWidget(detail);
        this->m_diskDetails.append(detail);
        this->m_detailByItem.insert(item, detail);
    }
}

void PerformanceWidget::setupGpuPanels()
{
    const ColorScheme *scheme = ColorScheme::GetCurrent();
    const int count = Metrics::GetGPU()->GpuCount();
    for (int i = 0; i < count; ++i)
    {
        const GPU::GPUInfo &gpu = Metrics::GetGPU()->FromIndex(i);
        this->m_gpuNames.append(gpu.Name);

        auto *item = new Perf::SidePanelItem(tr("GPU %1").arg(i), this);
        item->SetGraphColor(scheme->GpuGraphLineColor, scheme->GpuGraphFillColor);
        item->SetGraphSource(gpu.UtilHistory);
        item->setProperty("deviceId", QString("gpu:%1").arg(i));
        item->setProperty("deviceName", tr("GPU %1").arg(i));
        item->setProperty("deviceGroup", static_cast<int>(Perf::SidePanelGroup::Gpu));
        this->m_sidePanel->AddItem(item);
        this->m_gpuItems.append(item);

        auto *detail = new Perf::GpuDetailWidget(this);
        detail->SetGpu(i);
        this->m_stack->addWidget(detail);
        this->m_gpuDetails.append(detail);
        this->m_detailByItem.insert(item, detail);
    }
}

void PerformanceWidget::setupNetworkPanels()
{
    const ColorScheme *scheme = ColorScheme::GetCurrent();
    const int count = Metrics::GetNetwork()->NetworkCount();
    for (int i = 0; i < count; ++i)
    {
        const Network::NetworkInfo &network = Metrics::GetNetwork()->FromIndex(i);
        this->m_networkNames.append(network.Name);

        auto *item = new Perf::SidePanelItem(tr("NIC (%1)").arg(network.Name), this);
        item->SetGraphColor(scheme->NetworkGraphLineColor, scheme->NetworkGraphFillColor);
        item->SetGraphSource(network.RxHistory, 1024.0);

        const bool isVirtual = QFile::exists(QString("/sys/devices/virtual/net/%1").arg(network.Name));
        item->setProperty("deviceId", QString("net:%1").arg(network.Name));
        item->setProperty("deviceName", tr("NIC (%1)").arg(network.Name));
        item->setProperty("isVirtualNetwork", isVirtual);
        item->setProperty("deviceGroup", static_cast<int>(Perf::SidePanelGroup::Network));

        this->m_sidePanel->AddItem(item);
        this->m_networkItems.append(item);

        auto *detail = new Perf::NetworkDetailWidget(this);
        detail->SetNetwork(i);
        this->m_stack->addWidget(detail);
        this->m_networkDetails.append(detail);
        this->m_detailByItem.insert(item, detail);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Slots
////////////////////////////////////////////////////////////////////////////////////////////////////

void PerformanceWidget::onProviderUpdated()
{
    // Update CPU side panel item
    const double cpuPct = Metrics::GetCPU()->CpuPercent();
    const int cpuTempC = Metrics::GetCPU()->CpuTemperatureC();
    const QString cpuSub = (cpuTempC >= 0)
                           ? tr("%1%2 %3C", "%1=value %2=percent sign %3=temperature in Celsius")
                                 .arg(QString::number(cpuPct, 'f', 0), "%", QString::number(cpuTempC))
                           : QString::number(cpuPct, 'f', 0) + "%";
    if (this->m_sidePanel->IsItemVisible(this->m_cpuItem))
        this->m_cpuItem->Update(cpuSub);

    // Update Memory side panel item
    const qint64 used  = Metrics::GetMemory()->MemUsedKb();
    const qint64 total = Metrics::GetMemory()->MemTotalKb();
    const int    pct     = total > 0
                           ? static_cast<int>(static_cast<double>(used) / total * 100.0)
                           : 0;
    const QString memSub = QString("%1/%2 (%3%)")
                           .arg(Misc::FormatKiB(static_cast<quint64>(qMax<qint64>(0, used)), 1),
                                Misc::FormatKiB(static_cast<quint64>(qMax<qint64>(0, total)), 1),
                                QString::number(pct));
    if (this->m_sidePanel->IsItemVisible(this->m_memoryItem))
        this->m_memoryItem->Update(memSub);

    // Update Swap side panel item
    const qint64 swapUsed = Metrics::GetSwap()->SwapUsedKb();
    const qint64 swapTotal = Metrics::GetSwap()->SwapTotalKb();
    const int swapPct = (swapTotal > 0)
                        ? static_cast<int>(static_cast<double>(swapUsed) / static_cast<double>(swapTotal) * 100.0)
                        : 0;
    QString swapSub;
    if (swapTotal > 0)
    {
        swapSub = QString("%1/%2 (%3%)")
                  .arg(Misc::FormatKiB(static_cast<quint64>(qMax<qint64>(0, swapUsed)), 1),
                       Misc::FormatKiB(static_cast<quint64>(qMax<qint64>(0, swapTotal)), 1),
                       QString::number(swapPct));
    } else
    {
        swapSub = tr("Off");
    }

    if (this->m_sidePanel->IsItemVisible(this->m_swapItem))
        this->m_swapItem->Update(swapSub);

    if (CFG->PerfShowDisks)
    {
        for (int i = 0; i < this->m_diskItems.size(); ++i)
        {
            if (i >= Metrics::GetStorage()->DiskCount())
                break;
            auto *item = this->m_diskItems.at(i);
            if (!item || !this->m_sidePanel->IsItemVisible(item))
                continue;

            const Storage::DiskInfo &disk = Metrics::GetStorage()->FromIndex(i);
            const QString diskSub = tr("%1 %2", "%1=disk type %2=active percentage").arg(disk.Type, QString::number(disk.ActivePct, 'f', 0) + "%");
            item->Update(diskSub);
        }
    }

    if (CFG->PerfShowGpu)
    {
        for (int i = 0; i < this->m_gpuItems.size(); ++i)
        {
            if (i >= Metrics::GetGPU()->GpuCount())
                break;
            auto *item = this->m_gpuItems.at(i);
            if (!item || !this->m_sidePanel->IsItemVisible(item))
                continue;

            const GPU::GPUInfo &gpu = Metrics::GetGPU()->FromIndex(i);
            const QString utilText = tr("%1%2", "%1=GPU utilization value %2=percent sign").arg(QString::number(gpu.UtilPct, 'f', 0), "%");
            const int tempC = gpu.TemperatureC;
            const QString sub = (tempC >= 0)
                                ? tr("%1 %2C", "%1=GPU utilization %2=temperature in Celsius")
                                      .arg(utilText, QString::number(tempC))
                                : utilText;
            item->Update(sub);
        }
    }

    if (CFG->PerfShowNetwork)
    {
        for (int i = 0; i < this->m_networkItems.size(); ++i)
        {
            if (i >= Metrics::GetNetwork()->NetworkCount())
                break;
            auto *item = this->m_networkItems.at(i);
            if (!item || !this->m_sidePanel->IsItemVisible(item))
                continue;

            const Network::NetworkInfo &network = Metrics::GetNetwork()->FromIndex(i);
            const QString uploadRate = CFG->PerfNetworkUseBits
                                       ? Misc::FormatBitsPerSecond(network.TxBps)
                                       : Misc::FormatBytesPerSecond(network.TxBps);
            const QString downloadRate = CFG->PerfNetworkUseBits
                                         ? Misc::FormatBitsPerSecond(network.RxBps)
                                         : Misc::FormatBytesPerSecond(network.RxBps);
            const QString netSub = tr("U:%1 D:%2", "%1=upload rate %2=download rate").arg(uploadRate, downloadRate);
            item->Update(netSub, network.MaxThroughputBps);
        }
    }
}

void PerformanceWidget::SetActive(bool active)
{
    if (this->m_active == active)
        return;

    this->m_active = active;
    if (active)
    {
        this->applyPanelVisibility();
        this->updateSamplingPolicy();
    }
    Metrics::Get()->SetActive(active);
    if (active)
        this->onProviderUpdated();
}

void PerformanceWidget::onSidePanelContextMenu(Perf::SidePanelItem *item, const QPoint &globalPos)
{
    QMenu menu(this);

    // Count visible items to prevent hiding the last visible item
    int visibleCount = 0;
    if (this->m_sidePanel->IsItemVisible(this->m_cpuItem)) visibleCount++;
    if (this->m_sidePanel->IsItemVisible(this->m_memoryItem)) visibleCount++;
    if (this->m_sidePanel->IsItemVisible(this->m_swapItem)) visibleCount++;
    for (Perf::SidePanelItem *it : std::as_const(this->m_diskItems))
        if (this->m_sidePanel->IsItemVisible(it)) visibleCount++;
    for (Perf::SidePanelItem *it : std::as_const(this->m_networkItems))
        if (this->m_sidePanel->IsItemVisible(it)) visibleCount++;
    for (Perf::SidePanelItem *it : std::as_const(this->m_gpuItems))
        if (this->m_sidePanel->IsItemVisible(it)) visibleCount++;

    const int totalItems = 3 + this->m_diskItems.size() + this->m_networkItems.size() + this->m_gpuItems.size();
    const int hiddenCount = qMax(0, totalItems - visibleCount);

    QAction *hideCurrentItem = nullptr;
    QAction *hideAllVirtualNets = nullptr;

    if (item)
    {
        const QString devName = item->property("deviceName").toString();
        if (!devName.isEmpty())
        {
            hideCurrentItem = menu.addAction(tr("Hide '%1'").arg(devName));
            if (visibleCount <= 1)
                hideCurrentItem->setEnabled(false);
        }

        if (item->property("isVirtualNetwork").toBool() && !CFG->PerfHideVirtualNetworks)
        {
            hideAllVirtualNets = menu.addAction(tr("Hide all virtual networks"));
        }

        menu.addSeparator();
    }

    QAction *cpu = nullptr;
    QAction *memory = nullptr;
    QAction *swap = nullptr;

    QAction *showAllDisks = nullptr;
    QHash<QAction *, QString> diskActionMap;

    QAction *showAllNet = nullptr;
    QAction *hideVirtualNet = nullptr;
    QHash<QAction *, QString> netActionMap;

    QAction *showAllGpu = nullptr;
    QHash<QAction *, QString> gpuActionMap;

    const QList<Perf::SidePanelGroup> groupOrder = sanitizeSidePanelGroupOrder(CFG->PerfSidePanelGroupOrder);
    for (Perf::SidePanelGroup group : groupOrder)
    {
        switch (group)
        {
            case Perf::SidePanelGroup::Cpu:
            {
                cpu = menu.addAction(Perf::SidePanelGroupLabel(group));
                cpu->setCheckable(true);
                cpu->setChecked(CFG->PerfShowCpu && !CFG->PerfHiddenDevices.contains("cpu"));
                break;
            }
            case Perf::SidePanelGroup::Memory:
            {
                memory = menu.addAction(Perf::SidePanelGroupLabel(group));
                memory->setCheckable(true);
                memory->setChecked(CFG->PerfShowMemory && !CFG->PerfHiddenDevices.contains("memory"));
                break;
            }
            case Perf::SidePanelGroup::Swap:
            {
                swap = menu.addAction(Perf::SidePanelGroupLabel(group));
                swap->setCheckable(true);
                swap->setChecked(CFG->PerfShowSwap && !CFG->PerfHiddenDevices.contains("swap"));
                break;
            }
            case Perf::SidePanelGroup::Disks:
            {
                QMenu *disksMenu = menu.addMenu(Perf::SidePanelGroupLabel(group));
                showAllDisks = disksMenu->addAction(tr("Show Disks"));
                showAllDisks->setCheckable(true);
                showAllDisks->setChecked(CFG->PerfShowDisks);

                if (!this->m_diskItems.isEmpty())
                {
                    disksMenu->addSeparator();
                    for (Perf::SidePanelItem *diskItem : std::as_const(this->m_diskItems))
                    {
                        const QString devId = diskItem->property("deviceId").toString();
                        const QString devName = diskItem->property("deviceName").toString();
                        QAction *act = disksMenu->addAction(devName);
                        act->setCheckable(true);
                        act->setChecked(CFG->PerfShowDisks && !CFG->PerfHiddenDevices.contains(devId));
                        act->setEnabled(CFG->PerfShowDisks);
                        diskActionMap.insert(act, devId);
                    }
                }
                break;
            }
            case Perf::SidePanelGroup::Network:
            {
                QMenu *netMenu = menu.addMenu(Perf::SidePanelGroupLabel(group));
                showAllNet = netMenu->addAction(tr("Show Network"));
                showAllNet->setCheckable(true);
                showAllNet->setChecked(CFG->PerfShowNetwork);

                hideVirtualNet = netMenu->addAction(tr("Hide virtual networks"));
                hideVirtualNet->setCheckable(true);
                hideVirtualNet->setChecked(CFG->PerfHideVirtualNetworks);
                hideVirtualNet->setEnabled(CFG->PerfShowNetwork);

                if (!this->m_networkItems.isEmpty())
                {
                    netMenu->addSeparator();
                    for (Perf::SidePanelItem *netItem : std::as_const(this->m_networkItems))
                    {
                        const QString devId = netItem->property("deviceId").toString();
                        const QString devName = netItem->property("deviceName").toString();
                        const bool isVirtual = netItem->property("isVirtualNetwork").toBool();
                        QString label = devName;
                        if (isVirtual)
                            label += tr(" (virtual)");
                        QAction *act = netMenu->addAction(label);
                        act->setCheckable(true);
                        const bool isHidden = CFG->PerfHiddenDevices.contains(devId) || (isVirtual && CFG->PerfHideVirtualNetworks);
                        act->setChecked(CFG->PerfShowNetwork && !isHidden);
                        act->setEnabled(CFG->PerfShowNetwork && !(isVirtual && CFG->PerfHideVirtualNetworks));
                        netActionMap.insert(act, devId);
                    }
                }
                break;
            }
            case Perf::SidePanelGroup::Gpu:
            {
                QMenu *gpuMenu = menu.addMenu(Perf::SidePanelGroupLabel(group));
                showAllGpu = gpuMenu->addAction(tr("Show GPU"));
                showAllGpu->setCheckable(true);
                showAllGpu->setChecked(CFG->PerfShowGpu);

                if (!this->m_gpuItems.isEmpty())
                {
                    gpuMenu->addSeparator();
                    for (Perf::SidePanelItem *gpuItem : std::as_const(this->m_gpuItems))
                    {
                        const QString devId = gpuItem->property("deviceId").toString();
                        const QString devName = gpuItem->property("deviceName").toString();
                        QAction *act = gpuMenu->addAction(devName);
                        act->setCheckable(true);
                        act->setChecked(CFG->PerfShowGpu && !CFG->PerfHiddenDevices.contains(devId));
                        act->setEnabled(CFG->PerfShowGpu);
                        gpuActionMap.insert(act, devId);
                    }
                }
                break;
            }
        }
    }

    QAction *unhideAllMain = nullptr;
    if (hiddenCount > 0)
    {
        menu.addSeparator();
        unhideAllMain = menu.addAction(tr("Unhide all hidden graphs (%1)").arg(hiddenCount));
    }

    menu.addSeparator();
    QMenu *settingsMenu = menu.addMenu(tr("Settings"));
    QAction *customizeOrder = settingsMenu->addAction(tr("Customize order..."));
    QAction *customizeColors = settingsMenu->addAction(tr("Customize colors..."));
    QAction *showGrid = settingsMenu->addAction(tr("Show grid in side panel"));
    showGrid->setCheckable(true);
    showGrid->setChecked(CFG->SidePanelGridEnabled);

    settingsMenu->addSeparator();
    QAction *unhideAllSettings = settingsMenu->addAction(hiddenCount > 0
        ? tr("Unhide all hidden graphs (%1)").arg(hiddenCount)
        : tr("Unhide all hidden graphs"));
    unhideAllSettings->setEnabled(hiddenCount > 0);

    menu.addSeparator();
    UIHelper::AddRefreshIntervalContextMenu(&menu, nullptr, this->m_active);
    UIHelper::AddGraphWindowContextMenu(&menu);

    menu.addSeparator();
    UIHelper::AddGlobalContextMenuItems(&menu, this);

    QAction *picked = menu.exec(globalPos);
    if (!picked)
        return;

    if (picked == hideCurrentItem && item)
    {
        const QString devId = item->property("deviceId").toString();
        if (devId == "cpu")
            CFG->PerfShowCpu = false;
        else if (devId == "memory")
            CFG->PerfShowMemory = false;
        else if (devId == "swap")
            CFG->PerfShowSwap = false;
        else if (!devId.isEmpty())
        {
            if (!CFG->PerfHiddenDevices.contains(devId))
                CFG->PerfHiddenDevices.append(devId);
        }

        this->applyPanelVisibility();
        this->updateSamplingPolicy();
        CFG->Save();
        if (this->m_active)
            this->onProviderUpdated();
        return;
    }

    if (picked == hideAllVirtualNets)
    {
        CFG->PerfHideVirtualNetworks = true;
        this->applyPanelVisibility();
        this->updateSamplingPolicy();
        CFG->Save();
        if (this->m_active)
            this->onProviderUpdated();
        return;
    }

    if ((unhideAllMain && picked == unhideAllMain) || picked == unhideAllSettings)
    {
        CFG->PerfShowCpu = true;
        CFG->PerfShowMemory = true;
        CFG->PerfShowSwap = true;
        CFG->PerfShowDisks = true;
        CFG->PerfShowNetwork = true;
        CFG->PerfShowGpu = true;
        CFG->PerfHiddenDevices.clear();
        CFG->PerfHideVirtualNetworks = false;
        this->applyPanelVisibility();
        this->updateSamplingPolicy();
        CFG->Save();
        if (this->m_active)
            this->onProviderUpdated();
        return;
    }

    if (picked == customizeOrder)
    {
        SidePanelOrderDialog dialog(sanitizeSidePanelGroupOrder(CFG->PerfSidePanelGroupOrder), this);
        if (dialog.exec() != QDialog::Accepted)
            return;

        CFG->PerfSidePanelGroupOrder = serializeSidePanelGroupOrder(dialog.GetOrder());
        this->applySidePanelOrder();
        CFG->Save();
        return;
    }

    if (picked == customizeColors)
    {
        ColorSchemeDialog dialog(this);
        if (dialog.exec() != QDialog::Accepted)
            return;

        CFG->UseCustomColorScheme = dialog.UseCustomScheme();
        const ColorScheme scheme = dialog.BuildScheme();
        CFG->CustomColorScheme = scheme.ToVariantMap();

        if (CFG->UseCustomColorScheme)
            ColorScheme::Install(new ColorScheme(scheme));
        else
            ColorScheme::Install(new ColorScheme(ColorScheme::DetectDarkMode()
                                                 ? ColorScheme::DefaultDark()
                                                 : ColorScheme::DefaultLight()));

        this->ApplyColorScheme();
        CFG->Save();
        return;
    }

    if (picked == showGrid)
    {
        CFG->SidePanelGridEnabled = showGrid->isChecked();
        this->applySidePanelGridEnabled();
        CFG->Save();
        return;
    }

    if (picked == cpu)
    {
        CFG->PerfShowCpu = cpu->isChecked();
        CFG->PerfHiddenDevices.removeAll("cpu");
    }
    else if (picked == memory)
    {
        CFG->PerfShowMemory = memory->isChecked();
        CFG->PerfHiddenDevices.removeAll("memory");
    }
    else if (picked == swap)
    {
        CFG->PerfShowSwap = swap->isChecked();
        CFG->PerfHiddenDevices.removeAll("swap");
    }
    else if (picked == showAllDisks)
    {
        CFG->PerfShowDisks = showAllDisks->isChecked();
    }
    else if (picked == showAllNet)
    {
        CFG->PerfShowNetwork = showAllNet->isChecked();
    }
    else if (picked == showAllGpu)
    {
        CFG->PerfShowGpu = showAllGpu->isChecked();
    }
    else if (picked == hideVirtualNet)
    {
        CFG->PerfHideVirtualNetworks = hideVirtualNet->isChecked();
    }
    else if (diskActionMap.contains(picked))
    {
        const QString devId = diskActionMap.value(picked);
        if (picked->isChecked())
            CFG->PerfHiddenDevices.removeAll(devId);
        else if (!CFG->PerfHiddenDevices.contains(devId))
            CFG->PerfHiddenDevices.append(devId);
    }
    else if (netActionMap.contains(picked))
    {
        const QString devId = netActionMap.value(picked);
        if (picked->isChecked())
            CFG->PerfHiddenDevices.removeAll(devId);
        else if (!CFG->PerfHiddenDevices.contains(devId))
            CFG->PerfHiddenDevices.append(devId);
    }
    else if (gpuActionMap.contains(picked))
    {
        const QString devId = gpuActionMap.value(picked);
        if (picked->isChecked())
            CFG->PerfHiddenDevices.removeAll(devId);
        else if (!CFG->PerfHiddenDevices.contains(devId))
            CFG->PerfHiddenDevices.append(devId);
    }

    this->applyPanelVisibility();
    this->updateSamplingPolicy();
    CFG->Save();
    if (this->m_active)
        this->onProviderUpdated();
}

void PerformanceWidget::ApplyColorScheme()
{
    const ColorScheme *scheme = ColorScheme::GetCurrent();
    this->m_sidePanel->ApplyColorScheme();

    auto applySidePanelItem = [](Perf::SidePanelItem *item, const QColor &line, const QColor &fill)
    {
        if (!item)
            return;
        item->SetGraphColor(line, fill);
        item->update();
    };

    applySidePanelItem(this->m_cpuItem, scheme->CpuGraphLineColor, scheme->CpuGraphFillColor);
    applySidePanelItem(this->m_memoryItem, scheme->MemoryGraphLineColor, scheme->MemoryGraphFillColor);
    applySidePanelItem(this->m_swapItem, scheme->SwapUsageGraphLineColor, scheme->SwapUsageGraphFillColor);

    for (Perf::SidePanelItem *item : std::as_const(this->m_diskItems))
        applySidePanelItem(item, scheme->DiskGraphLineColor, scheme->DiskGraphFillColor);
    for (Perf::SidePanelItem *item : std::as_const(this->m_networkItems))
        applySidePanelItem(item, scheme->NetworkGraphLineColor, scheme->NetworkGraphFillColor);
    for (Perf::SidePanelItem *item : std::as_const(this->m_gpuItems))
        applySidePanelItem(item, scheme->GpuGraphLineColor, scheme->GpuGraphFillColor);

    this->m_cpuDetail->ApplyColorScheme();
    this->m_memDetail->ApplyColorScheme();
    this->m_swapDetail->ApplyColorScheme();
    for (Perf::DiskDetailWidget *detail : std::as_const(this->m_diskDetails))
        if (detail)
            detail->ApplyColorScheme();
    for (Perf::NetworkDetailWidget *detail : std::as_const(this->m_networkDetails))
        if (detail)
            detail->ApplyColorScheme();
    for (Perf::GpuDetailWidget *detail : std::as_const(this->m_gpuDetails))
        if (detail)
            detail->ApplyColorScheme();

    if (this->m_splitter)
        this->m_splitter->update();
    this->m_sidePanel->update();
    this->update();
}

void PerformanceWidget::applySidePanelOrder()
{
    const QList<Perf::SidePanelGroup> order = sanitizeSidePanelGroupOrder(CFG->PerfSidePanelGroupOrder);
    CFG->PerfSidePanelGroupOrder = serializeSidePanelGroupOrder(order);

    QList<Perf::SidePanelItem *> items;
    items.reserve(this->m_sidePanel->GetCount());

    for (Perf::SidePanelGroup group : order)
    {
        switch (group)
        {
            case Perf::SidePanelGroup::Cpu:
                items.append(this->m_cpuItem);
                break;
            case Perf::SidePanelGroup::Memory:
                items.append(this->m_memoryItem);
                break;
            case Perf::SidePanelGroup::Swap:
                items.append(this->m_swapItem);
                break;
            case Perf::SidePanelGroup::Disks:
                for (Perf::SidePanelItem *item : std::as_const(this->m_diskItems))
                    items.append(item);
                break;
            case Perf::SidePanelGroup::Network:
                for (Perf::SidePanelItem *item : std::as_const(this->m_networkItems))
                    items.append(item);
                break;
            case Perf::SidePanelGroup::Gpu:
                for (Perf::SidePanelItem *item : std::as_const(this->m_gpuItems))
                    items.append(item);
                break;
        }
    }

    this->m_sidePanel->SetItemOrder(items);
}

void PerformanceWidget::applyPanelVisibility()
{
    auto isItemVisible = [](Perf::SidePanelItem *item, bool groupEnabled) -> bool {
        if (!item || !groupEnabled)
            return false;
        const QString devId = item->property("deviceId").toString();
        if (!devId.isEmpty() && CFG->PerfHiddenDevices.contains(devId))
            return false;
        if (item->property("isVirtualNetwork").toBool() && CFG->PerfHideVirtualNetworks)
            return false;
        return true;
    };

    bool cpuVis = isItemVisible(this->m_cpuItem, CFG->PerfShowCpu);
    bool memVis = isItemVisible(this->m_memoryItem, CFG->PerfShowMemory);
    bool swapVis = isItemVisible(this->m_swapItem, CFG->PerfShowSwap);

    bool anyDiskVis = false;
    for (Perf::SidePanelItem *item : std::as_const(this->m_diskItems))
    {
        const bool vis = isItemVisible(item, CFG->PerfShowDisks);
        this->m_sidePanel->SetItemVisible(item, vis);
        if (vis)
            anyDiskVis = true;
    }

    bool anyNetVis = false;
    for (Perf::SidePanelItem *item : std::as_const(this->m_networkItems))
    {
        const bool vis = isItemVisible(item, CFG->PerfShowNetwork);
        this->m_sidePanel->SetItemVisible(item, vis);
        if (vis)
            anyNetVis = true;
    }

    bool anyGpuVis = false;
    for (Perf::SidePanelItem *item : std::as_const(this->m_gpuItems))
    {
        const bool vis = isItemVisible(item, CFG->PerfShowGpu);
        this->m_sidePanel->SetItemVisible(item, vis);
        if (vis)
            anyGpuVis = true;
    }

    // Safety fallback: ensure at least one item remains visible
    if (!cpuVis && !memVis && !swapVis && !anyDiskVis && !anyNetVis && !anyGpuVis)
    {
        cpuVis = true;
        CFG->PerfShowCpu = true;
        CFG->PerfHiddenDevices.removeAll("cpu");
    }

    this->m_sidePanel->SetItemVisible(this->m_cpuItem, cpuVis);
    this->m_sidePanel->SetItemVisible(this->m_memoryItem, memVis);
    this->m_sidePanel->SetItemVisible(this->m_swapItem, swapVis);

    Perf::SidePanelItem *first = this->m_sidePanel->FirstVisibleItem();
    if (first && !this->m_sidePanel->IsItemVisible(this->m_sidePanel->GetCurrentItem()))
        this->m_sidePanel->SetCurrentItem(first);
}

void PerformanceWidget::updateSamplingPolicy()
{
    const bool cpuVis = this->m_sidePanel->IsItemVisible(this->m_cpuItem);
    const bool memVis = this->m_sidePanel->IsItemVisible(this->m_memoryItem);
    const bool swapVis = this->m_sidePanel->IsItemVisible(this->m_swapItem);

    bool anyDiskVis = false;
    for (Perf::SidePanelItem *item : std::as_const(this->m_diskItems))
    {
        if (this->m_sidePanel->IsItemVisible(item))
        {
            anyDiskVis = true;
            break;
        }
    }

    bool anyNetVis = false;
    for (Perf::SidePanelItem *item : std::as_const(this->m_networkItems))
    {
        if (this->m_sidePanel->IsItemVisible(item))
        {
            anyNetVis = true;
            break;
        }
    }

    bool anyGpuVis = false;
    for (Perf::SidePanelItem *item : std::as_const(this->m_gpuItems))
    {
        if (this->m_sidePanel->IsItemVisible(item))
        {
            anyGpuVis = true;
            break;
        }
    }

    Metrics::Get()->SetCpuSamplingEnabled(cpuVis);
    Metrics::Get()->SetMemorySamplingEnabled(memVis);
    Metrics::Get()->SetSwapSamplingEnabled(swapVis);
    Metrics::Get()->SetDiskSamplingEnabled(anyDiskVis);
    Metrics::Get()->SetNetworkSamplingEnabled(anyNetVis);
    Metrics::Get()->SetGpuSamplingEnabled(anyGpuVis);
    Metrics::Get()->SetProcessStatsEnabled(cpuVis && this->m_sidePanel->GetCurrentItem() == this->m_cpuItem);
}

void PerformanceWidget::applySidePanelGridEnabled()
{
    const bool enabled = CFG->SidePanelGridEnabled;

    if (this->m_cpuItem)
        this->m_cpuItem->SetGraphGridEnabled(enabled);
    if (this->m_memoryItem)
        this->m_memoryItem->SetGraphGridEnabled(enabled);
    if (this->m_swapItem)
        this->m_swapItem->SetGraphGridEnabled(enabled);

    for (Perf::SidePanelItem *item : std::as_const(this->m_diskItems))
        item->SetGraphGridEnabled(enabled);
    for (Perf::SidePanelItem *item : std::as_const(this->m_networkItems))
        item->SetGraphGridEnabled(enabled);
    for (Perf::SidePanelItem *item : std::as_const(this->m_gpuItems))
        item->SetGraphGridEnabled(enabled);
}

void PerformanceWidget::tagTimeAxisLabels()
{
    static const QRegularExpression kSecondsRe("^[0-9]+\\s+seconds$");
    for (QLabel *label : this->findChildren<QLabel *>())
    {
        if (!label)
            continue;
        if (kSecondsRe.match(label->text()).hasMatch())
            label->setProperty("perfTimeAxisLabel", true);
    }
}

void PerformanceWidget::applyGraphWindowSeconds()
{
    const int sec = CFG->PerfGraphWindowSec;
    for (Perf::GraphWidget *g : this->findChildren<Perf::GraphWidget *>())
    {
        if (g)
            g->SetSampleCapacity(sec);
    }

    const QString labelText = Misc::SimplifyTime(sec);

    for (QLabel *label : this->findChildren<QLabel *>())
    {
        if (label && label->property("perfTimeAxisLabel").toBool())
            label->setText(labelText);
    }
}

void PerformanceWidget::installSidebarToggle(QWidget *detail)
{
    if (!detail)
        return;

    QBoxLayout *header = detail->findChild<QBoxLayout *>("headerLayout");
    if (!header)
        return;

    auto *btn = new QToolButton(detail);
    btn->setText(QString::fromUtf8("◫"));
    btn->setCheckable(true);
    btn->setChecked(this->m_sidePanel->isVisible());
    btn->setToolTip(tr("Toggle devices sidebar"));
    btn->setFixedSize(26, 26);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet("QToolButton { font-size: 13pt; border: 1px solid transparent; border-radius: 4px; padding: 0px; }"
                       "QToolButton:hover { background-color: rgba(128, 128, 128, 0.2); border: 1px solid rgba(128, 128, 128, 0.3); }"
                       "QToolButton:checked { background-color: rgba(128, 128, 128, 0.25); }");
    connect(btn, &QToolButton::clicked, this, [this]()
    {
        this->toggleSidePanel();
    });
    header->insertWidget(0, btn);
    this->m_sidebarToggleButtons.append(btn);
}

void PerformanceWidget::updateSidebarToggleButtons()
{
    const bool vis = this->m_sidePanel->isVisible();
    for (QToolButton *btn : this->m_sidebarToggleButtons)
    {
        if (btn)
            btn->setChecked(vis);
    }
}

void PerformanceWidget::setSidePanelVisible(bool visible)
{
    if (this->m_sidePanel->isVisible() == visible)
        return;
    this->m_sidePanel->setVisible(visible);
    this->updateSidebarToggleButtons();
}

bool PerformanceWidget::isSidePanelVisible() const
{
    return this->m_sidePanel->isVisible();
}

void PerformanceWidget::toggleSidePanel()
{
    this->setSidePanelVisible(!this->m_sidePanel->isVisible());
}

void PerformanceWidget::setCompactMode(bool compact)
{
    if (this->m_compactMode == compact)
        return;
    this->m_compactMode = compact;

    if (this->m_cpuDetail)
        this->m_cpuDetail->SetCompactMode(compact);
    if (this->m_memDetail)
        this->m_memDetail->SetCompactMode(compact);
    if (this->m_swapDetail)
        this->m_swapDetail->SetCompactMode(compact);
    for (auto *disk : this->m_diskDetails)
    {
        if (disk)
            disk->SetCompactMode(compact);
    }
    for (auto *net : this->m_networkDetails)
    {
        if (net)
            net->SetCompactMode(compact);
    }
    for (auto *gpu : this->m_gpuDetails)
    {
        if (gpu)
            gpu->SetCompactMode(compact);
    }
}

void PerformanceWidget::updateResponsiveLayout(int w, int h)
{
    if (w <= 0 || h <= 0)
        return;

    const int kSidePanelBreakpoint = 720;
    if (this->m_lastWidth > 0)
    {
        if (this->m_lastWidth >= kSidePanelBreakpoint && w < kSidePanelBreakpoint)
        {
            this->setSidePanelVisible(false);
            this->m_autoHiddenSidePanel = true;
        }
        else if (this->m_lastWidth < kSidePanelBreakpoint && w >= kSidePanelBreakpoint)
        {
            if (this->m_autoHiddenSidePanel || !this->m_sidePanel->isVisible())
            {
                this->setSidePanelVisible(true);
                this->m_autoHiddenSidePanel = false;
            }
        }
    }
    else
    {
        if (w < kSidePanelBreakpoint)
        {
            this->setSidePanelVisible(false);
            this->m_autoHiddenSidePanel = true;
        }
        else
        {
            this->setSidePanelVisible(true);
            this->m_autoHiddenSidePanel = false;
        }
    }

    const int kCompactBreakpointW = 520;
    const int kCompactBreakpointH = 480;

    const bool shouldBeCompact = (w < kCompactBreakpointW || h < kCompactBreakpointH);
    this->setCompactMode(shouldBeCompact);

    this->m_lastWidth = w;
    this->m_lastHeight = h;
}

void PerformanceWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    this->updateResponsiveLayout(this->width(), this->height());
}

void PerformanceWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    this->updateResponsiveLayout(this->width(), this->height());
}
