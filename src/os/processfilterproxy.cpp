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

#include "processfilterproxy.h"
#include "processmodel.h"

#include <unistd.h>

using namespace OS;

ProcessFilterProxy::ProcessFilterProxy(QObject *parent) : QSortFilterProxyModel(parent), m_myUid(::getuid())
{
    this->setSortRole(Qt::UserRole);
    this->setSortCaseSensitivity(Qt::CaseInsensitive);
    this->setFilterRole(Qt::DisplayRole);
    this->setFilterCaseSensitivity(Qt::CaseInsensitive);
    this->setFilterKeyColumn(-1); // search across all columns
}

void ProcessFilterProxy::ApplyFilters()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    this->beginFilterChange();
    this->endFilterChange(QSortFilterProxyModel::Direction::Rows);
#elif QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    this->beginFilterChange();
    this->invalidateRowsFilter();
#else
    this->invalidateFilter();
#endif
}

bool ProcessFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const auto *model = qobject_cast<const ProcessModel *>(this->sourceModel());
    if (!model)
        return true;

    const QList<Process> &procs = model->GetProcesses();
    if (sourceRow < 0 || sourceRow >= procs.size())
        return false;

    const Process &proc = procs.at(sourceRow);

    // ── Kernel task filter ───────────────────────────────────────────────────
    if (!this->ShowKernelTasks && isKernelTask(proc))
        return false;

    // ── Other-users filter ───────────────────────────────────────────────────
    if (!this->ShowOtherUsersProcs && proc.UID != this->m_myUid)
        return false;

    // ── Free-text search (delegate to base class) ────────────────────────────
    return QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent);
}

// static
bool ProcessFilterProxy::isKernelTask(const Process &proc)
{
    // Rely solely on the PF_KTHREAD flag read from /proc/pid/stat (same as
    // htop). PID 1 (systemd/init) is NOT a kernel thread and must NOT be
    // hidden. PID 2 (kthreadd) has PF_KTHREAD set so it is covered naturally.
    return proc.IsKernelThread;
}
