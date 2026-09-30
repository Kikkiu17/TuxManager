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

#include "processtreemodel.h"
#include "../misc.h"

#include <QSet>
#include <algorithm>

namespace
{
    QString extractAppCGroup(const QString &cgroup)
    {
        if (cgroup.isEmpty())
            return QString();

        int appSliceIdx = cgroup.indexOf("app.slice/");
        if (appSliceIdx < 0)
            return QString();

        QString unit = cgroup.mid(appSliceIdx + 10);
        int slashIdx = unit.indexOf('/');
        if (slashIdx >= 0)
            unit = unit.left(slashIdx);

        if (!unit.startsWith("app-"))
            return QString();

        if (unit.startsWith("app-flatpak-"))
        {
            int lastDash = unit.lastIndexOf('-');
            if (lastDash > 12)
                return unit.left(lastDash);
        }

        int atIdx = unit.indexOf('@');
        if (atIdx >= 0)
            return unit.left(atIdx);

        int scopeIdx = unit.indexOf(".scope");
        if (scopeIdx >= 0)
        {
            int lastDash = unit.lastIndexOf('-');
            if (lastDash > 4)
                return unit.left(lastDash);
        }

        return unit;
    }

    QString friendlyAppName(const QString &cgroupId, const QString &fallbackComm)
    {
        if (cgroupId.isEmpty())
            return fallbackComm;

        if (cgroupId.contains("waterfox", Qt::CaseInsensitive))
            return QStringLiteral("Waterfox");
        if (cgroupId.contains("steam", Qt::CaseInsensitive))
            return QStringLiteral("Steam");
        if (cgroupId.contains("thunderbird", Qt::CaseInsensitive))
            return QStringLiteral("Thunderbird");
        if (cgroupId.contains("spotify", Qt::CaseInsensitive))
            return QStringLiteral("Spotify");
        if (cgroupId.contains("bambustudio", Qt::CaseInsensitive))
            return QStringLiteral("Bambu Studio");
        if (cgroupId.contains("konsole", Qt::CaseInsensitive))
            return QStringLiteral("Konsole");
        if (cgroupId.contains("chrome", Qt::CaseInsensitive))
            return QStringLiteral("Google Chrome");
        if (cgroupId.contains("chromium", Qt::CaseInsensitive))
            return QStringLiteral("Chromium");
        if (cgroupId.contains("firefox", Qt::CaseInsensitive))
            return QStringLiteral("Firefox");

        if (cgroupId.startsWith("app-flatpak-"))
        {
            QString id = cgroupId.mid(12);
            int lastDot = id.lastIndexOf('.');
            if (lastDot >= 0 && lastDot < id.length() - 1)
                return id.mid(lastDot + 1);
            return id;
        }

        if (cgroupId.startsWith("app-"))
        {
            QString id = cgroupId.mid(4);
            int lastDot = id.lastIndexOf('.');
            if (lastDot >= 0 && lastDot < id.length() - 1)
                return id.mid(lastDot + 1);
            return id;
        }

        return fallbackComm;
    }

    bool isKnownWorkerProcess(const OS::Process &proc)
    {
        static const QSet<QString> workerComms = {
            QStringLiteral("forkserver"),
            QStringLiteral("socket process"),
            QStringLiteral("privileged cont"),
            QStringLiteral("rdd process"),
            QStringLiteral("webextensions"),
            QStringLiteral("utility process"),
            QStringLiteral("isolated web co"),
            QStringLiteral("isolated servic"),
            QStringLiteral("gmplugin"),
            QStringLiteral("steamwebhelper"),
            QStringLiteral("pv-adverb"),
            QStringLiteral("bwrap"),
            QStringLiteral("xdg-dbus-proxy"),
            QStringLiteral("webkitwebprocess"),
            QStringLiteral("webkitnetworkprocess")
        };

        if (workerComms.contains(proc.Name.toLower()))
            return true;

        if (proc.CmdLine.contains("-contentproc") || proc.CmdLine.contains("-parentPid"))
            return true;

        if (proc.CmdLine.contains("--type=zygote") ||
            proc.CmdLine.contains("--type=renderer") ||
            proc.CmdLine.contains("--type=gpu-process") ||
            proc.CmdLine.contains("--type=utility") ||
            proc.CmdLine.contains("--type=crashpad-handler"))
        {
            return true;
        }

        return false;
    }

