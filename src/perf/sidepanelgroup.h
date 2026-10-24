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

#ifndef PERF_SIDEPANELGROUP_H
#define PERF_SIDEPANELGROUP_H

#include <QList>
#include <QString>

#include <optional>

namespace Perf
{
    enum class SidePanelGroup
    {
        Cpu,
        Memory,
        Swap,
        Disks,
        Network,
        Gpu,
        Npu,
        Battery
    };

    QString SidePanelGroupId(SidePanelGroup group);
    QString SidePanelGroupLabel(SidePanelGroup group);
    std::optional<SidePanelGroup> SidePanelGroupFromId(const QString &id);
    QList<SidePanelGroup> DefaultSidePanelGroupOrder();
}

#endif // PERF_SIDEPANELGROUP_H
