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

#ifndef UPDATER_H
#define UPDATER_H

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QDialog;
class QWidget;

class Updater : public QObject
{
    Q_OBJECT

    public:
        enum class InstallKind
        {
            WritableAppImage,
            ReadOnlyAppImage,
            Nix,
            SystemPackage,
            LocalBuild
        };
        Q_ENUM(InstallKind)

        struct ReleaseInfo
        {
            QString TagName;
            QString Version;
            QString ReleaseUrl;
            QString ReleaseNotes;
            QString AssetName;
            QString DownloadUrl;
            QString Sha256Url;
            QString ExpectedSha256;
            qint64 ExpectedSize { 0 };
        };

        static Updater *instance();

        static InstallKind DetectInstallKind();
        static InstallKind DetectInstallKind(const QString &appImageEnvPath, const QString &applicationFilePath);
        static QString InstallKindDisplayName(InstallKind kind);

        static QString NormalizeVersion(const QString &version);
        static int CompareVersions(const QString &a, const QString &b);
        static QString ArchitectureString();

        static QString ExtractTagFromReleaseUrl(const QString &effectiveUrl);
        static QString ParseSha256Text(const QString &text, const QString &expectedAssetName = QString());
        static ReleaseInfo BuildReleaseInfo(const QString &tagName, const QString &arch = QString());
        static bool VerifyDownloadedAppImage(const QString &filePath, const QString &expectedSha256, QString *error = nullptr);
        static QString TempFilePathForAppImage(const QString &appImagePath, qint64 pid = -1);
        static QStringList BuildSelfUpdateShellArguments(qint64 pid,
                                                         const QString &tempPath,
                                                         const QString &appImagePath,
                                                         const QStringList &appArgs = QStringList());

        void CheckOnStartup(QWidget *parentWidget = nullptr);
        void CheckForUpdates(bool userInitiated, QWidget *parentWidget = nullptr);
        void PromptAndInstall(const ReleaseInfo &release, QWidget *parentWidget = nullptr);

        bool IsCheckInProgress() const;

    signals:
        void CheckFinished(bool updateAvailable, const QString &latestVersion, const QString &errorMessage);

    private:
        explicit Updater(QObject *parent = nullptr);
        ~Updater() override = default;

        void startLatestTagCheck(const QString &curlPath);
        void fetchReleaseMetadataAndPrompt(const QString &curlPath, const ReleaseInfo &baseRelease);
        void finishCheckWithError(const QString &errorMessage);
        QWidget *resolveParent(QWidget *preferred) const;

        bool m_checkInProgress { false };
        bool m_userInitiated { false };
        QPointer<QWidget> m_checkParent;
        QPointer<QDialog> m_activeDialog;

        static Updater *s_instance;
};

#endif // UPDATER_H