    pid_t extractParentPidArg(const QString &cmdLine)
    {
        int idx = cmdLine.indexOf("-parentPid ");
        if (idx < 0)
            return 0;
        int start = idx + 11;
        int end = cmdLine.indexOf(' ', start);
        QString pidStr = (end >= 0) ? cmdLine.mid(start, end - start) : cmdLine.mid(start);
        bool ok = false;
        pid_t p = static_cast<pid_t>(pidStr.toInt(&ok));
        return ok ? p : 0;
    }

    bool isWrapperProcess(const QString &comm)
    {
        static const QSet<QString> wrappers = {
            QStringLiteral("bwrap"),
            QStringLiteral("xdg-dbus-proxy"),
            QStringLiteral("flatpak-spawn"),
            QStringLiteral("pv-adverb"),
            QStringLiteral("srt-logger"),
            QStringLiteral("steam-runtime-l"),
            QStringLiteral("bash"),
            QStringLiteral("sh")
        };
        return wrappers.contains(comm);
    }

    bool isLauncherOrInfra(pid_t pid, const QString &comm)
    {
        if (pid <= 1)
            return true;

        static const QSet<QString> infraComms = {
            QStringLiteral("systemd"), QStringLiteral("init"),
            QStringLiteral("gnome-shell"), QStringLiteral("plasmashell"),
            QStringLiteral("kwin_wayland"), QStringLiteral("kwin_x11"),
            QStringLiteral("sway"), QStringLiteral("hyprland"), QStringLiteral("wayfire"),
            QStringLiteral("mutter"), QStringLiteral("xfwm4"), QStringLiteral("openbox"),
            QStringLiteral("i3"), QStringLiteral("bspwm"), QStringLiteral("xorg"),
            QStringLiteral("xwayland"), QStringLiteral("gnome-session-binary"),
            QStringLiteral("ksmserver"), QStringLiteral("xfce4-session"),
            QStringLiteral("dbus-broker"), QStringLiteral("dbus-daemon"),
            QStringLiteral("systemd-journald"), QStringLiteral("pipewire"),
            QStringLiteral("wireplumber"), QStringLiteral("xdg-desktop-portal"),
            QStringLiteral("xdg-document-portal")
        };
        return infraComms.contains(comm.toLower());
    }
}

using namespace OS;

ProcessTreeModel::ProcessTreeModel(QObject *parent) : QAbstractItemModel(parent), m_root(new Node())
{
}

ProcessTreeModel::~ProcessTreeModel()
{
    freeNode(this->m_root);
}

QModelIndex ProcessTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColCount)
        return {};

    Node *parentNode = nodeFromIndex(parent);
    if (!parentNode)
        parentNode = this->m_root;
    if (row >= parentNode->children.size())
        return {};

    return createIndex(row, column, parentNode->children.at(row));
}

QModelIndex ProcessTreeModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};

    Node *node = nodeFromIndex(child);
    if (!node || !node->parent || node->parent == this->m_root)
        return {};

    Node *parentNode = node->parent;
    Node *grandParent = parentNode->parent ? parentNode->parent : this->m_root;
    const int row = grandParent->children.indexOf(parentNode);
    if (row < 0)
        return {};
    return createIndex(row, 0, parentNode);
}

int ProcessTreeModel::rowCount(const QModelIndex &parent) const
{
    Node *parentNode = nodeFromIndex(parent);
    if (!parentNode)
        parentNode = this->m_root;
    return parentNode->children.size();
}

int ProcessTreeModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return ColCount;
}

