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

#include "updater.h"
#include "configuration.h"
#include "globals.h"
#include "logger.h"
#include "misc.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>

#include <algorithm>
#include <functional>
#include <sys/stat.h>
#include <sys/utsname.h>

Updater *Updater::s_instance = nullptr;

namespace
{
    constexpr qint64 kUpdateCheckIntervalSecs = 86400; // 24 hours
    constexpr int kCurlMetadataTimeoutMs = 15000;
    constexpr int kCurlDownloadTimeoutMs = 610000;

    bool isSystemPrefix(const QString &path)
    {
        return path.startsWith(QLatin1String("/nix/store/"))
            || path.startsWith(QLatin1String("/usr/"))
            || path.startsWith(QLatin1String("/opt/"))
            || path.startsWith(QLatin1String("/bin/"))
            || path.startsWith(QLatin1String("/sbin/"))
            || path.startsWith(QLatin1String("/lib/"))
            || path.startsWith(QLatin1String("/lib64/"));
    }

    struct ParsedVersion
    {
        QVector<quint64> Core;
        QStringList PreRelease;
        bool Valid { false };
    };

    ParsedVersion parseVersion(const QString &raw)
    {
        ParsedVersion parsed;
        QString v = Updater::NormalizeVersion(raw);
        if (v.isEmpty())
            return parsed;

        const int plusIdx = v.indexOf(QLatin1Char('+'));
        if (plusIdx >= 0)
            v = v.left(plusIdx);

        int preIdx = v.indexOf(QLatin1Char('-'));
        const int tildeIdx = v.indexOf(QLatin1Char('~'));
        if (tildeIdx >= 0 && (preIdx < 0 || tildeIdx < preIdx))
            preIdx = tildeIdx;

        const QString coreStr = (preIdx >= 0) ? v.left(preIdx) : v;
        const QString preStr = (preIdx >= 0) ? v.mid(preIdx + 1) : QString();
        if (coreStr.isEmpty())
            return parsed;

        const QStringList coreParts = coreStr.split(QLatin1Char('.'), Qt::KeepEmptyParts);
        parsed.Core.reserve(coreParts.size());
        for (const QString &part : coreParts)
        {
            if (part.isEmpty())
                return ParsedVersion{};
            bool ok = false;
            const quint64 val = part.toULongLong(&ok);
            if (!ok)
                return ParsedVersion{};
            parsed.Core.append(val);
        }

        if (!preStr.isEmpty())
            parsed.PreRelease = preStr.split(QLatin1Char('.'), Qt::SkipEmptyParts);

        parsed.Valid = !parsed.Core.isEmpty();
        return parsed;
    }

    int comparePreReleasePart(const QString &a, const QString &b)
    {
        bool aNumOk = false;
        bool bNumOk = false;
        const quint64 aNum = a.toULongLong(&aNumOk);
        const quint64 bNum = b.toULongLong(&bNumOk);

        if (aNumOk && bNumOk)
        {
            if (aNum < bNum)
                return -1;
            if (aNum > bNum)
                return 1;
            return 0;
        }
        if (aNumOk != bNumOk)
            return aNumOk ? -1 : 1;

        const int cmp = QString::compare(a, b, Qt::CaseSensitive);
        return (cmp < 0) ? -1 : ((cmp > 0) ? 1 : 0);
    }

    QString formatCurlFailure(int exitCode, const QByteArray &stderrData, const QString &startError)
    {
        if (!startError.isEmpty())
            return QObject::tr("Failed to start curl: %1").arg(startError);

        const QString errText = QString::fromUtf8(stderrData).trimmed();
        if (!errText.isEmpty())
            return errText;
        return QObject::tr("Network request failed (curl exit code %1).").arg(exitCode);
    }

    QString guidanceTextForInstallKind(Updater::InstallKind kind, const QString &installPath)
    {
        switch (kind)
        {
            case Updater::InstallKind::WritableAppImage:
                return QObject::tr("TuxManager is running as a writable AppImage and can download, verify, and apply this update automatically.");
            case Updater::InstallKind::ReadOnlyAppImage:
                return QObject::tr("TuxManager is running from a read-only AppImage (%1) and cannot update itself in place. Download the latest AppImage from the Releases page and replace it manually.").arg(installPath);
            case Updater::InstallKind::Nix:
                return QObject::tr("TuxManager is managed by Nix (%1). Run 'nix flake update' and rebuild your environment to upgrade.").arg(installPath);
            case Updater::InstallKind::SystemPackage:
                return QObject::tr("TuxManager is installed as a system package (%1). Please update it via your distribution's package manager (apt, dnf, zypper, pacman) or download a package from the Releases page.").arg(installPath);
            case Updater::InstallKind::LocalBuild:
                return QObject::tr("TuxManager is running from a local build (%1). Pull the latest changes and rebuild, or download a release from GitHub.").arg(installPath);
        }
        return QString();
    }

    using CurlDoneCallback = std::function<void(int exitCode,
                                                QProcess::ExitStatus exitStatus,
                                                const QByteArray &stdoutData,
                                                const QByteArray &stderrData,
                                                const QString &startError)>;

