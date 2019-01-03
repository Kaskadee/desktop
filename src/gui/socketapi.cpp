/*
 * Copyright (C) by Dominik Schmidt <dev@dominik-schmidt.de>
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 * Copyright (C) by Roeland Jago Douma <roeland@famdouma.nl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "socketapi.h"

#include "config.h"
#include "configfile.h"
#include "folderman.h"
#include "folder.h"
#include "theme.h"
#include "common/syncjournalfilerecord.h"
#include "syncengine.h"
#include "syncfileitem.h"
#include "filesystem.h"
#include "version.h"
#include "account.h"
#include "accountstate.h"
#include "account.h"
#include "capabilities.h"
#include "common/asserts.h"
#include "guiutility.h"
#ifndef OWNCLOUD_TEST
#include "sharemanager.h"
#endif

#include <array>
#include <QBitArray>
#include <QUrl>
#include <QMetaMethod>
#include <QMetaObject>
#include <QStringList>
#include <QScopedPointer>
#include <QFile>
#include <QDir>
#include <QApplication>
#include <QLocalSocket>
#include <QStringBuilder>
#include <QMessageBox>

#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>


#include "configfile.h"
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
#include <QStandardPaths>
#endif

#if defined(Q_OS_WIN)
#include "vfs_windows.h"
#include <shlobj_core.h>
#include <windows.h>
#endif

// This is the version that is returned when the client asks for the VERSION.
// The first number should be changed if there is an incompatible change that breaks old clients.
// The second number should be changed when there are new features.
#define MIRALL_SOCKET_API_VERSION "1.1"

static inline QString removeTrailingSlash(QString path)
{
    Q_ASSERT(path.endsWith(QLatin1Char('/')));
    path.truncate(path.length() - 1);
    return path;
}

static QString buildMessage(const QString &verb, const QString &path, const QString &status = QString())
{
    QString msg(verb);

    if (!status.isEmpty()) {
        msg.append(QLatin1Char(':'));
        msg.append(status);
    }
    if (!path.isEmpty()) {
        msg.append(QLatin1Char(':'));
        QFileInfo fi(path);
        msg.append(QDir::toNativeSeparators(fi.absoluteFilePath()));
    }
    return msg;
}

namespace OCC {

Q_LOGGING_CATEGORY(lcSocketApi, "nextcloud.gui.socketapi", QtInfoMsg)
Q_LOGGING_CATEGORY(lcPublicLink, "nextcloud.gui.socketapi.publiclink", QtInfoMsg)

class BloomFilter
{
    // Initialize with m=1024 bits and k=2 (high and low 16 bits of a qHash).
    // For a client navigating in less than 100 directories, this gives us a probability less than (1-e^(-2*100/1024))^2 = 0.03147872136 false positives.
    const static int NumBits = 1024;

public:
    BloomFilter()
        : hashBits(NumBits)
    {
    }

    void storeHash(uint hash)
    {
        hashBits.setBit((hash & 0xFFFF) % NumBits); // NOLINT it's uint all the way and the modulo puts us back in the 0..1023 range
        hashBits.setBit((hash >> 16) % NumBits); // NOLINT
    }
    bool isHashMaybeStored(uint hash) const
    {
        return hashBits.testBit((hash & 0xFFFF) % NumBits) // NOLINT
            && hashBits.testBit((hash >> 16) % NumBits); // NOLINT
    }

private:
    QBitArray hashBits;
};

class SocketListener
{
public:
    QIODevice *socket;

    SocketListener(QIODevice *socket = 0)
        : socket(socket)
    {
    }

    void sendMessage(const QString &message, bool doWait = false) const
    {
        qCInfo(lcSocketApi) << "Sending SocketAPI message -->" << message << "to" << socket;
        QString localMessage = message;
        if (!localMessage.endsWith(QLatin1Char('\n'))) {
            localMessage.append(QLatin1Char('\n'));
        }

        QByteArray bytesToSend = localMessage.toUtf8();
        qint64 sent = socket->write(bytesToSend);
        if (doWait) {
            socket->waitForBytesWritten(1000);
        }
        if (sent != bytesToSend.length()) {
            qCWarning(lcSocketApi) << "Could not send all data on socket for " << localMessage;
        }
    }

    void sendMessageIfDirectoryMonitored(const QString &message, uint systemDirectoryHash) const
    {
        if (_monitoredDirectoriesBloomFilter.isHashMaybeStored(systemDirectoryHash))
            sendMessage(message, false);
    }

    void registerMonitoredDirectory(uint systemDirectoryHash)
    {
        _monitoredDirectoriesBloomFilter.storeHash(systemDirectoryHash);
    }

private:
    BloomFilter _monitoredDirectoriesBloomFilter;
};

struct ListenerHasSocketPred
{
    QIODevice *socket;
    ListenerHasSocketPred(QIODevice *socket)
        : socket(socket)
    {
    }
    bool operator()(const SocketListener &listener) const { return listener.socket == socket; }
};

SocketApi::SocketApi(QObject *parent)
    : QObject(parent)
{
    QString socketPath;

    if (Utility::isWindows()) {
        socketPath = QLatin1String(R"(\\.\pipe\)")
            + QLatin1String(APPLICATION_EXECUTABLE)
            + QLatin1String("-")
            + QString::fromLocal8Bit(qgetenv("USERNAME"));
        // TODO: once the windows extension supports multiple
        // client connections, switch back to the theme name
        // See issue #2388
        // + Theme::instance()->appName();
    } else if (Utility::isMac()) {
        // This must match the code signing Team setting of the extension
        // Example for developer builds (with ad-hoc signing identity): "" "com.owncloud.desktopclient" ".socketApi"
        // Example for official signed packages: "9B5WD74GWJ." "com.owncloud.desktopclient" ".socketApi"
        socketPath = SOCKETAPI_TEAM_IDENTIFIER_PREFIX APPLICATION_REV_DOMAIN ".socketApi";
#ifdef Q_OS_MAC
        // Tell Finder to use the Extension (checking it from System Preferences -> Extensions)
        system("pluginkit -e use -i  " APPLICATION_REV_DOMAIN ".FinderSyncExt &");
#endif
    } else if (Utility::isLinux() || Utility::isBSD()) {
        QString runtimeDir;
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
        runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
#else
        runtimeDir = QFile::decodeName(qgetenv("XDG_RUNTIME_DIR"));
        if (runtimeDir.isEmpty()) {
            runtimeDir = QDir::tempPath() + QLatin1String("/runtime-")
                + QString::fromLocal8Bit(qgetenv("USER"));
            QDir().mkdir(runtimeDir);
        }
#endif
        socketPath = runtimeDir + "/" + Theme::instance()->appName() + "/socket";
    } else {
        qCWarning(lcSocketApi) << "An unexpected system detected, this probably won't work.";
    }

    SocketApiServer::removeServer(socketPath);
    QFileInfo info(socketPath);
    if (!info.dir().exists()) {
        bool result = info.dir().mkpath(".");
        qCDebug(lcSocketApi) << "creating" << info.dir().path() << result;
        if (result) {
            QFile::setPermissions(socketPath,
                QFile::Permissions(QFile::ReadOwner + QFile::WriteOwner + QFile::ExeOwner));
        }
    }
    if (!_localServer.listen(socketPath)) {
        qCWarning(lcSocketApi) << "can't start server" << socketPath;
    } else {
        qCInfo(lcSocketApi) << "server started, listening at " << socketPath;
    }

    connect(&_localServer, &SocketApiServer::newConnection, this, &SocketApi::slotNewConnection);

    // folder watcher
    connect(FolderMan::instance(), &FolderMan::folderSyncStateChange, this, &SocketApi::slotUpdateFolderView);
}

SocketApi::~SocketApi()
{
    qCDebug(lcSocketApi) << "dtor";
    _localServer.close();
    // All remaining sockets will be destroyed with _localServer, their parent
    ASSERT(_listeners.isEmpty() || _listeners.first().socket->parent() == &_localServer);
    _listeners.clear();

#ifdef Q_OS_MAC
    // Unload the extension (uncheck from System Preferences -> Extensions)
    system("pluginkit -e ignore -i  " APPLICATION_REV_DOMAIN ".FinderSyncExt &");
#endif
}

void SocketApi::slotNewConnection()
{
    // Note that on macOS this is not actually a line-based QIODevice, it's a SocketApiSocket which is our
    // custom message based macOS IPC.
    QIODevice *socket = _localServer.nextPendingConnection();

    if (!socket) {
        return;
    }
    qCInfo(lcSocketApi) << "New connection" << socket;
    connect(socket, &QIODevice::readyRead, this, &SocketApi::slotReadSocket);
    connect(socket, SIGNAL(disconnected()), this, SLOT(onLostConnection()));
    connect(socket, &QObject::destroyed, this, &SocketApi::slotSocketDestroyed);
    ASSERT(socket->readAll().isEmpty());

    _listeners.append(SocketListener(socket));
    SocketListener &listener = _listeners.last();

    foreach (Folder *f, FolderMan::instance()->map()) {
        if (f->canSync()) {
            QString message_mirror = buildRegisterPathMessage(removeTrailingSlash(f->path()));
            QString message_fs = buildRegisterFsMessage();
            listener.sendMessage(message_mirror);
            listener.sendMessage(message_fs);
        }
    }
}

void SocketApi::onLostConnection()
{
    qCInfo(lcSocketApi) << "Lost connection " << sender();
    sender()->deleteLater();
}

void SocketApi::slotSocketDestroyed(QObject *obj)
{
    QIODevice *socket = static_cast<QIODevice *>(obj);
    _listeners.erase(std::remove_if(_listeners.begin(), _listeners.end(), ListenerHasSocketPred(socket)), _listeners.end());
}

void SocketApi::slotReadSocket()
{
    QIODevice *socket = qobject_cast<QIODevice *>(sender());
    ASSERT(socket);
    SocketListener *listener = &*std::find_if(_listeners.begin(), _listeners.end(), ListenerHasSocketPred(socket));

    while (socket->canReadLine()) {
        // Make sure to normalize the input from the socket to
        // make sure that the path will match, especially on OS X.
        QString line = QString::fromUtf8(socket->readLine()).normalized(QString::NormalizationForm_C);
        line.chop(1); // remove the '\n'
        qCInfo(lcSocketApi) << "Received SocketAPI message <--" << line << "from" << socket;
        QByteArray command = line.split(":").value(0).toLatin1();
        QByteArray functionWithArguments = "command_" + command + "(QString,SocketListener*)";
        int indexOfMethod = staticMetaObject.indexOfMethod(functionWithArguments);

        QString argument = line.remove(0, command.length() + 1);
        if (indexOfMethod != -1) {
            staticMetaObject.method(indexOfMethod).invoke(this, Q_ARG(QString, argument), Q_ARG(SocketListener *, listener));
        } else {
            qCWarning(lcSocketApi) << "The command is not supported by this version of the client:" << command << "with argument:" << argument;
        }
    }
}

void SocketApi::slotRegisterPath(const QString &alias)
{
    // Make sure not to register twice to each connected client
    if (_registeredAliases.contains(alias))
        return;

    Folder *f = FolderMan::instance()->folder(alias);
    if (f) {
        QString message_mirror = buildRegisterPathMessage(removeTrailingSlash(f->path()));
        QString message_fs = buildRegisterFsMessage();
        foreach (auto &listener, _listeners) {
            listener.sendMessage(message_mirror);
            listener.sendMessage(message_fs);
        }
    }

    _registeredAliases.insert(alias);
}

void SocketApi::slotUnregisterPath(const QString &alias)
{
    if (!_registeredAliases.contains(alias))
        return;

    Folder *f = FolderMan::instance()->folder(alias);
	if (f)
	{
#if defined(Q_OS_WIN)
		ConfigFile Cfg;
		QString FileStreamLetterDrive = Cfg.defaultFileStreamLetterDrive().toUpper().append(":/");
		broadcastMessage(buildMessage(QLatin1String("UNREGISTER_PATH"), FileStreamLetterDrive, QString()), true);
#elif defined(Q_OS_MAC)
		broadcastMessage(buildMessage(QLatin1String("UNREGISTER_PATH"), removeTrailingSlash(f->path()), QString()), true);
#endif
	}

    _registeredAliases.remove(alias);
}

void SocketApi::slotUpdateFolderView(Folder *f)
{
    if (_listeners.isEmpty()) {
        return;
    }

    if (f) {
        // do only send UPDATE_VIEW for a couple of status
        if (f->syncResult().status() == SyncResult::SyncPrepare
            || f->syncResult().status() == SyncResult::Success
            || f->syncResult().status() == SyncResult::Paused
            || f->syncResult().status() == SyncResult::Problem
            || f->syncResult().status() == SyncResult::Error
            || f->syncResult().status() == SyncResult::SetupError) {
            QString rootPath = removeTrailingSlash(f->path());

#if defined(Q_OS_WIN)
			ConfigFile Cfg;
			QString FileStreamLetterDrive = Cfg.defaultFileStreamLetterDrive().toUpper().append(":");
			broadcastStatusPushMessage(FileStreamLetterDrive, f->syncEngine().syncFileStatusTracker().fileStatus(""));
			broadcastMessage(buildMessage(QLatin1String("UPDATE_VIEW"), FileStreamLetterDrive.append("/")));
#elif defined(Q_OS_MAC)
			broadcastStatusPushMessage(rootPath, f->syncEngine().syncFileStatusTracker().fileStatus(""));
			broadcastMessage(buildMessage(QLatin1String("UPDATE_VIEW"), rootPath));
#endif
        } else {
            qCDebug(lcSocketApi) << "Not sending UPDATE_VIEW for" << f->alias() << "because status() is" << f->syncResult().status();
        }
    }
}

void SocketApi::broadcastMessage(const QString &msg, bool doWait)
{
    foreach (auto &listener, _listeners) {
        listener.sendMessage(msg, doWait);
    }
}

void SocketApi::processShareRequest(const QString &localFileC, SocketListener *listener, ShareDialogStartPage startPage)
{
    QString localFile = localFileC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = localFile.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        localFile.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    auto theme = Theme::instance();

    auto fileData = FileData::get(localFile);
    auto shareFolder = fileData.folder;
    if (!shareFolder) {
        const QString message = QLatin1String("SHARE:NOP:") + QDir::toNativeSeparators(localFile);
        // files that are not within a sync folder are not synced.
        listener->sendMessage(message);
    } else if (!shareFolder->accountState()->isConnected()) {
        const QString message = QLatin1String("SHARE:NOTCONNECTED:") + QDir::toNativeSeparators(localFile);
        // if the folder isn't connected, don't open the share dialog
        listener->sendMessage(message);
    } else if (!theme->linkSharing() && (!theme->userGroupSharing() || shareFolder->accountState()->account()->serverVersionInt() < Account::makeServerVersion(8, 2, 0))) {
        const QString message = QLatin1String("SHARE:NOP:") + QDir::toNativeSeparators(localFile);
        listener->sendMessage(message);
    } else {
        // If the file doesn't have a journal record, it might not be uploaded yet
        if (!fileData.journalRecord().isValid()) {
            const QString message = QLatin1String("SHARE:NOTSYNCED:") + QDir::toNativeSeparators(localFile);
            listener->sendMessage(message);
            return;
        }

        auto &remotePath = fileData.accountRelativePath;

        // Can't share root folder
        if (remotePath == "/") {
            const QString message = QLatin1String("SHARE:CANNOTSHAREROOT:") + QDir::toNativeSeparators(localFile);
            listener->sendMessage(message);
            return;
        }

        const QString message = QLatin1String("SHARE:OK:") + QDir::toNativeSeparators(localFile);
        listener->sendMessage(message);

        emit shareCommandReceived(remotePath, fileData.localPath, startPage);
    }
}

void SocketApi::broadcastStatusPushMessage(const QString &systemPath, SyncFileStatus fileStatus)
{
    QString msg = buildMessage(QLatin1String("STATUS"), systemPath, fileStatus.toSocketAPIString());
    Q_ASSERT(!systemPath.endsWith('/'));
#if defined(Q_OS_WIN)
	ConfigFile Cfg;
	QString FileStreamLetterDrive = Cfg.defaultFileStreamLetterDrive().toUpper().append("://");
    uint directoryHash = qHash(systemPath.left(FileStreamLetterDrive.lastIndexOf('/')));
#elif defined(Q_OS_MAC)
    uint directoryHash = qHash(systemPath.left(systemPath.lastIndexOf('/')));
#endif
    foreach (auto &listener, _listeners) {
#if defined(Q_OS_WIN)
        QString relative_prefix = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/";
        QString systemPath2 = systemPath;
        systemPath2.replace(0, relative_prefix.length(), Cfg.defaultFileStreamLetterDrive().toUpper().append(":"));
        QString msg2 = buildMessage(QLatin1String("STATUS"), systemPath2, fileStatus.toSocketAPIString());
		listener.sendMessageIfDirectoryMonitored(msg2, directoryHash);
#elif defined(Q_OS_MAC)
		listener.sendMessageIfDirectoryMonitored(msg, directoryHash);
#endif
    }
}

void SocketApi::command_RETRIEVE_FOLDER_STATUS(const QString &argument, SocketListener *listener)
{
    // This command is the same as RETRIEVE_FILE_STATUS
    command_RETRIEVE_FILE_STATUS(argument, listener);
}

void SocketApi::command_RETRIEVE_FILE_STATUS(const QString &argumentC, SocketListener *listener)
{
	QString argument = argumentC;
	QString relative_prefix = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/";
	argument.replace(0, 3, relative_prefix);

	QString statusString;

    auto fileData = FileData::get(argument);
    if (!fileData.folder) {
        // this can happen in offline mode e.g.: nothing to worry about
        statusString = QLatin1String("NOP");
    } else {
        QString systemPath = QDir::cleanPath(argument);
        if (systemPath.endsWith(QLatin1Char('/'))) {
            systemPath.truncate(systemPath.length() - 1);
            qCWarning(lcSocketApi) << "Removed trailing slash for directory: " << systemPath << "Status pushes won't have one.";
        }
        // The user probably visited this directory in the file shell.
        // Let the listener know that it should now send status pushes for sibblings of this file.
        QString directory = fileData.localPath.left(fileData.localPath.lastIndexOf('/'));
#if defined(Q_OS_WIN)
		ConfigFile Cfg;
		QString FileStreamLetterDrive = Cfg.defaultFileStreamLetterDrive().toUpper().append("://");
		listener->registerMonitoredDirectory(qHash(FileStreamLetterDrive));
#elif defined(Q_OS_MAC)
		listener->registerMonitoredDirectory(qHash(directory));
#endif
        SyncFileStatus fileStatus = fileData.syncFileStatus();
        statusString = fileStatus.toSocketAPIString();
    }

	//QString message2 = message;
	//QString relative_prefix = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/";
	argument.replace(0, relative_prefix.length(), QString("X:\\"));

    const QString message = QLatin1String("STATUS:") % statusString % QLatin1Char(':') % QDir::toNativeSeparators(argument);

    listener->sendMessage(message);
}

void SocketApi::command_SHARE(const QString &localFileC, SocketListener *listener)
{
    QString localFile = localFileC;
    
#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = localFile.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        localFile.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    processShareRequest(localFile, listener, ShareDialogStartPage::UsersAndGroups);
}

void SocketApi::command_MANAGE_PUBLIC_LINKS(const QString &localFileC, SocketListener *listener)
{
    QString localFile = localFileC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = localFile.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        localFile.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    processShareRequest(localFile, listener, ShareDialogStartPage::PublicLinks);
}

void SocketApi::command_VERSION(const QString &, SocketListener *listener)
{
    listener->sendMessage(QLatin1String("VERSION:" MIRALL_VERSION_STRING ":" MIRALL_SOCKET_API_VERSION));
}

void SocketApi::command_SHARE_STATUS(const QString &localFileC, SocketListener *listener)
{
    QString localFile = localFileC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = localFile.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        localFile.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    Folder *shareFolder = FolderMan::instance()->folderForPath(localFile);

    if (!shareFolder) {
        const QString message = QLatin1String("SHARE_STATUS:NOP:") + QDir::toNativeSeparators(localFile);
        listener->sendMessage(message);
    } else {
        const QString file = QDir::cleanPath(localFile).mid(shareFolder->cleanPath().length() + 1);
        SyncFileStatus fileStatus = shareFolder->syncEngine().syncFileStatusTracker().fileStatus(file);

        // Verify the file is on the server (to our knowledge of course)
        if (fileStatus.tag() != SyncFileStatus::StatusUpToDate) {
            const QString message = QLatin1String("SHARE_STATUS:NOTSYNCED:") + QDir::toNativeSeparators(localFile);
            listener->sendMessage(message);
            return;
        }

        const Capabilities capabilities = shareFolder->accountState()->account()->capabilities();

        if (!capabilities.shareAPI()) {
            const QString message = QLatin1String("SHARE_STATUS:DISABLED:") + QDir::toNativeSeparators(localFile);
            listener->sendMessage(message);
        } else {
            auto theme = Theme::instance();
            QString available;

            if (theme->userGroupSharing()) {
                available = "USER,GROUP";
            }

            if (theme->linkSharing() && capabilities.sharePublicLink()) {
                if (available.isEmpty()) {
                    available = "LINK";
                } else {
                    available += ",LINK";
                }
            }

            if (available.isEmpty()) {
                const QString message = QLatin1String("SHARE_STATUS:DISABLED") + ":" + QDir::toNativeSeparators(localFile);
                listener->sendMessage(message);
            } else {
                const QString message = QLatin1String("SHARE_STATUS:") + available + ":" + QDir::toNativeSeparators(localFile);
                listener->sendMessage(message);
            }
        }
    }
}

void SocketApi::command_SHARE_MENU_TITLE(const QString &, SocketListener *listener)
{
    listener->sendMessage(QLatin1String("SHARE_MENU_TITLE:") + tr("Share with %1", "parameter is ownCloud").arg(Theme::instance()->appNameGUI()));
    listener->sendMessage(QLatin1String("STREAM_SUBMENU_TITLE:") + tr("Virtual Drive"));
    listener->sendMessage(QLatin1String("STREAM_OFFLINE_ITEM_TITLE:") + tr("Available offline"));
    listener->sendMessage(QLatin1String("STREAM_ONLINE_ITEM_TITLE:") + tr("Online only"));
}

void SocketApi::command_EDIT(const QString &localFile, SocketListener *listener)
{
    Q_UNUSED(listener)
    auto fileData = FileData::get(localFile);
    if (!fileData.folder) {
        qCWarning(lcSocketApi) << "Unknown path" << localFile;
        return;
    }

    auto record = fileData.journalRecord();
    if (!record.isValid())
        return;

    DirectEditor* editor = getDirectEditorForLocalFile(fileData.localPath);
    if (!editor)
        return;

    auto *job = new JsonApiJob(fileData.folder->accountState()->account(), QLatin1String("ocs/v2.php/apps/files/api/v1/directEditing/open"), this);

    QUrlQuery params;
    params.addQueryItem("path", fileData.accountRelativePath);
    params.addQueryItem("editorId", editor->id());
    job->addQueryParams(params);
    job->usePOST();

    QObject::connect(job, &JsonApiJob::jsonReceived, [](const QJsonDocument &json){
        auto data = json.object().value("ocs").toObject().value("data").toObject();
        auto url = QUrl(data.value("url").toString());

        if(!url.isEmpty())
            Utility::openBrowser(url, nullptr);
    });
    job->start();
}

// don't pull the share manager into socketapi unittests
#ifndef OWNCLOUD_TEST

class GetOrCreatePublicLinkShare : public QObject
{
    Q_OBJECT
public:
    GetOrCreatePublicLinkShare(const AccountPtr &account, const QString &localFile,
        std::function<void(const QString &link)> targetFun, QObject *parent)
        : QObject(parent)
        , _shareManager(account)
        , _localFile(localFile)
        , _targetFun(targetFun)
    {
        connect(&_shareManager, &ShareManager::sharesFetched,
            this, &GetOrCreatePublicLinkShare::sharesFetched);
        connect(&_shareManager, &ShareManager::linkShareCreated,
            this, &GetOrCreatePublicLinkShare::linkShareCreated);
        connect(&_shareManager, &ShareManager::serverError,
            this, &GetOrCreatePublicLinkShare::serverError);
    }

    void run()
    {
        qCDebug(lcPublicLink) << "Fetching shares";
        _shareManager.fetchShares(_localFile);
    }

private slots:
    void sharesFetched(const QList<QSharedPointer<Share>> &shares)
    {
        auto shareName = SocketApi::tr("Context menu share");
        // If there already is a context menu share, reuse it
        for (const auto &share : shares) {
            const auto linkShare = qSharedPointerDynamicCast<LinkShare>(share);
            if (!linkShare)
                continue;

            if (linkShare->getName() == shareName) {
                qCDebug(lcPublicLink) << "Found existing share, reusing";
                return success(linkShare->getLink().toString());
            }
        }

        // otherwise create a new one
        qCDebug(lcPublicLink) << "Creating new share";
        _shareManager.createLinkShare(_localFile, shareName, QString());
    }

    void linkShareCreated(const QSharedPointer<LinkShare> &share)
    {
        qCDebug(lcPublicLink) << "New share created";
        success(share->getLink().toString());
    }

    void serverError(int code, const QString &messageC)
    {
        QString message = messageC;

#if defined(Q_OS_WIN)
        OCC::ConfigFile Cfg;
        char letter[2];
        letter[0] = message.toStdString().c_str()[0];
        letter[1] = 0;

        if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
            message.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

        qCWarning(lcPublicLink) << "Share fetch/create error" << code << message;
        QMessageBox::warning(
            0,
            tr("Sharing error"),
            tr("Could not retrieve or create the public link share. Error:\n\n%1").arg(message),
            QMessageBox::Ok,
            QMessageBox::NoButton);
        deleteLater();
    }

private:
    void success(const QString &linkC)
    {
        QString link = linkC;

#if defined(Q_OS_WIN)
        OCC::ConfigFile Cfg;
        char letter[2];
        letter[0] = link.toStdString().c_str()[0];
        letter[1] = 0;

        if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
            link.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

        _targetFun(link);
        deleteLater();
    }

    ShareManager _shareManager;
    QString _localFile;
    std::function<void(const QString &url)> _targetFun;
};

#else

class GetOrCreatePublicLinkShare : public QObject
{
    Q_OBJECT
public:
    GetOrCreatePublicLinkShare(const AccountPtr &, const QString &,
        std::function<void(const QString &link)>, QObject *)
    {
    }

    void run()
    {
    }
};

#endif

void SocketApi::command_COPY_PUBLIC_LINK(const QString &localFileC, SocketListener *)
{
    QString localFile = localFileC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = localFile.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        localFile.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    auto fileData = FileData::get(localFile);
    if (!fileData.folder)
        return;

    AccountPtr account = fileData.folder->accountState()->account();
    auto job = new GetOrCreatePublicLinkShare(account, fileData.accountRelativePath, [](const QString &url) { copyUrlToClipboard(url); }, this);
    job->run();
}

// Windows Shell / Explorer pinning fallbacks, see issue: https://github.com/nextcloud/desktop/issues/1599
#ifdef Q_OS_WIN
void SocketApi::command_COPYASPATH(const QString &localFile, SocketListener *)
{
    QApplication::clipboard()->setText(localFile);
}

void SocketApi::command_OPENNEWWINDOW(const QString &localFile, SocketListener *)
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(localFile));
}

void SocketApi::command_OPEN(const QString &localFile, SocketListener *socketListener)
{
    command_OPENNEWWINDOW(localFile, socketListener);
}
#endif

// Fetches the private link url asynchronously and then calls the target slot
void SocketApi::fetchPrivateLinkUrlHelper(const QString &localFileC, const std::function<void(const QString &url)> &targetFun)
{
    QString localFile = localFileC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = localFile.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        localFile.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    auto fileData = FileData::get(localFile);
    if (!fileData.folder) {
        qCWarning(lcSocketApi) << "Unknown path" << localFile;
        return;
    }

    auto record = fileData.journalRecord();
    if (!record.isValid())
        return;

    fetchPrivateLinkUrl(
        fileData.folder->accountState()->account(),
        fileData.accountRelativePath,
        record.numericFileId(),
        this,
        targetFun);
}

void SocketApi::command_COPY_PRIVATE_LINK(const QString &localFileC, SocketListener *)
{
    QString localFile = localFileC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = localFile.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        localFile.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    fetchPrivateLinkUrlHelper(localFile, &SocketApi::copyUrlToClipboard);
}

void SocketApi::command_EMAIL_PRIVATE_LINK(const QString &localFileC, SocketListener *)
{
    QString localFile = localFileC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = localFile.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        localFile.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    fetchPrivateLinkUrlHelper(localFile, &SocketApi::emailPrivateLink);
}

void SocketApi::command_OPEN_PRIVATE_LINK(const QString &localFileC, SocketListener *)
{
    QString localFile = localFileC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = localFile.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        localFile.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    fetchPrivateLinkUrlHelper(localFile, &SocketApi::openPrivateLink);
}

void SocketApi::copyUrlToClipboard(const QString &linkC)
{
    QString link = linkC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = link.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        link.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    QApplication::clipboard()->setText(link);
}

void SocketApi::emailPrivateLink(const QString &linkC)
{
    QString link = linkC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = link.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        link.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    Utility::openEmailComposer(
        tr("I shared something with you"),
        link,
        0);
}

void OCC::SocketApi::openPrivateLink(const QString &linkC)
{
    QString link = linkC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = link.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        link.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    Utility::openBrowser(link, nullptr);
}

void SocketApi::command_GET_STRINGS(const QString &argumentC, SocketListener *listener)
{
    QString argument = argumentC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = argument.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        argument.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    static std::array<std::pair<const char *, QString>, 5> strings{ {
        { "SHARE_MENU_TITLE", tr("Share options") },
        { "CONTEXT_MENU_TITLE", tr("Share via ") + Theme::instance()->appNameGUI()},
        { "COPY_PRIVATE_LINK_MENU_TITLE", tr("Copy private link to clipboard") },
        { "EMAIL_PRIVATE_LINK_MENU_TITLE", tr("Send private link by email...") },
    } };
    listener->sendMessage(QString("GET_STRINGS:BEGIN"));
    for (auto key_value : strings) {
        if (argument.isEmpty() || argument == QLatin1String(key_value.first)) {
            listener->sendMessage(QString("STRING:%1:%2").arg(key_value.first, key_value.second));
        }
    }
    listener->sendMessage(QString("GET_STRINGS:END"));
}

void SocketApi::sendSharingContextMenuOptions(const FileData &fileData, SocketListener *listener, bool enabled)
{
    auto record = fileData.journalRecord();
    bool isOnTheServer = record.isValid();
    auto flagString = isOnTheServer && enabled ? QLatin1String("::") : QLatin1String(":d:");

    auto capabilities = fileData.folder->accountState()->account()->capabilities();
    auto theme = Theme::instance();
    if (!capabilities.shareAPI() || !(theme->userGroupSharing() || (theme->linkSharing() && capabilities.sharePublicLink())))
        return;

    // If sharing is globally disabled, do not show any sharing entries.
    // If there is no permission to share for this file, add a disabled entry saying so

    listener->sendMessage(QLatin1String("MENU_ITEM:OFFLINE_DOWNLOAD_MODE") + flagString + tr("0ffline"));
    listener->sendMessage(QLatin1String("MENU_ITEM:ONLINE_DOWNLOAD_MODE") + flagString + tr("Online"));

    if (isOnTheServer && !record._remotePerm.isNull() && !record._remotePerm.hasPermission(RemotePermissions::CanReshare)) {
        listener->sendMessage(QLatin1String("MENU_ITEM:DISABLED:d:") + tr("Resharing this file is not allowed"));
    } else {
        listener->sendMessage(QLatin1String("MENU_ITEM:SHARE") + flagString + tr("Share..."));

        // Do we have public links?
        bool publicLinksEnabled = theme->linkSharing() && capabilities.sharePublicLink();

        // Is is possible to create a public link without user choices?
        bool canCreateDefaultPublicLink = publicLinksEnabled
            && !capabilities.sharePublicLinkEnforceExpireDate()
            && !capabilities.sharePublicLinkEnforcePassword();

        if (canCreateDefaultPublicLink) {
            listener->sendMessage(QLatin1String("MENU_ITEM:COPY_PUBLIC_LINK") + flagString + tr("Copy public link"));
        } else if (publicLinksEnabled) {
            listener->sendMessage(QLatin1String("MENU_ITEM:MANAGE_PUBLIC_LINKS") + flagString + tr("Copy public link"));
        }
    }

    listener->sendMessage(QLatin1String("MENU_ITEM:COPY_PRIVATE_LINK") + flagString + tr("Copy internal link"));

    // Disabled: only providing email option for private links would look odd,
    // and the copy option is more general.
    //listener->sendMessage(QLatin1String("MENU_ITEM:EMAIL_PRIVATE_LINK") + flagString + tr("Send private link by email..."));
}

SocketApi::FileData SocketApi::FileData::get(const QString &localFileC)
{
    QString localFile = localFileC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = localFile.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        localFile.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    FileData data;

    data.localPath = QDir::cleanPath(localFile);
    if (data.localPath.endsWith(QLatin1Char('/')))
        data.localPath.chop(1);

    data.folder = FolderMan::instance()->folderForPath(data.localPath);
    if (!data.folder)
        return data;

    data.folderRelativePath = data.localPath.mid(data.folder->cleanPath().length() + 1);
    data.accountRelativePath = QDir(data.folder->remotePath()).filePath(data.folderRelativePath);

    return data;
}

SyncFileStatus SocketApi::FileData::syncFileStatus() const
{
    if (!folder)
        return SyncFileStatus::StatusNone;
    return folder->syncEngine().syncFileStatusTracker().fileStatus(folderRelativePath);
}

SyncJournalFileRecord SocketApi::FileData::journalRecord() const
{
    SyncJournalFileRecord record;
    if (!folder)
        return record;
    folder->journalDb()->getFileRecord(folderRelativePath, &record);
    return record;
}

void SocketApi::command_GET_MENU_ITEMS(const QString &argumentC, OCC::SocketListener *listener)
{
    QString argument = argumentC;

#if defined(Q_OS_WIN)
    OCC::ConfigFile Cfg;
    char letter[2];
    letter[0] = argument.toStdString().c_str()[0];
    letter[1] = 0;

    if (!QString(letter).compare(Cfg.defaultFileStreamLetterDrive().toUpper()))
        argument.replace(0, 3, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cachedFiles/");
#endif

    listener->sendMessage(QString("GET_MENU_ITEMS:BEGIN"));

    bool hasSeveralFiles = argument.contains(QLatin1Char('\x1e')); // Record Separator
    FileData fileData = hasSeveralFiles ? FileData{} : FileData::get(argument);
    bool isOnTheServer = fileData.journalRecord().isValid();

    const auto isE2eEncryptedPath = fileData.journalRecord()._isE2eEncrypted || !fileData.journalRecord()._e2eMangledName.isEmpty();
    auto flagString = isOnTheServer && !isE2eEncryptedPath ? QLatin1String("::") : QLatin1String(":d:");

    listener->sendMessage(QLatin1String("MENU_ITEM:OFFLINE_DOWNLOAD_MODE") + flagString + tr("0ff line"));
    listener->sendMessage(QLatin1String("MENU_ITEM:ONLINE_DOWNLOAD_MODE") + flagString + tr("On line"));
    listener->sendMessage(QLatin1String("MENU_ITEM:SHARE") + flagString + tr("Share..."));

    if (fileData.folder && fileData.folder->accountState()->isConnected()) {
        DirectEditor* editor = getDirectEditorForLocalFile(fileData.localPath);
        if (editor) {
            //listener->sendMessage(QLatin1String("MENU_ITEM:EDIT") + flagString + tr("Edit via ") + editor->name());
            listener->sendMessage(QLatin1String("MENU_ITEM:EDIT") + flagString + tr("Edit"));
        } else {
            listener->sendMessage(QLatin1String("MENU_ITEM:OPEN_PRIVATE_LINK") + flagString + tr("Open in browser"));
        }

        sendSharingContextMenuOptions(fileData, listener, !isE2eEncryptedPath);
    }
    listener->sendMessage(QString("GET_MENU_ITEMS:END"));
}

DirectEditor* SocketApi::getDirectEditorForLocalFile(const QString &localFile)
{
    FileData fileData = FileData::get(localFile);
    auto capabilities = fileData.folder->accountState()->account()->capabilities();

    if (fileData.folder && fileData.folder->accountState()->isConnected()) {
        QMimeDatabase db;
        QMimeType type = db.mimeTypeForFile(localFile);

        DirectEditor* editor = capabilities.getDirectEditorForMimetype(type);
        if (!editor) {
            editor = capabilities.getDirectEditorForOptionalMimetype(type);
        }
        return editor;
    }

    return nullptr;
}


QString SocketApi::buildRegisterPathMessage(const QString &pathC)
{
#if defined(Q_OS_WIN)
	ConfigFile Cfg;
	QString path = Cfg.defaultFileStreamLetterDrive().toUpper().append(":/");
#elif defined(Q_OS_MAC)
	QString path = pathC;
#endif
    QFileInfo fi(path);
    QString message = QLatin1String("REGISTER_PATH:");
    message.append(QDir::toNativeSeparators(fi.absoluteFilePath()));
    return message;
}

QString SocketApi::buildRegisterFsMessage()
{
    ConfigFile cfg;
    QString path;
#if defined(Q_OS_WIN)
    path = QLatin1String("REGISTER_DRIVEFS:");
    path.append(cfg.defaultFileStreamLetterDrive().toUpper());
    return path;
#elif defined(Q_OS_MAC)
    path = cfg.defaultFileStreamSyncPath();
#endif
    QFileInfo fi(path);
    QString message = QLatin1String("REGISTER_DRIVEFS:");
    message.append(QDir::toNativeSeparators(fi.absoluteFilePath()));
    return message;
}

//< Mac callback for ContextMenu Online option
void SocketApi::command_ONLINE_DOWNLOAD_MODE(const QString &path, SocketListener *listener)
{
    ConfigFile cfg;
    QString relative_prefix = cfg.defaultFileStreamMirrorPath();
    QString relative_path = path;
    relative_path = relative_path.replace(0, relative_prefix.length(), QString(""));
    
    qDebug() << "\n" << Q_FUNC_INFO << "ONLINE_DOWNLOAD_MODE: " << relative_path;
    SyncJournalDb::instance()->setSyncMode(relative_path, SyncJournalDb::SYNCMODE_ONLINE);
    //< Example
    //SyncJournalDb::instance()->setSyncModeDownload(path, SyncJournalDb::SYNCMODE_DOWNLOADED_YES); //< Set when file was downloaded
    //SyncJournalDb::instance()->updateLastAccess(path);  //< Set when file was opened or updated
}

//< Mac callback for ContextMenu Offline option
void SocketApi::command_OFFLINE_DOWNLOAD_MODE(const QString &path, SocketListener *listener)
{
    ConfigFile cfg;
    QString relative_prefix = cfg.defaultFileStreamMirrorPath();
    QString relative_path = path;
    relative_path = relative_path.replace(0, relative_prefix.length(), QString(""));

    qDebug() << "\n" << Q_FUNC_INFO << "OFFLINE_DOWNLOAD_MODE: " << relative_path;
    SyncJournalDb::instance()->setSyncMode(relative_path, SyncJournalDb::SYNCMODE_OFFLINE);
    //< Example
    //SyncJournalDb::instance()->setSyncModeDownload(path, SyncJournalDb::SYNCMODE_DOWNLOADED_YES); //< Set when file was downloaded
    //SyncJournalDb::instance()->updateLastAccess(path);  //< Set when file was opened or updated
}

//< Windows callback for ContextMenu option
void SocketApi::command_SET_DOWNLOAD_MODE(const QString& argument, SocketListener* listener)
{
    qDebug() << Q_FUNC_INFO << " argument: " << argument;

    #if defined(Q_OS_WIN)
        //< Parser on type string: (for get path and type: 0 or 1).
        //< "C:\\Users\\USERNAME\\DIR_LOCAL\\Mi unidad\\Mi unidad\\b6.txt|1"
        std::string m_alias = argument.toLocal8Bit().constData();
        char *pc = (char*)m_alias.c_str();

        while (*pc != NULL)
            pc++;
        pc--;
        char *pq = pc;
        char *pw = pq -= 2;
        pq = (char*)m_alias.c_str();
        char QQ[300]; int l = 0;
        while (pq != pw)
            {
            if (l < 300) {
                QQ[l++] = *pq;
            }
            else
                {
                qDebug() << "\n" << Q_FUNC_INFO << " QQ is very small for enter value";
                break;
                }
            pq++;
            }

        QQ[l]   = *pq;
        QQ[l+1] = 0;

        QString path = QString(QQ);

        qDebug() << "\n" << Q_FUNC_INFO << " QQ==" <<QQ<< "==";

            if (*pc == '0')     //< OffLine
            {
        qDebug() << "\n" << Q_FUNC_INFO << " *pc is 0";
            SyncJournalDb::instance()->setSyncMode(path, SyncJournalDb::SYNCMODE_OFFLINE);

            //< Example
            SyncJournalDb::instance()->setSyncModeDownload(path, SyncJournalDb::SyncModeDownload::SYNCMODE_DOWNLOADED_YES); //< Set when file was downloaded
            SyncJournalDb::instance()->updateLastAccess(path);  //< Set when file was opened or updated
            }
            else if (*pc == '1')    //< Online
            {
        qDebug() << "\n" << Q_FUNC_INFO << " *pc is 1";
            SyncJournalDb::instance()->setSyncMode(path, SyncJournalDb::SYNCMODE_ONLINE);

            //< Example
            SyncJournalDb::instance()->setSyncModeDownload(path, SyncJournalDb::SYNCMODE_DOWNLOADED_YES); //< Set when file was downloaded
            SyncJournalDb::instance()->updateLastAccess(path);  //< Set when file was opened or updated
            }
#endif

    qDebug() << "\n" << Q_FUNC_INFO << " Show paths BD INIT";

        //< Show paths from SyncMode table.
        QList<QString> list = SyncJournalDb::instance()->getSyncModePaths();
            QString item;
            foreach(item, list)
                {

                SyncJournalDb::SyncMode m = SyncJournalDb::instance()->getSyncMode(item);

                if(m == SyncJournalDb::SYNCMODE_ONLINE) 
                    qDebug() << Q_FUNC_INFO << " :::BD " << item << " ONLINE";
    
                if(m == SyncJournalDb::SYNCMODE_OFFLINE) 
                    qDebug() << Q_FUNC_INFO << " :::BD " << item << " OFFLINE";
                }

    qDebug() << "\n" << Q_FUNC_INFO << " Show paths BD END";

}

void SocketApi::command_GET_DOWNLOAD_MODE(const QString& localFile, SocketListener* listener)
{
    QString downloadMode = "ONLINE";

    //< Iterate paths from SyncMode table.
    QList<QString> list = SyncJournalDb::instance()->getSyncModePaths();
    QString item;

    foreach(item, list)
    {
        qDebug() << Q_FUNC_INFO << " localFile: " << localFile << " item: " << item;
        if (item.compare(localFile) == 0)
        {
        SyncJournalDb::SyncMode m = SyncJournalDb::instance()->getSyncMode(item);
        if (m == SyncJournalDb::SYNCMODE_OFFLINE)
            downloadMode = "OFFLINE";
        break;
        }
    }

    listener->sendMessage(QLatin1String("GET_DOWNLOAD_MODE:") + downloadMode);
}
} // namespace OCC

#include "socketapi.moc"
