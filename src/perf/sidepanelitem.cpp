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

#include "sidepanelitem.h"
#include "../configuration.h"
#include "../colorscheme.h"
#include "globals.h"

#include <QPainter>
#include <QPaintEvent>
#include <QHBoxLayout>
#include <QStringList>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QEnterEvent>
#endif

using namespace Perf;

SidePanelItem::SidePanelItem(const QString &title, QWidget *parent) : QWidget(parent), m_title(title), m_graph(new GraphWidget(this))
{
    this->setCursor(Qt::PointingHandCursor);
    this->setMouseTracking(true);

    // Windows 11 Task Manager style: mini thumbnail graph on the left,
    // title and subtitle lines stacked to the right.
    QHBoxLayout *lay = new QHBoxLayout(this);
    lay->setContentsMargins(10, 10, 8, 10);
    lay->setSpacing(10);
    this->m_graph->SetSampleCapacity(TUX_MANAGER_HISTORY_SIZE);
    this->m_graph->SetGridEnabled(CFG->SidePanelGridEnabled);
    this->m_graph->SetHoverLineEnabled(false);
    this->m_graph->SetHoverTooltipEnabled(false);
    this->m_graph->setFixedSize(68, 46);
    lay->addWidget(this->m_graph, 0, Qt::AlignVCenter);
    lay->addStretch(1);
    this->setLayout(lay);
}

void SidePanelItem::SetGraphSource(const HistoryBuffer &history, double maxVal)
{
    this->m_graph->SetDataSource(history, maxVal);
}

void SidePanelItem::Update(const QString &subtitle, double maxVal)
{
    this->m_subtitle = subtitle;
    this->m_graph->SetMax(maxVal);
    this->m_graph->Tick();
    this->update();
}

void SidePanelItem::SetSelected(bool selected)
{
    if (this->m_selected == selected)
        return;
    this->m_selected = selected;
    this->update();
}

void SidePanelItem::SetGraphColor(QColor line, QColor fill)
{
    this->m_graph->SetColor(line, fill);
}

void SidePanelItem::SetGraphGridEnabled(bool enabled)
{
    this->m_graph->SetGridEnabled(enabled);
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void SidePanelItem::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);   // draw children (the graph widget)

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const ColorScheme *scheme = ColorScheme::GetCurrent();

    const QRect r = this->rect();
    const QRectF cardRect = QRectF(r).adjusted(2.0, 1.0, -2.0, -1.0);

    // Rounded card background for selected / hovered state (Windows 11 style)
    if (this->m_selected)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(scheme->SidePanelItemSelectedBackgroundColor);
        p.drawRoundedRect(cardRect, 5.0, 5.0);
    } else if (this->m_hovered)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(scheme->SidePanelItemHoverBackgroundColor);
        p.drawRoundedRect(cardRect, 5.0, 5.0);
    } else if (scheme->SidePanelItemBackgroundColor.alpha() > 0)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(scheme->SidePanelItemBackgroundColor);
        p.drawRoundedRect(cardRect, 5.0, 5.0);
    }

    // Text block to the right of the mini graph
    QFont titleFont = this->font();
    titleFont.setBold(false);
    titleFont.setPointSize(10);
    const QFontMetrics titleFm(titleFont);

    QFont subFont = this->font();
    subFont.setBold(false);
    subFont.setPointSize(8);
    const QFontMetrics subFm(subFont);

    const int textLeft = this->m_graph->geometry().right() + 11;
    const int textRightMargin = 8;
    const int availW = qMax(0, r.width() - textLeft - textRightMargin);

    const QStringList subLines = this->m_subtitle.split('\n', Qt::SkipEmptyParts);
    const int titleH = titleFm.height();
    const int subLineH = subFm.height();
    const int lineGap = 1;
    const int totalTextH = titleH + (subLines.isEmpty() ? 0 : (2 + subLines.size() * subLineH + (subLines.size() - 1) * lineGap));
    int curY = qMax(6, (r.height() - totalTextH) / 2);

    const QString titleText = titleFm.elidedText(this->m_title, Qt::ElideRight, availW);
    p.setFont(titleFont);
    p.setPen(this->m_selected ? scheme->SidePanelItemSelectedTextColor
                              : scheme->SidePanelItemTextColor);
    p.drawText(QRect(textLeft, curY, availW, titleH), Qt::AlignLeft | Qt::AlignVCenter, titleText);
    curY += titleH + 2;

    if (!subLines.isEmpty())
    {
        p.setFont(subFont);
        p.setPen(scheme->SidePanelItemSubtitleColor);
        for (const QString &rawLine : subLines)
        {
            const QString lineText = subFm.elidedText(rawLine, Qt::ElideRight, availW);
            p.drawText(QRect(textLeft, curY, availW, subLineH), Qt::AlignLeft | Qt::AlignVCenter, lineText);
            curY += subLineH + lineGap;
        }
    }
}

// ── Events ────────────────────────────────────────────────────────────────────

void SidePanelItem::mousePressEvent(QMouseEvent *event)
{
    QWidget::mousePressEvent(event);
    if (event->button() == Qt::LeftButton)
        emit this->clicked();
    else if (event->button() == Qt::RightButton)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        emit this->contextMenuRequested(event->globalPosition().toPoint());
#else
        emit this->contextMenuRequested(event->globalPos());
#endif
    }
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void SidePanelItem::enterEvent(QEnterEvent *event)
#else
void SidePanelItem::enterEvent(QEvent *event)
#endif
{
    QWidget::enterEvent(event);
    this->m_hovered = true;
    this->update();
}

void SidePanelItem::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    this->m_hovered = false;
    this->update();
}