    QProcess *runCurlCommand(QObject *owner,
                             const QString &curlPath,
                             const QStringList &args,
                             int timeoutMs,
                             CurlDoneCallback onComplete)
    {
        auto *proc = new QProcess(owner);
        auto *timeoutTimer = new QTimer(proc);
        timeoutTimer->setSingleShot(true);
        QObject::connect(timeoutTimer, &QTimer::timeout, proc, [proc]()
        {
            if (proc->state() != QProcess::NotRunning)
                proc->kill();
        });

        QObject::connect(proc, &QProcess::errorOccurred, owner, [proc, timeoutTimer, onComplete](QProcess::ProcessError err)
        {
            if (err != QProcess::FailedToStart)
                return;
            timeoutTimer->stop();
            const QString errStr = proc->errorString();
            proc->deleteLater();
            onComplete(-1, QProcess::CrashExit, QByteArray(), QByteArray(), errStr);
        });

        QObject::connect(proc, &QProcess::finished, owner, [proc, timeoutTimer, onComplete](int exitCode, QProcess::ExitStatus exitStatus)
        {
            timeoutTimer->stop();
            const QByteArray stdoutData = proc->readAllStandardOutput();
            const QByteArray stderrData = proc->readAllStandardError();
            proc->deleteLater();
            onComplete(exitCode, exitStatus, stdoutData, stderrData, QString());
        });

        timeoutTimer->start(timeoutMs);
        proc->start(curlPath, args);
        return proc;
    }

    QStringList httpsCurlBaseArgs(const QStringList &extraArgs)
    {
        QStringList args = {
            QStringLiteral("--proto"), QStringLiteral("=https"),
            QStringLiteral("--proto-redir"), QStringLiteral("=https")
        };
        args.append(extraArgs);
        return args;
    }

    struct UpdateDialogWidgets
    {
        QPointer<QDialog> Dialog;
        QPushButton *SkipButton { nullptr };
        QPushButton *ReleasesButton { nullptr };
        QPushButton *UpdateButton { nullptr };
        QPushButton *CloseButton { nullptr };
        QCheckBox *AutoCheckBox { nullptr };
        QLabel *StatusLabel { nullptr };
        QProgressBar *ProgressBar { nullptr };
    };

    struct InstallState
    {
        bool Installing { false };
        bool Cancelled { false };
        QPointer<QProcess> ActiveProc;
        QString TempPath;
    };

    void abortInstallState(const QSharedPointer<InstallState> &state)
    {
        if (!state->Installing)
            return;
        state->Cancelled = true;
        if (state->ActiveProc && state->ActiveProc->state() != QProcess::NotRunning)
        {
            state->ActiveProc->kill();
            state->ActiveProc->waitForFinished(500);
        }
        if (!state->TempPath.isEmpty())
            QFile::remove(state->TempPath);
        state->Installing = false;
    }

    void restoreDialogAfterInstallEnd(const UpdateDialogWidgets &ui,
                                      const QSharedPointer<InstallState> &state,
                                      const QString &statusMessage,
                                      bool showWarningPopup)
    {
        if (!state->TempPath.isEmpty())
            QFile::remove(state->TempPath);
        state->Installing = false;
        state->ActiveProc = nullptr;
        if (!ui.Dialog)
            return;

        ui.SkipButton->setEnabled(true);
        ui.ReleasesButton->setEnabled(true);
        ui.AutoCheckBox->setEnabled(true);
        if (ui.UpdateButton)
            ui.UpdateButton->setEnabled(true);
        ui.CloseButton->setText(QObject::tr("Close"));
        ui.ProgressBar->setVisible(false);
        ui.StatusLabel->setText(statusMessage);
        if (showWarningPopup)
            QMessageBox::warning(ui.Dialog, QObject::tr("Update Failed"), statusMessage);
    }

