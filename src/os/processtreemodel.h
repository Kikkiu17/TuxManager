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

#ifndef OS_PROCESSTREEMODEL_H
#define OS_PROCESSTREEMODEL_H

#include "process.h"

#include <QAbstractItemModel>
#include <QHash>

namespace OS
{
    class ProcessTreeModel : public QAbstractItemModel
    {
        Q_OBJECT

        public:
            enum Column
            {
                ColPid = 0,
                ColName,
                ColUser,
                ColState,
                ColCpu,
                ColMemRss,
                ColMemVirt,
                ColMemShared,
                ColMemText,
                ColMemData,
                ColIoReads,
                ColIoWrites,
                ColIoReadsPerSec,
                ColIoWritesPerSec,
                ColThreads,
                ColPriority,
                ColNice,
                ColCmdline,
                ColCount
            };

            enum class Mode
            {
                ApplicationGrouped = 0,
                ProcessHierarchy = 1
            };

            explicit ProcessTreeModel(QObject *parent = nullptr);
            ~ProcessTreeModel() override;

            QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
            QModelIndex parent(const QModelIndex &child) const override;
            int rowCount(const QModelIndex &parent = QModelIndex()) const override;
            int columnCount(const QModelIndex &parent = QModelIndex()) const override;
            QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
            QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
            Qt::ItemFlags flags(const QModelIndex &index) const override;

            void SetMode(Mode mode);
            Mode GetMode() const { return this->m_mode; }
            void SetProcesses(const QList<Process> &processes);
            QModelIndex IndexForPid(pid_t pid) const;
            QList<pid_t> PidsForIndex(const QModelIndex &index) const;
            bool IsGroupIndex(const QModelIndex &index) const;

        private:
            struct Node
            {
                Process process;
                bool isGroup { false };
                int processCount { 1 };
                Node *parent { nullptr };
                QList<Node *> children;
            };

            Mode m_mode { Mode::ApplicationGrouped };
            Node *m_root { nullptr };
            QHash<pid_t, Node *> m_byPid;
            QHash<pid_t, Node *> m_groupHeaders;

            void buildGroupedTree(const QList<Process> &processes);
            void buildHierarchyTree(const QList<Process> &processes);

            static QString columnHeader(Column col);
            static void freeNode(Node *node);
            static Node *nodeFromIndex(const QModelIndex &index);
    };
}

#endif // OS_PROCESSTREEMODEL_H