QVariant ProcessTreeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.column() < 0 || index.column() >= ColCount)
        return {};

    Node *node = nodeFromIndex(index);
    if (!node)
        return {};
    const Process &proc = node->process;

    if (role == Qt::DisplayRole)
    {
        switch (static_cast<Column>(index.column()))
        {
            case ColPid:      return proc.PID;
            case ColName:     return proc.Name;
            case ColUser:     return proc.User;
            case ColState:    return Process::GetStateString(proc.State);
            case ColCpu:      return QString::number(proc.CPUPercent, 'f', 1) + " %";
            case ColMemRss:   return Misc::FormatKiB(proc.VMRssKb, 0);
            case ColMemVirt:  return Misc::FormatKiB(proc.vmSizeKb, 0);
            case ColMemShared:return Misc::FormatKiB(proc.SharedKb, 0);
            case ColMemText:  return Misc::FormatKiB(proc.TextKb, 0);
            case ColMemData:  return Misc::FormatKiB(proc.DataKb, 0);
            case ColIoReads:  return proc.IOTotalsAvailable ? Misc::FormatBytes(proc.IOReadBytes, 0) : QString("?");
            case ColIoWrites: return proc.IOTotalsAvailable ? Misc::FormatBytes(proc.IOWriteBytes, 0) : QString("?");
            case ColIoReadsPerSec:
                if (proc.IOPermissionDenied || !proc.IOTotalsAvailable)
                    return QString("?");
                return proc.IORatesAvailable ? Misc::FormatBytesPerSecond(proc.IOReadBps) : tr("measuring...");
            case ColIoWritesPerSec:
                if (proc.IOPermissionDenied || !proc.IOTotalsAvailable)
                    return QString("?");
                return proc.IORatesAvailable ? Misc::FormatBytesPerSecond(proc.IOWriteBps) : tr("measuring...");
            case ColThreads:  return proc.Threads;
            case ColPriority: return proc.Priority;
            case ColNice:     return proc.Nice;
            case ColCmdline:  return proc.CmdLine;
            default: break;
        }
    }

    if (role == Qt::UserRole)
    {
        switch (static_cast<Column>(index.column()))
        {
            case ColPid:      return static_cast<qlonglong>(proc.PID);
            case ColCpu:      return proc.CPUPercent;
            case ColMemRss:   return static_cast<qulonglong>(proc.VMRssKb);
            case ColMemVirt:  return static_cast<qulonglong>(proc.vmSizeKb);
            case ColMemShared:return static_cast<qulonglong>(proc.SharedKb);
            case ColMemText:  return static_cast<qulonglong>(proc.TextKb);
            case ColMemData:  return static_cast<qulonglong>(proc.DataKb);
            case ColIoReads:
                return proc.IOTotalsAvailable
                       ? QVariant::fromValue(static_cast<qlonglong>(proc.IOReadBytes))
                       : QVariant::fromValue(static_cast<qlonglong>(-1));
            case ColIoWrites:
                return proc.IOTotalsAvailable
                       ? QVariant::fromValue(static_cast<qlonglong>(proc.IOWriteBytes))
                       : QVariant::fromValue(static_cast<qlonglong>(-1));
            case ColIoReadsPerSec:
                return proc.IORatesAvailable
                       ? QVariant::fromValue(proc.IOReadBps)
                       : QVariant::fromValue(-1.0);
            case ColIoWritesPerSec:
                return proc.IORatesAvailable
                       ? QVariant::fromValue(proc.IOWriteBps)
                       : QVariant::fromValue(-1.0);
            case ColThreads:  return proc.Threads;
            case ColPriority: return proc.Priority;
            case ColNice:     return proc.Nice;
            default:          return data(index, Qt::DisplayRole);
        }
    }

    if (role == Qt::TextAlignmentRole)
    {
        switch (static_cast<Column>(index.column()))
        {
            case ColCpu:
            case ColMemRss:
            case ColMemVirt:
            case ColMemShared:
            case ColMemText:
            case ColMemData:
            case ColIoReads:
            case ColIoWrites:
            case ColIoReadsPerSec:
            case ColIoWritesPerSec:
            case ColThreads:
            case ColPriority:
            case ColNice:
                return QVariant(Qt::AlignRight | Qt::AlignVCenter);
            default:
                return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }

    return {};
}

QVariant ProcessTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    if (section < 0 || section >= ColCount)
        return {};
    return columnHeader(static_cast<Column>(section));
}

Qt::ItemFlags ProcessTreeModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void ProcessTreeModel::SetMode(Mode mode)
{
    if (this->m_mode == mode)
        return;
    this->m_mode = mode;
}

void ProcessTreeModel::SetProcesses(const QList<Process> &processes)
{
    beginResetModel();
    freeNode(this->m_root);
    this->m_root = new Node();
    this->m_byPid.clear();
    this->m_groupHeaders.clear();

    if (this->m_mode == Mode::ProcessHierarchy)
    {
        this->buildHierarchyTree(processes);
    }
    else
    {
        this->buildGroupedTree(processes);
    }

    endResetModel();
}