    void startDownloadAndInstall(const UpdateDialogWidgets &ui,
                                 const Updater::ReleaseInfo &release,
                                 const QString &curlPath,
                                 const QString &appImagePath,
                                 const QString &tempPath,
                                 const QString &verifiedSha256,
                                 const QSharedPointer<InstallState> &state)
    {
        if (!ui.Dialog || state->Cancelled)
            return;

        if (verifiedSha256.isEmpty())
        {
            restoreDialogAfterInstallEnd(
                ui,
                state,
                QObject::tr("Checksum verification failed: %1.sha256 was not found on this release.\nPlease update manually from the Releases page.").arg(release.AssetName),
                true);
            return;
        }

        QFile::remove(tempPath);
        ui.StatusLabel->setText(QObject::tr("Downloading %1...").arg(release.AssetName));
        if (release.ExpectedSize > 0)
        {
            ui.ProgressBar->setRange(0, 100);
            ui.ProgressBar->setValue(0);
        } else
        {
            ui.ProgressBar->setRange(0, 0);
        }

        const QStringList dlArgs = httpsCurlBaseArgs({
            QStringLiteral("-fL"),
            QStringLiteral("-sS"),
            QStringLiteral("--connect-timeout"), QStringLiteral("15"),
            QStringLiteral("--max-time"), QStringLiteral("600"),
            QStringLiteral("-o"), tempPath,
            release.DownloadUrl
        });

        QProcess *dlProc = runCurlCommand(
            ui.Dialog,
            curlPath,
            dlArgs,
            kCurlDownloadTimeoutMs,
            [ui, release, appImagePath, tempPath, verifiedSha256, state](int exitCode,
                                                                          QProcess::ExitStatus exitStatus,
                                                                          const QByteArray &,
                                                                          const QByteArray &stderrData,
                                                                          const QString &startError)
            {
                state->ActiveProc = nullptr;
                if (state->Cancelled || !ui.Dialog)
                {
                    QFile::remove(tempPath);
                    return;
                }

                if (!startError.isEmpty() || exitStatus != QProcess::NormalExit || exitCode != 0)
                {
                    restoreDialogAfterInstallEnd(ui, state, formatCurlFailure(exitCode, stderrData, startError), true);
                    return;
                }

                ui.StatusLabel->setText(QObject::tr("Verifying SHA-256 checksum..."));
                ui.ProgressBar->setRange(0, 0);
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

                QString verifyErr;
                if (!Updater::VerifyDownloadedAppImage(tempPath, verifiedSha256, &verifyErr))
                {
                    restoreDialogAfterInstallEnd(ui, state, verifyErr, true);
                    return;
                }

                if (::chmod(QFile::encodeName(tempPath).constData(), 0755) != 0)
                {
                    restoreDialogAfterInstallEnd(ui, state, QObject::tr("Failed to mark downloaded AppImage as executable."), true);
                    return;
                }

                ui.StatusLabel->setText(QObject::tr("Applying update and restarting..."));
                CFG->SkippedUpdateVersion.clear();
                CFG->Save();

                const QStringList shArgs = Updater::BuildSelfUpdateShellArguments(
                    QCoreApplication::applicationPid(),
                    tempPath,
                    appImagePath,
                    QCoreApplication::arguments().mid(1));

                qint64 helperPid = 0;
                if (!QProcess::startDetached(QStringLiteral("/bin/sh"),
                                             shArgs,
                                             QFileInfo(appImagePath).absolutePath(),
                                             &helperPid))
                {
                    restoreDialogAfterInstallEnd(ui, state, QObject::tr("Failed to start detached update helper (/bin/sh)."), true);
                    return;
                }

                state->Installing = false;
                ui.Dialog->accept();
                QApplication::closeAllWindows();
                QApplication::quit();
            });

        state->ActiveProc = dlProc;

        auto *pollTimer = new QTimer(dlProc);
        pollTimer->setInterval(200);
        QObject::connect(pollTimer, &QTimer::timeout, dlProc, [ui, tempPath, release, state]()
        {
            if (state->Cancelled || !ui.Dialog)
                return;
            const qint64 bytes = QFileInfo(tempPath).size();
            if (bytes <= 0)
                return;
            if (release.ExpectedSize > 0)
            {
                const qint64 bounded = std::clamp<qint64>((bytes * 100) / release.ExpectedSize, 0, 100);
                ui.ProgressBar->setValue(static_cast<int>(bounded));
                ui.StatusLabel->setText(QObject::tr("Downloading %1... (%2 / %3)")
                                            .arg(release.AssetName,
                                                 Misc::FormatBytes(static_cast<quint64>(bytes)),
                                                 Misc::FormatBytes(static_cast<quint64>(release.ExpectedSize))));
            } else
            {
                ui.StatusLabel->setText(QObject::tr("Downloading %1... (%2)")
                                            .arg(release.AssetName,
                                                 Misc::FormatBytes(static_cast<quint64>(bytes))));
            }
        });
        QObject::connect(dlProc, &QProcess::finished, pollTimer, &QTimer::stop);
        QObject::connect(dlProc, &QProcess::errorOccurred, pollTimer, &QTimer::stop);
        pollTimer->start();
    }

    void beginSelfUpdateFlow(const UpdateDialogWidgets &ui,
                             const Updater::ReleaseInfo &release,
                             const QSharedPointer<InstallState> &state)
    {
        if (!ui.Dialog || state->Installing)
            return;

        if (Updater::DetectInstallKind() != Updater::InstallKind::WritableAppImage)
        {
            QMessageBox::warning(ui.Dialog,
                                 QObject::tr("Update Unavailable"),
                                 QObject::tr("The running executable is no longer a writable AppImage. Please update manually from the Releases page."));
            return;
        }

        const QString curlPath = QStandardPaths::findExecutable(QStringLiteral("curl"));
        if (curlPath.isEmpty())
        {
            QMessageBox::warning(ui.Dialog,
                                 QObject::tr("Update Unavailable"),
                                 QObject::tr("The 'curl' tool was not found in PATH."));
            return;
        }

        const QString appImagePath = qEnvironmentVariable("APPIMAGE").trimmed();
        const QString tempPath = Updater::TempFilePathForAppImage(appImagePath);
        state->Installing = true;
        state->Cancelled = false;
        state->TempPath = tempPath;

        ui.SkipButton->setEnabled(false);
        ui.ReleasesButton->setEnabled(false);
        ui.UpdateButton->setEnabled(false);
        ui.AutoCheckBox->setEnabled(false);
        ui.CloseButton->setText(QObject::tr("Cancel"));
        ui.StatusLabel->setVisible(true);
        ui.ProgressBar->setVisible(true);
        ui.ProgressBar->setRange(0, 0);

        if (!release.ExpectedSha256.isEmpty())
        {
            startDownloadAndInstall(ui, release, curlPath, appImagePath, tempPath, release.ExpectedSha256, state);
            return;
        }

        ui.StatusLabel->setText(QObject::tr("Fetching SHA-256 checksum..."));
        const QStringList shaArgs = httpsCurlBaseArgs({
            QStringLiteral("-fsSL"),
            QStringLiteral("--max-time"), QStringLiteral("10"),
            release.Sha256Url
        });

        state->ActiveProc = runCurlCommand(
            ui.Dialog,
            curlPath,
            shaArgs,
            kCurlMetadataTimeoutMs,
            [ui, release, curlPath, appImagePath, tempPath, state](int exitCode,
                                                                    QProcess::ExitStatus exitStatus,
                                                                    const QByteArray &stdoutData,
                                                                    const QByteArray &stderrData,
                                                                    const QString &startError)
            {
                state->ActiveProc = nullptr;
                if (state->Cancelled || !ui.Dialog)
                    return;

                if (!startError.isEmpty())
                {
                    restoreDialogAfterInstallEnd(ui, state, formatCurlFailure(exitCode, stderrData, startError), true);
                    return;
                }

                QString parsedHash;
                if (exitStatus == QProcess::NormalExit && exitCode == 0 && !stdoutData.isEmpty())
                    parsedHash = Updater::ParseSha256Text(QString::fromUtf8(stdoutData), release.AssetName);

                startDownloadAndInstall(ui, release, curlPath, appImagePath, tempPath, parsedHash, state);
            });
    }
}

