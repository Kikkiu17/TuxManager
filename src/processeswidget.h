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

#ifndef PROCESSESWIDGET_H
#define PROCESSESWIDGET_H

#include "os/processmodel.h"
#include "os/appregistry.h"
#include "os/processtreemodel.h"
#include "os/processfilterproxy.h"
#include "os/processrefreshservice.h"

#include <QModelIndex>
#include <QHeaderView>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QTreeView>
#include <QWidget>

#include <functional>

QT_BEGIN_NAMESPACE
namespace Ui
{
    class ProcessesWidget;
}
QT_END_NAMESPACE

class QComboBox;
class QMenu;

//! Houses both table and tree widgets with lists of processes, the table and tree all reuse
//! same context menu and filters, but are two separate widgets that user can toggle between
//!
//! we are only updating the active widget
class ProcessesWidget : public QWidget
{
    Q_OBJECT

    public:
        enum class ViewMode
        {
            Grouped = 0,
            Table = 1,
            Tree = 2
        };

        explicit ProcessesWidget(OS::ProcessRefreshService *processRefreshService, QWidget *parent = nullptr);
        ~ProcessesWidget();
        void SetActive(bool active);
        bool IsActive() const { return this->m_active; }
        void ClearSearchFilter();
        bool SelectProcessByPid(pid_t pid);
        void SetViewMode(ViewMode mode);

    private slots:
        void onTimerTick();
        void onRefreshFinished(int consumer, quint64 token, const QList<OS::Process> &processes, int totalTasks, qlonglong totalThreads);
        void onHeaderContextMenu(const QPoint &pos);
        void onTableContextMenu(const QPoint &pos);
        void onTreeContextMenu(const QPoint &pos);
        void updateStatusBar();

    private:
        Ui::ProcessesWidget      *ui;
        OS::ProcessModel         *m_model;
        OS::ProcessTreeModel     *m_treeModel;
        OS::AppRegistry          *m_appRegistry { nullptr };
        OS::ProcessFilterProxy   *m_proxy;
        QSortFilterProxyModel    *m_treeProxy;
        QTimer                   *m_refreshTimer;
        OS::ProcessRefreshService *m_processRefreshService { nullptr };
        QTreeView                *m_treeView { nullptr };
        QComboBox                *m_viewModeCombo { nullptr };
        QComboBox                *m_cpuModeCombo { nullptr };
        bool                      m_active { false };
        bool                      m_tableContextMenuOpen { false };
        ViewMode                  m_viewMode { ViewMode::Grouped };
        bool                      m_refreshInFlight { false };
        bool                      m_refreshPending { false };
        bool                      m_tableHeaderPersistenceEnabled { false };
        bool                      m_treeHeaderPersistenceEnabled { false };
        quint64                   m_refreshToken { 0 };
        pid_t                     m_pendingSelectPid { 0 };
        QList<OS::Process>        m_lastProcessSnapshot;
        int                       m_totalTasks { 0 };
        qlonglong                 m_totalThreads { 0 };
        QModelIndex               m_contextMenuTargetIndex;

        struct TreeStateSnapshot
        {
            QSet<pid_t>  ExpandedPids;
            QList<pid_t> SelectedLeafPids;
            QList<pid_t> SelectedGroupPrimaryPids;
            pid_t        CurrentPid { 0 };
            bool         CurrentIsGroup { false };
            int          ScrollPos { 0 };
        };

        void setupTable();
        void startRefresh();
        void setViewMode(ViewMode mode);
        void setTreeViewMode(bool enabled);
        void setCpuPerCore(bool perCore);
        void ensureCpuColumnWidth();
        void syncSortIndicators();
        void addCpuUsageMenu(QMenu *menu);
        void populateViewMenu(QMenu *viewMenu);
        void showHeaderContextMenu(QHeaderView *header, int columnCount, const std::function<QString(int)> &titleForColumn, const QPoint &pos);
        void saveTableHeaderState() const;
        void saveTreeHeaderState() const;
        void syncProcessColumnVisibility(int column, bool hidden);
        void syncAllProcessColumnVisibility();
        void updateIOMetricsEnabledState(bool triggerImmediateRefresh = true);
        bool anyIOMetricsColumnVisible() const;
        void updatePssMetricsEnabledState(bool triggerImmediateRefresh = true);
        bool anyPssMetricsColumnVisible() const;
        bool selectProcessInTree(pid_t pid);
        bool selectProcessInTable(pid_t pid);
        QList<OS::Process> filteredProcessesForTree(const QList<OS::Process> &processes) const;
        QVariant tableSelectionKeyFromProxy(const QModelIndex &proxyKeyIndex) const;
        void copyRowSelectionToClipboard();
        void copyCellSelectionToClipboard();
        void copySelectedPidsToClipboard();
        void terminateSelectedProcesses();
        void killSelectedProcesses();
        void promptAndSendCustomSignal();
        void runNewTask();
        void openTerminal();
        void setShowKernelTasks(bool checked);
        void setShowOtherUsersProcesses(bool checked);
        void setShowIcons(bool checked);
        void applyIconSetting();
        TreeStateSnapshot captureTreeState() const;
        void captureExpandedTreePids(const QModelIndex &parentProxy, QSet<pid_t> &expandedPids) const;
        void restoreExpandedTreePids(const QModelIndex &sourceParent, const QSet<pid_t> &expandedPids);
        void restoreTreeStateDeferred(const TreeStateSnapshot &snapshot, pid_t pendingSelectPid = 0);

        /// Collect PIDs of all currently selected rows.
        QList<pid_t> selectedPids() const;
        QList<OS::Process> selectedProcesses() const;

        /// Send signal to all selected processes; show error dialog on failure.
        void sendSignalToSelected(int signal);

        /// Open renice dialog for all selected processes.
        void reniceSelected();
        static QString findTerminalExecutable();
};

#endif // PROCESSESWIDGET_H