void ProcessTreeModel::buildHierarchyTree(const QList<Process> &processes)
{
    QList<Node *> nodes;
    nodes.reserve(processes.size());
    for (const Process &proc : processes)
    {
        Node *node = new Node();
        node->process = proc;
        this->m_byPid.insert(proc.PID, node);
        nodes.append(node);
    }

    for (Node *node : nodes)
    {
        Node *parent = this->m_root;
        const pid_t ppid = node->process.PPID;
        if (ppid > 0 && ppid != node->process.PID && this->m_byPid.contains(ppid))
            parent = this->m_byPid.value(ppid);

        node->parent = parent;
        parent->children.append(node);
    }
}

void ProcessTreeModel::buildGroupedTree(const QList<Process> &processes)
{
    // Fast lookup by PID
    QHash<pid_t, const Process *> procByPid;
    procByPid.reserve(processes.size());
    for (const Process &p : processes)
        procByPid.insert(p.PID, &p);

    // 1. Group by cgroup app
    QHash<QString, QList<pid_t>> cgroupApps;
    QHash<pid_t, QString> pidToCGroupApp;
    for (const Process &p : processes)
    {
        QString cgroupApp = extractAppCGroup(p.CGroup);
        if (!cgroupApp.isEmpty())
        {
            cgroupApps[cgroupApp].append(p.PID);
            pidToCGroupApp.insert(p.PID, cgroupApp);
        }
    }

    QHash<int, QList<pid_t>> groups; // group id -> pids
    QHash<pid_t, int> pidToGroup;
    int nextGroupId = 1;

    for (auto it = cgroupApps.cbegin(); it != cgroupApps.cend(); ++it)
    {
        const QList<pid_t> &pids = it.value();
        if (pids.size() > 1)
        {
            int gid = nextGroupId++;
            groups.insert(gid, pids);
            for (pid_t pid : pids)
                pidToGroup.insert(pid, gid);
        }
    }

    // 2. For remaining processes, check -parentPid arg or worker ancestry
    for (const Process &p : processes)
    {
        if (pidToGroup.contains(p.PID))
            continue;

        // Check -parentPid argument (common in Firefox / Waterfox / Thunderbird)
        pid_t parentPidArg = extractParentPidArg(p.CmdLine);
        if (parentPidArg > 0 && parentPidArg != p.PID && procByPid.contains(parentPidArg))
        {
            int targetGid = pidToGroup.value(parentPidArg, 0);
            if (targetGid == 0)
            {
                targetGid = nextGroupId++;
                groups[targetGid].append(parentPidArg);
                pidToGroup.insert(parentPidArg, targetGid);
            }
            groups[targetGid].append(p.PID);
            pidToGroup.insert(p.PID, targetGid);
            continue;
        }

        // Check if this is a worker process
        if (isKnownWorkerProcess(p))
        {
            pid_t curr = p.PPID;
            while (curr > 1 && procByPid.contains(curr))
            {
                if (pidToGroup.contains(curr))
                {
                    int targetGid = pidToGroup.value(curr);
                    groups[targetGid].append(p.PID);
                    pidToGroup.insert(p.PID, targetGid);
                    break;
                }
                const Process *parentProc = procByPid.value(curr);
                if (!isKnownWorkerProcess(*parentProc) && !isLauncherOrInfra(curr, parentProc->Name))
                {
                    int targetGid = nextGroupId++;
                    groups[targetGid].append(curr);
                    pidToGroup.insert(curr, targetGid);
                    groups[targetGid].append(p.PID);
                    pidToGroup.insert(p.PID, targetGid);
                    break;
                }
                curr = parentProc->PPID;
            }
            if (pidToGroup.contains(p.PID))
                continue;
        }

        // Check if child shares the same binary/comm with parent (e.g. Chrome workers, python, make)
        if (p.PPID > 1 && procByPid.contains(p.PPID))
        {
            const Process *parentProc = procByPid.value(p.PPID);
            if (parentProc->Name == p.Name && !isLauncherOrInfra(p.PPID, parentProc->Name))
            {
                int targetGid = pidToGroup.value(p.PPID, 0);
                if (targetGid == 0)
                {
                    targetGid = nextGroupId++;
                    groups[targetGid].append(p.PPID);
                    pidToGroup.insert(p.PPID, targetGid);
                }
                groups[targetGid].append(p.PID);
                pidToGroup.insert(p.PID, targetGid);
                continue;
            }
        }
    }

    // 3. For any processes not in a multi-process group, they are standalone top-level items
    for (const Process &p : processes)
    {
        if (!pidToGroup.contains(p.PID))
        {
            Node *node = new Node();
            node->process = p;
            node->isGroup = false;
            node->processCount = 1;
            node->parent = this->m_root;
            this->m_root->children.append(node);
            this->m_byPid.insert(p.PID, node);
        }
    }

    // 4. Build Application Group nodes
    for (auto it = groups.cbegin(); it != groups.cend(); ++it)
    {
        QList<pid_t> memberPids = it.value();
        std::sort(memberPids.begin(), memberPids.end());
        memberPids.erase(std::unique(memberPids.begin(), memberPids.end()), memberPids.end());

        if (memberPids.isEmpty())
            continue;

        if (memberPids.size() == 1)
        {
            pid_t pid = memberPids.first();
            if (procByPid.contains(pid))
            {
                Node *node = new Node();
                node->process = *procByPid.value(pid);
                node->isGroup = false;
                node->processCount = 1;
                node->parent = this->m_root;
                this->m_root->children.append(node);
                this->m_byPid.insert(pid, node);
            }
            continue;
        }

        // Multi-process application!
        // Find Primary process
        QSet<pid_t> memberSet(memberPids.begin(), memberPids.end());
        QList<pid_t> nonWrappers;
        for (pid_t pid : memberPids)
        {
            if (procByPid.contains(pid))
            {
                if (!isWrapperProcess(procByPid.value(pid)->Name))
                    nonWrappers.append(pid);
            }
        }
        const QList<pid_t> &candidateList = nonWrappers.isEmpty() ? memberPids : nonWrappers;

        pid_t primaryPid = candidateList.first();
        for (pid_t pid : candidateList)
        {
            if (procByPid.contains(pid))
            {
                if (!memberSet.contains(procByPid.value(pid)->PPID))
                {
                    primaryPid = pid;
                    break;
                }
            }
        }

        const Process &primaryProc = *procByPid.value(primaryPid, procByPid.value(memberPids.first()));
        QString cgroupId = pidToCGroupApp.value(primaryPid, QString());
        if (cgroupId.isEmpty())
        {
            for (pid_t pid : memberPids)
            {
                if (pidToCGroupApp.contains(pid))
                {
                    cgroupId = pidToCGroupApp.value(pid);
                    break;
                }
            }
        }

        QString friendlyName = friendlyAppName(cgroupId, primaryProc.Name);

        Node *groupNode = new Node();
        groupNode->isGroup = true;
        groupNode->processCount = memberPids.size();
        groupNode->parent = this->m_root;
        this->m_root->children.append(groupNode);

        // Populate groupNode->process with primary metadata and aggregated stats
        groupNode->process = primaryProc;
        groupNode->process.Name = QString("%1 (%2)").arg(friendlyName).arg(memberPids.size());

        // Zero out aggregate fields before accumulating
        groupNode->process.CPUPercent = 0.0;
        groupNode->process.VMRssKb = 0;
        groupNode->process.vmSizeKb = 0;
        groupNode->process.SharedKb = 0;
        groupNode->process.TextKb = 0;
        groupNode->process.DataKb = 0;
        groupNode->process.Threads = 0;
        groupNode->process.IOReadBytes = 0;
        groupNode->process.IOWriteBytes = 0;
        groupNode->process.IOReadBps = 0.0;
        groupNode->process.IOWriteBps = 0.0;

        bool anyRatesAvailable = false;
        bool anyTotalsAvailable = false;
        bool allPermissionDenied = true;
        int validProcCount = 0;

        for (pid_t pid : memberPids)
        {
            if (!procByPid.contains(pid))
                continue;
            const Process &p = *procByPid.value(pid);
            validProcCount++;
            if (p.IORatesAvailable)
                anyRatesAvailable = true;
            if (p.IOTotalsAvailable)
                anyTotalsAvailable = true;
            if (!p.IOPermissionDenied)
                allPermissionDenied = false;

            groupNode->process.CPUPercent += p.CPUPercent;
            groupNode->process.VMRssKb += p.VMRssKb;
            groupNode->process.vmSizeKb += p.vmSizeKb;
            groupNode->process.SharedKb += p.SharedKb;
            groupNode->process.TextKb += p.TextKb;
            groupNode->process.DataKb += p.DataKb;
            groupNode->process.Threads += p.Threads;
            groupNode->process.IOReadBytes += p.IOReadBytes;
            groupNode->process.IOWriteBytes += p.IOWriteBytes;
            groupNode->process.IOReadBps += p.IOReadBps;
            groupNode->process.IOWriteBps += p.IOWriteBps;
        }

        groupNode->process.IORatesAvailable = anyRatesAvailable;
        groupNode->process.IOTotalsAvailable = anyTotalsAvailable;
        groupNode->process.IOPermissionDenied = allPermissionDenied && (validProcCount > 0);

        // Add child nodes: primary process first, then remaining sorted by CPU% descending
        QList<pid_t> orderedChildren;
        orderedChildren.append(primaryPid);
        QList<pid_t> otherChildren;
        for (pid_t pid : memberPids)
        {
            if (pid != primaryPid)
                otherChildren.append(pid);
        }
        std::sort(otherChildren.begin(), otherChildren.end(), [&procByPid](pid_t a, pid_t b) {
            double cpuA = procByPid.contains(a) ? procByPid.value(a)->CPUPercent : 0.0;
            double cpuB = procByPid.contains(b) ? procByPid.value(b)->CPUPercent : 0.0;
            return cpuA > cpuB;
        });
        orderedChildren.append(otherChildren);

        for (pid_t pid : orderedChildren)
        {
            if (!procByPid.contains(pid))
                continue;
            Node *childNode = new Node();
            childNode->process = *procByPid.value(pid);
            childNode->isGroup = false;
            childNode->processCount = 1;
            childNode->parent = groupNode;
            groupNode->children.append(childNode);
            this->m_byPid.insert(pid, childNode);
        }

        this->m_groupHeaders.insert(primaryProc.PID, groupNode);
    }
}