Updater *Updater::instance()
{
    if (!s_instance)
        s_instance = new Updater();
    return s_instance;
}

Updater::Updater(QObject *parent) : QObject(parent)
{}

Updater::InstallKind Updater::DetectInstallKind()
{
    return DetectInstallKind(qEnvironmentVariable("APPIMAGE"), QCoreApplication::applicationFilePath());
}

Updater::InstallKind Updater::DetectInstallKind(const QString &appImageEnvPath, const QString &applicationFilePath)
{
    const QString trimmedAppImage = appImageEnvPath.trimmed();
    if (!trimmedAppImage.isEmpty())
    {
        const QFileInfo fileInfo(trimmedAppImage);
        const QFileInfo dirInfo(fileInfo.absolutePath());
        const QString canonicalFile = fileInfo.canonicalFilePath();

        if (!isSystemPrefix(trimmedAppImage)
            && (canonicalFile.isEmpty() || !isSystemPrefix(canonicalFile))
            && fileInfo.exists()
            && fileInfo.isFile()
            && fileInfo.isWritable()
            && dirInfo.exists()
            && dirInfo.isWritable())
        {
            return InstallKind::WritableAppImage;
        }
        return InstallKind::ReadOnlyAppImage;
    }

    const QFileInfo appInfo(applicationFilePath);
    const QString canonicalApp = appInfo.canonicalFilePath();
    const QString resolvedApp = canonicalApp.isEmpty() ? applicationFilePath : canonicalApp;

    if (applicationFilePath.startsWith(QLatin1String("/nix/store/"))
        || resolvedApp.startsWith(QLatin1String("/nix/store/")))
    {
        return InstallKind::Nix;
    }

    if (applicationFilePath.startsWith(QLatin1String("/usr/"))
        || applicationFilePath.startsWith(QLatin1String("/opt/"))
        || applicationFilePath.startsWith(QLatin1String("/bin/"))
        || resolvedApp.startsWith(QLatin1String("/usr/"))
        || resolvedApp.startsWith(QLatin1String("/opt/"))
        || resolvedApp.startsWith(QLatin1String("/bin/")))
    {
        return InstallKind::SystemPackage;
    }

    return InstallKind::LocalBuild;
}

QString Updater::InstallKindDisplayName(InstallKind kind)
{
    switch (kind)
    {
        case InstallKind::WritableAppImage: return tr("AppImage (writable)");
        case InstallKind::ReadOnlyAppImage: return tr("AppImage (read-only)");
        case InstallKind::Nix:              return tr("Nix package");
        case InstallKind::SystemPackage:    return tr("System package");
        case InstallKind::LocalBuild:       return tr("Local build");
    }
    return tr("Unknown");
}

