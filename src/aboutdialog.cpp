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

#include "aboutdialog.h"
#include "configuration.h"
#include "globals.h"
#include "ui_aboutdialog.h"
#include "updater.h"

#include <QCheckBox>
#include <QIcon>
#include <QLayout>
#include <QPushButton>
#include <QStyle>

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent), ui(new Ui::AboutDialog)
{
    this->ui->setupUi(this);
    this->ui->labelTitle->setText(QString::fromLatin1(TUX_MANAGER_PRODUCT_NAME));
    this->ui->labelVersion->setText(tr("Version %1").arg(QString::fromLatin1(TUX_MANAGER_VERSION_STRING)));

    QString infoText = this->ui->labelInfo->text();
    const QString oldLink = QStringLiteral("<a href=\"https://github.com/benapetr/TuxManager\">https://github.com/benapetr/TuxManager</a>");
    const QString newLinks = QStringLiteral("<a href=\"%1\">%1</a> (<a href=\"%2\">%3</a>)")
                                 .arg(QString::fromLatin1(TUX_MANAGER_HOMEPAGE_URL),
                                      QString::fromLatin1(TUX_MANAGER_RELEASES_URL),
                                      tr("Releases"));
    if (infoText.contains(oldLink))
    {
        infoText.replace(oldLink, newLinks);
    } else
    {
        infoText.replace(QStringLiteral("https://github.com/benapetr/TuxManager"),
                         QString::fromLatin1(TUX_MANAGER_HOMEPAGE_URL));
    }
    this->ui->labelInfo->setText(infoText);

    if (this->ui->closeButton->icon().isNull())
    {
        this->ui->closeButton->setIcon(
            QIcon::fromTheme(QStringLiteral("window-close"),
                             this->style()->standardIcon(QStyle::SP_DialogCloseButton)));
    }

    this->ui->autoCheckUpdatesCheck->setChecked(CFG->AutoCheckUpdates);
    connect(this->ui->autoCheckUpdatesCheck, &QCheckBox::toggled, this, [](bool checked)
    {
        CFG->AutoCheckUpdates = checked;
        CFG->Save();
    });

    if (Updater::instance()->IsCheckInProgress())
    {
        this->ui->checkUpdatesButton->setEnabled(false);
        this->ui->checkUpdatesButton->setText(tr("Checking..."));
    }

    connect(this->ui->checkUpdatesButton, &QPushButton::clicked, this, &AboutDialog::checkForUpdates);
    connect(Updater::instance(), &Updater::CheckFinished, this, &AboutDialog::onUpdateCheckFinished);

    if (this->layout())
        this->layout()->activate();
    this->setFixedSize(this->sizeHint().expandedTo(QSize(520, 420)));
}

AboutDialog::~AboutDialog()
{
    delete this->ui;
}

void AboutDialog::on_closeButton_clicked()
{
    this->close();
}

void AboutDialog::checkForUpdates()
{
    this->ui->checkUpdatesButton->setEnabled(false);
    this->ui->checkUpdatesButton->setText(tr("Checking..."));
    Updater::instance()->CheckForUpdates(true, this);
}

void AboutDialog::onUpdateCheckFinished(bool, const QString &, const QString &)
{
    this->ui->checkUpdatesButton->setEnabled(true);
    this->ui->checkUpdatesButton->setText(tr("Check for Updates..."));
}