QModelIndex ProcessTreeModel::IndexForPid(pid_t pid) const
{
    Node *node = this->m_groupHeaders.value(pid, nullptr);
    if (!node)
        node = this->m_byPid.value(pid, nullptr);
    if (!node || !node->parent)
        return {};
    const int row = node->parent->children.indexOf(node);
    if (row < 0)
        return {};
    return createIndex(row, 0, node);
}

QList<pid_t> ProcessTreeModel::PidsForIndex(const QModelIndex &index) const
{
    Node *node = nodeFromIndex(index);
    if (!node)
        return {};

    QList<pid_t> result;
    if (node->isGroup)
    {
        for (Node *child : node->children)
            result.append(child->process.PID);
    }
    else
    {
        result.append(node->process.PID);
    }
    return result;
}

bool ProcessTreeModel::IsGroupIndex(const QModelIndex &index) const
{
    Node *node = nodeFromIndex(index);
    return node && node->isGroup;
}

QString ProcessTreeModel::columnHeader(Column col)
{
    switch (col)
    {
        case ColPid:      return tr("PID");
        case ColName:     return tr("Name");
        case ColUser:     return tr("User");
        case ColState:    return tr("State");
        case ColCpu:      return tr("CPU %");
        case ColMemRss:   return tr("MEM RES");
        case ColMemVirt:  return tr("MEM VIRT");
        case ColMemShared:return tr("MEM SHR");
        case ColMemText:  return tr("MEM TEXT");
        case ColMemData:  return tr("MEM DATA");
        case ColIoReads:  return tr("IO Reads");
        case ColIoWrites: return tr("IO Writes");
        case ColIoReadsPerSec: return tr("IO Read/s");
        case ColIoWritesPerSec:return tr("IO Write/s");
        case ColThreads:  return tr("Threads");
        case ColPriority: return tr("Priority");
        case ColNice:     return tr("Nice");
        case ColCmdline:  return tr("Command");
        default:          return {};
    }
}

void ProcessTreeModel::freeNode(Node *node)
{
    if (!node)
        return;
    for (Node *child : node->children)
        freeNode(child);
    delete node;
}

ProcessTreeModel::Node *ProcessTreeModel::nodeFromIndex(const QModelIndex &index)
{
    if (!index.isValid())
        return nullptr;
    return static_cast<Node *>(index.internalPointer());
}