QString Updater::NormalizeVersion(const QString &version)
{
    QString v = version.trimmed();
    if (v.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        v.remove(0, 1);
    return v;
}

int Updater::CompareVersions(const QString &a, const QString &b)
{
    const ParsedVersion pa = parseVersion(a);
    const ParsedVersion pb = parseVersion(b);

    if (!pa.Valid || !pb.Valid)
    {
        if (!pa.Valid && !pb.Valid)
        {
            const int cmp = QString::compare(NormalizeVersion(a), NormalizeVersion(b), Qt::CaseSensitive);
            return (cmp < 0) ? -1 : ((cmp > 0) ? 1 : 0);
        }
        return pa.Valid ? 1 : -1;
    }

    const qsizetype maxLen = std::max(pa.Core.size(), pb.Core.size());
    for (qsizetype i = 0; i < maxLen; ++i)
    {
        const quint64 va = (i < pa.Core.size()) ? pa.Core.at(i) : 0;
        const quint64 vb = (i < pb.Core.size()) ? pb.Core.at(i) : 0;
        if (va < vb)
            return -1;
        if (va > vb)
            return 1;
    }

    if (pa.PreRelease.isEmpty() && pb.PreRelease.isEmpty())
        return 0;
    if (pa.PreRelease.isEmpty())
        return 1;
    if (pb.PreRelease.isEmpty())
        return -1;

    const qsizetype minPre = std::min(pa.PreRelease.size(), pb.PreRelease.size());
    for (qsizetype i = 0; i < minPre; ++i)
    {
        const int cmp = comparePreReleasePart(pa.PreRelease.at(i), pb.PreRelease.at(i));
        if (cmp != 0)
            return cmp;
    }

    if (pa.PreRelease.size() < pb.PreRelease.size())
        return -1;
    if (pa.PreRelease.size() > pb.PreRelease.size())
        return 1;
    return 0;
}

QString Updater::ArchitectureString()
{
    const QString arch = QSysInfo::currentCpuArchitecture();
    if (arch == QLatin1String("x86_64"))
        return QStringLiteral("x86_64");
    if (arch == QLatin1String("arm64"))
        return QStringLiteral("aarch64");

    struct utsname uts {};
    if (::uname(&uts) == 0 && uts.machine[0] != '\0')
        return QString::fromLatin1(uts.machine);

    return arch;
}

QString Updater::ExtractTagFromReleaseUrl(const QString &effectiveUrl)
{
    const QString url = effectiveUrl.trimmed();
    if (!url.startsWith(QLatin1String("https://github.com/")))
        return QString();

    const QString marker = QStringLiteral("/releases/tag/");
    const int markerIdx = url.indexOf(marker);
    if (markerIdx < 0)
        return QString();

    QString rawTag = url.mid(markerIdx + marker.size());
    const int queryIdx = rawTag.indexOf(QLatin1Char('?'));
    if (queryIdx >= 0)
        rawTag = rawTag.left(queryIdx);
    const int fragIdx = rawTag.indexOf(QLatin1Char('#'));
    if (fragIdx >= 0)
        rawTag = rawTag.left(fragIdx);
    while (rawTag.endsWith(QLatin1Char('/')))
        rawTag.chop(1);

    const QString decoded = QUrl::fromPercentEncoding(rawTag.toUtf8()).trimmed();
    if (decoded.isEmpty() || decoded.contains(QLatin1Char('/')) || decoded.contains(QLatin1Char('\\')) || decoded.contains(QLatin1String("..")))
        return QString();

    for (const QChar c : decoded)
    {
        if (c.isSpace() || c.unicode() < 0x20)
            return QString();
    }

    return decoded;
}

QString Updater::ParseSha256Text(const QString &text, const QString &expectedAssetName)
{
    static const QRegularExpression kHex64Re(QStringLiteral("^[0-9a-fA-F]{64}$"));
    static const QRegularExpression kStdLineRe(QStringLiteral("^([0-9a-fA-F]{64})(?:\\s+\\*?(.+))?$"));
    static const QRegularExpression kBsdLineRe(QStringLiteral("^SHA256\\s*\\((.+)\\)\\s*=\\s*([0-9a-fA-F]{64})$"));

    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const QString &rawLine : lines)
    {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;

        QString hash;
        QString filePart;

        const QRegularExpressionMatch bsdMatch = kBsdLineRe.match(line);
        if (bsdMatch.hasMatch())
        {
            filePart = bsdMatch.captured(1).trimmed();
            hash = bsdMatch.captured(2);
        } else
        {
            const QRegularExpressionMatch stdMatch = kStdLineRe.match(line);
            if (!stdMatch.hasMatch())
                continue;
            hash = stdMatch.captured(1);
            filePart = stdMatch.captured(2).trimmed();
        }

        if (!kHex64Re.match(hash).hasMatch())
            continue;

        if (!expectedAssetName.isEmpty() && !filePart.isEmpty())
        {
            if (QFileInfo(filePart).fileName() != expectedAssetName)
                continue;
        } else if (!expectedAssetName.isEmpty() && lines.size() > 1)
        {
            continue;
        }

        return hash.toLower();
    }

    return QString();
}

Updater::ReleaseInfo Updater::BuildReleaseInfo(const QString &tagName, const QString &arch)
{
    ReleaseInfo info;
    info.TagName = tagName.trimmed();
    info.Version = NormalizeVersion(info.TagName);

    const QString effectiveArch = arch.isEmpty() ? ArchitectureString() : arch;
    const QString encodedTag = QString::fromLatin1(QUrl::toPercentEncoding(info.TagName));

    info.ReleaseUrl = QStringLiteral("https://github.com/")
                    + QString::fromLatin1(TUX_MANAGER_GITHUB_REPO)
                    + QStringLiteral("/releases/tag/")
                    + encodedTag;

    info.AssetName = QStringLiteral("tux-manager-%1-%2.AppImage").arg(info.Version, effectiveArch);

    info.DownloadUrl = QStringLiteral("https://github.com/")
                     + QString::fromLatin1(TUX_MANAGER_GITHUB_REPO)
                     + QStringLiteral("/releases/download/")
                     + encodedTag
                     + QLatin1Char('/')
                     + info.AssetName;

    info.Sha256Url = info.DownloadUrl + QStringLiteral(".sha256");
    return info;
}

bool Updater::VerifyDownloadedAppImage(const QString &filePath, const QString &expectedSha256, QString *error)
{
    static const QRegularExpression kHex64Re(QStringLiteral("^[0-9a-f]{64}$"));
    static constexpr char kElfMagic[4] = { 0x7f, 'E', 'L', 'F' };

    const QString normalizedExpected = expectedSha256.trimmed().toLower();
    if (!kHex64Re.match(normalizedExpected).hasMatch())
    {
        if (error)
            *error = QObject::tr("Expected SHA-256 checksum is missing or invalid; refusing unverified update.");
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error)
            *error = QObject::tr("Failed to open downloaded file for verification: %1").arg(file.errorString());
        return false;
    }

    if (file.size() < 4)
    {
        if (error)
            *error = QObject::tr("Downloaded AppImage file is empty or truncated.");
        return false;
    }

    const QByteArray header = file.read(4);
    if (header != QByteArray(kElfMagic, 4))
    {
        if (error)
            *error = QObject::tr("Downloaded file is not a valid ELF executable / AppImage.");
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(header);

    char buffer[65536];
    while (!file.atEnd())
    {
        const qint64 n = file.read(buffer, sizeof(buffer));
        if (n < 0)
        {
            if (error)
                *error = QObject::tr("Failed reading downloaded file during SHA-256 verification.");
            return false;
        }
        if (n > 0)
            hash.addData(QByteArrayView(buffer, static_cast<qsizetype>(n)));
    }

    const QString actualHash = QString::fromLatin1(hash.result().toHex()).toLower();
    if (actualHash != normalizedExpected)
    {
        if (error)
            *error = QObject::tr("SHA-256 checksum mismatch.\nExpected: %1\nActual:   %2").arg(normalizedExpected, actualHash);
        return false;
    }

    if (error)
        error->clear();
    return true;
}

QString Updater::TempFilePathForAppImage(const QString &appImagePath, qint64 pid)
{
    const qint64 effectivePid = (pid > 0) ? pid : QCoreApplication::applicationPid();
    const QFileInfo info(appImagePath);
    QString dirPath = info.absolutePath();
    if (dirPath.endsWith(QLatin1Char('/')) && dirPath.size() > 1)
        dirPath.chop(1);

    if (dirPath == QLatin1String("/"))
        return QStringLiteral("/.") + info.fileName() + QStringLiteral(".tmp.%1").arg(effectivePid);
    return dirPath + QStringLiteral("/.") + info.fileName() + QStringLiteral(".tmp.%1").arg(effectivePid);
}

QStringList Updater::BuildSelfUpdateShellArguments(qint64 pid,
                                                   const QString &tempPath,
                                                   const QString &appImagePath,
                                                   const QStringList &appArgs)
{
    const QString script = QStringLiteral(
        "pid=$1; tmp=$2; dst=$3; shift 3; "
        "i=0; "
        "while kill -0 \"$pid\" 2>/dev/null; do "
        "i=$((i + 1)); "
        "[ \"$i\" -ge 300 ] && exit 1; "
        "sleep 0.1; "
        "done; "
        "sleep 0.2; "
        "mv -f -- \"$tmp\" \"$dst\" && exec \"$dst\" \"$@\""
    );

    QStringList args = {
        QStringLiteral("-c"),
        script,
        QStringLiteral("tux-updater"),
        QString::number(pid),
        tempPath,
        appImagePath
    };
    args.append(appArgs);
    return args;
}

bool Updater::IsCheckInProgress() const
{
    return this->m_checkInProgress;
}

void Updater::CheckOnStartup(QWidget *parentWidget)
{
    this->CheckForUpdates(false, parentWidget);
}

void Updater::CheckForUpdates(bool userInitiated, QWidget *parentWidget)
{
    if (!userInitiated)
    {
        if (!CFG->AutoCheckUpdates)
            return;

        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (CFG->LastUpdateCheckEpoch > 0
            && now >= CFG->LastUpdateCheckEpoch
            && (now - CFG->LastUpdateCheckEpoch) < kUpdateCheckIntervalSecs)
        {
            return;
        }
    }

    if (this->m_checkInProgress)
    {
        if (userInitiated)
        {
            this->m_userInitiated = true;
            if (parentWidget)
                this->m_checkParent = parentWidget;
        }
        return;
    }

    const QString curlPath = QStandardPaths::findExecutable(QStringLiteral("curl"));
    if (curlPath.isEmpty())
    {
        const QString msg = tr("The 'curl' tool is required to check for updates, but was not found in PATH.");
        LOG_WARN(QStringLiteral("Updater: curl executable not found in PATH."));
        if (userInitiated)
        {
            QMessageBox::warning(this->resolveParent(parentWidget),
                                 tr("Update Check Unavailable"),
                                 tr("%1\n\nYou can check releases manually at:\n%2").arg(msg, QString::fromLatin1(TUX_MANAGER_RELEASES_URL)));
        }
        emit this->CheckFinished(false, QString(), msg);
        return;
    }

    this->m_checkInProgress = true;
    this->m_userInitiated = userInitiated;
    this->m_checkParent = parentWidget ? parentWidget : QApplication::activeWindow();

    this->startLatestTagCheck(curlPath);
}

void Updater::startLatestTagCheck(const QString &curlPath)
{
    const QString latestUrl = QStringLiteral("https://github.com/")
                            + QString::fromLatin1(TUX_MANAGER_GITHUB_REPO)
                            + QStringLiteral("/releases/latest");
    const QStringList args = httpsCurlBaseArgs({
        QStringLiteral("-fsSLI"),
        QStringLiteral("-o"), QStringLiteral("/dev/null"),
        QStringLiteral("-w"), QStringLiteral("%{url_effective}"),
        QStringLiteral("--max-time"), QStringLiteral("10"),
        latestUrl
    });

    runCurlCommand(
        this,
        curlPath,
        args,
        kCurlMetadataTimeoutMs,
        [this, curlPath](int exitCode,
                         QProcess::ExitStatus exitStatus,
                         const QByteArray &stdoutData,
                         const QByteArray &stderrData,
                         const QString &startError)
        {
            if (!startError.isEmpty() || exitStatus != QProcess::NormalExit || exitCode != 0)
            {
                this->finishCheckWithError(formatCurlFailure(exitCode, stderrData, startError));
                return;
            }

            const QString effectiveUrl = QString::fromUtf8(stdoutData).trimmed();
            const QString tag = ExtractTagFromReleaseUrl(effectiveUrl);

            if (tag.isEmpty())
            {
                if (effectiveUrl.endsWith(QLatin1String("/releases")))
                {
                    CFG->LastUpdateCheckEpoch = QDateTime::currentSecsSinceEpoch();
                    CFG->Save();
                    this->m_checkInProgress = false;
                    if (this->m_userInitiated)
                    {
                        QMessageBox::information(this->resolveParent(this->m_checkParent),
                                                 tr("No Updates Available"),
                                                 tr("You are up to date (v%1).").arg(NormalizeVersion(QString::fromLatin1(TUX_MANAGER_VERSION_STRING))));
                    }
                    emit this->CheckFinished(false, QString(), QString());
                    return;
                }

                this->finishCheckWithError(tr("Could not determine latest release tag from GitHub response."));
                return;
            }

            CFG->LastUpdateCheckEpoch = QDateTime::currentSecsSinceEpoch();
            CFG->Save();

            const QString currentVer = NormalizeVersion(QString::fromLatin1(TUX_MANAGER_VERSION_STRING));
            const QString latestVer = NormalizeVersion(tag);
            if (CompareVersions(currentVer, latestVer) >= 0)
            {
                this->m_checkInProgress = false;
                if (this->m_userInitiated)
                {
                    QMessageBox::information(this->resolveParent(this->m_checkParent),
                                             tr("No Updates Available"),
                                             tr("You are up to date (v%1).").arg(currentVer));
                }
                emit this->CheckFinished(false, latestVer, QString());
                return;
            }

            if (!this->m_userInitiated
                && !CFG->SkippedUpdateVersion.isEmpty()
                && NormalizeVersion(CFG->SkippedUpdateVersion) == latestVer)
            {
                LOG_DEBUG(QStringLiteral("Updater: newer version %1 skipped by user preference.").arg(latestVer));
                this->m_checkInProgress = false;
                emit this->CheckFinished(true, latestVer, QString());
                return;
            }

            const ReleaseInfo baseRelease = BuildReleaseInfo(tag);
            this->fetchReleaseMetadataAndPrompt(curlPath, baseRelease);
        });
}

void Updater::fetchReleaseMetadataAndPrompt(const QString &curlPath, const ReleaseInfo &baseRelease)
{
    struct MetadataState
    {
        ReleaseInfo Info;
        int Pending { 2 };
        QString Sha256FromFile;
        QString Sha256FromApi;
    };

    auto state = QSharedPointer<MetadataState>::create();
    state->Info = baseRelease;

    auto onOneDone = [this, state]()
    {
        state->Pending--;
        if (state->Pending > 0)
            return;

        state->Info.ExpectedSha256 = !state->Sha256FromFile.isEmpty()
                                     ? state->Sha256FromFile
                                     : state->Sha256FromApi;

        this->m_checkInProgress = false;
        emit this->CheckFinished(true, state->Info.Version, QString());
        this->PromptAndInstall(state->Info, this->resolveParent(this->m_checkParent));
    };

    // 1) Best-effort release notes + asset metadata via GitHub API
    const QString encodedTag = QString::fromLatin1(QUrl::toPercentEncoding(baseRelease.TagName));
    const QString apiUrl = QStringLiteral("https://api.github.com/repos/")
                         + QString::fromLatin1(TUX_MANAGER_GITHUB_REPO)
                         + QStringLiteral("/releases/tags/")
                         + encodedTag;
    const QStringList apiArgs = httpsCurlBaseArgs({
        QStringLiteral("-fsSL"),
        QStringLiteral("--max-time"), QStringLiteral("10"),
        QStringLiteral("-H"), QStringLiteral("Accept: application/vnd.github+json"),
        QStringLiteral("-H"), QStringLiteral("User-Agent: %1/%2").arg(QString::fromLatin1(TUX_MANAGER_PRODUCT_NAME), QString::fromLatin1(TUX_MANAGER_VERSION_STRING)),
        apiUrl
    });

    runCurlCommand(
        this,
        curlPath,
        apiArgs,
        kCurlMetadataTimeoutMs,
        [state, onOneDone](int exitCode,
                           QProcess::ExitStatus exitStatus,
                           const QByteArray &payload,
                           const QByteArray &,
                           const QString &startError)
        {
            if (startError.isEmpty() && exitStatus == QProcess::NormalExit && exitCode == 0 && !payload.isEmpty())
            {
                const QJsonDocument doc = QJsonDocument::fromJson(payload);
                if (doc.isObject())
                {
                    const QJsonObject obj = doc.object();
                    const QString body = obj.value(QStringLiteral("body")).toString().trimmed();
                    if (!body.isEmpty())
                        state->Info.ReleaseNotes = body;

                    const QJsonArray assets = obj.value(QStringLiteral("assets")).toArray();
                    for (const QJsonValue &val : assets)
                    {
                        const QJsonObject asset = val.toObject();
                        if (asset.value(QStringLiteral("name")).toString() == state->Info.AssetName)
                        {
                            state->Info.ExpectedSize = asset.value(QStringLiteral("size")).toInteger(0);
                            const QString digest = asset.value(QStringLiteral("digest")).toString();
                            if (digest.startsWith(QLatin1String("sha256:"), Qt::CaseInsensitive))
                                state->Sha256FromApi = ParseSha256Text(digest.mid(7));
                            break;
                        }
                    }
                }
            }
            onOneDone();
        });

    // 2) Rate-limit-free .sha256 asset check from GitHub releases download URL
    const QStringList shaArgs = httpsCurlBaseArgs({
        QStringLiteral("-fsSL"),
        QStringLiteral("--max-time"), QStringLiteral("10"),
        baseRelease.Sha256Url
    });

    runCurlCommand(
        this,
        curlPath,
        shaArgs,
        kCurlMetadataTimeoutMs,
        [state, onOneDone](int exitCode,
                           QProcess::ExitStatus exitStatus,
                           const QByteArray &payload,
                           const QByteArray &,
                           const QString &startError)
        {
            if (startError.isEmpty() && exitStatus == QProcess::NormalExit && exitCode == 0 && !payload.isEmpty())
                state->Sha256FromFile = ParseSha256Text(QString::fromUtf8(payload), state->Info.AssetName);
            onOneDone();
        });
}

void Updater::finishCheckWithError(const QString &errorMessage)
{
    this->m_checkInProgress = false;
    LOG_DEBUG(QStringLiteral("Updater: check failed: %1").arg(errorMessage));
    if (this->m_userInitiated)
    {
        QMessageBox::warning(this->resolveParent(this->m_checkParent),
                             tr("Update Check Failed"),
                             tr("Could not check for updates:\n%1").arg(errorMessage));
    }
    emit this->CheckFinished(false, QString(), errorMessage);
}

QWidget *Updater::resolveParent(QWidget *preferred) const
{
    if (preferred && preferred->isVisible())
        return preferred;
    return QApplication::activeWindow();
}

void Updater::PromptAndInstall(const ReleaseInfo &release, QWidget *parentWidget)
{
    if (this->m_activeDialog)
    {
        this->m_activeDialog->raise();
        this->m_activeDialog->activateWindow();
        return;
    }

    const InstallKind kind = DetectInstallKind();
    const QString installPath = (kind == InstallKind::WritableAppImage || kind == InstallKind::ReadOnlyAppImage)
                                ? qEnvironmentVariable("APPIMAGE")
                                : QCoreApplication::applicationFilePath();

    QDialog *dialog = new QDialog(this->resolveParent(parentWidget));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Update Available"));
    dialog->setMinimumWidth(480);
    this->m_activeDialog = dialog;

    auto *mainLayout = new QVBoxLayout(dialog);
    mainLayout->setSpacing(10);

    auto *headerLabel = new QLabel(tr("A new version of %1 is available!").arg(QString::fromLatin1(TUX_MANAGER_PRODUCT_NAME)), dialog);
    QFont headerFont = headerLabel->font();
    headerFont.setPointSize(headerFont.pointSize() + 2);
    headerFont.setBold(true);
    headerLabel->setFont(headerFont);
    mainLayout->addWidget(headerLabel);

    auto *versionLabel = new QLabel(
        tr("Current version: %1    →    Latest version: %2")
            .arg(NormalizeVersion(QString::fromLatin1(TUX_MANAGER_VERSION_STRING)), release.Version),
        dialog);
    versionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mainLayout->addWidget(versionLabel);

    auto *guidanceLabel = new QLabel(guidanceTextForInstallKind(kind, installPath), dialog);
    guidanceLabel->setWordWrap(true);
    mainLayout->addWidget(guidanceLabel);

    if (!release.ReleaseNotes.trimmed().isEmpty())
    {
        mainLayout->addWidget(new QLabel(tr("Release notes:"), dialog));

        auto *notesView = new QPlainTextEdit(dialog);
        notesView->setReadOnly(true);
        notesView->setPlainText(release.ReleaseNotes.trimmed());
        notesView->setMinimumHeight(90);
        notesView->setMaximumHeight(180);
        mainLayout->addWidget(notesView);
    }

    UpdateDialogWidgets ui;
    ui.Dialog = dialog;

    ui.StatusLabel = new QLabel(dialog);
    ui.StatusLabel->setWordWrap(true);
    ui.StatusLabel->setVisible(false);
    mainLayout->addWidget(ui.StatusLabel);

    ui.ProgressBar = new QProgressBar(dialog);
    ui.ProgressBar->setVisible(false);
    mainLayout->addWidget(ui.ProgressBar);

    ui.AutoCheckBox = new QCheckBox(tr("Check for updates automatically"), dialog);
    ui.AutoCheckBox->setChecked(CFG->AutoCheckUpdates);
    connect(ui.AutoCheckBox, &QCheckBox::toggled, dialog, [](bool checked)
    {
        CFG->AutoCheckUpdates = checked;
        CFG->Save();
    });
    mainLayout->addWidget(ui.AutoCheckBox);

    auto *buttonLayout = new QHBoxLayout();
    ui.SkipButton = new QPushButton(tr("Skip this version"), dialog);
    ui.ReleasesButton = new QPushButton(tr("Open Releases Page"), dialog);
    ui.UpdateButton = (kind == InstallKind::WritableAppImage)
                      ? new QPushButton(tr("Update && Restart"), dialog)
                      : nullptr;
    ui.CloseButton = new QPushButton(tr("Close"), dialog);

    buttonLayout->addWidget(ui.SkipButton);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(ui.ReleasesButton);
    if (ui.UpdateButton)
    {
        ui.UpdateButton->setDefault(true);
        buttonLayout->addWidget(ui.UpdateButton);
    } else
    {
        ui.CloseButton->setDefault(true);
    }
    buttonLayout->addWidget(ui.CloseButton);
    mainLayout->addLayout(buttonLayout);

    auto installState = QSharedPointer<InstallState>::create();

    connect(dialog, &QDialog::finished, dialog, [installState](int)
    {
        abortInstallState(installState);
    });

    connect(ui.SkipButton, &QPushButton::clicked, dialog, [dialog, release]()
    {
        CFG->SkippedUpdateVersion = release.Version;
        CFG->Save();
        dialog->accept();
    });

    connect(ui.ReleasesButton, &QPushButton::clicked, dialog, [release]()
    {
        QDesktopServices::openUrl(QUrl(release.ReleaseUrl));
    });

    connect(ui.CloseButton, &QPushButton::clicked, dialog, [ui, installState]()
    {
        if (installState->Installing)
        {
            abortInstallState(installState);
            restoreDialogAfterInstallEnd(ui, installState, QObject::tr("Update download cancelled."), false);
            return;
        }
        if (ui.Dialog)
            ui.Dialog->reject();
    });

    if (ui.UpdateButton)
    {
        connect(ui.UpdateButton, &QPushButton::clicked, dialog, [ui, release, installState]()
        {
            beginSelfUpdateFlow(ui, release, installState);
        });
    }

    if (QApplication::activeModalWidget() != nullptr)
        dialog->setWindowModality(Qt::ApplicationModal);

    dialog->show();
}
