﻿/*
 * Copyright 2014 Canonical Ltd.
 *
 * This file is part of sync-monitor.
 *
 * sync-monitor is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * contact-service-app is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "sync-configure.h"
#include "sync-account.h"
#include "syncevolution-server-proxy.h"
#include "syncevolution-session-proxy.h"
#include "eds-helper.h"
#include "dbustypes.h"

#include "config.h"

#define ACCOUNT_SYNC_INTERVAL   "30"

using namespace Accounts;

SyncConfigure::SyncConfigure(SyncAccount *account,
                             const QSettings *settings,
                             QObject *parent)
    : QObject(parent),
      m_account(account),
      m_settings(settings)
{
}

SyncConfigure::~SyncConfigure()
{
}

AccountId SyncConfigure::accountId() const
{
    return m_account->id();
}

QString SyncConfigure::accountSessionName(Account *account)
{
    return QString("%1-%2")
            .arg(account->providerName())
            .arg(account->id());
}

void SyncConfigure::configure()
{
    m_remoteDatabasesByService.clear();
    fetchRemoteCalendars();
}

void SyncConfigure::fetchRemoteCalendars()
{
    connect(m_account, SIGNAL(remoteSourcesAvailable(QArrayOfDatabases,int)),
            SLOT(onRemoteSourcesAvailable(QArrayOfDatabases, int)));
    m_account->fetchRemoteSources(m_account->calendarServiceName());
}

void SyncConfigure::onRemoteSourcesAvailable(const QArrayOfDatabases &sources, int error)
{
    m_account->disconnect(this);
    if (sources.isEmpty()) {
        qWarning() << "Account with empty sources!:" << error;
        Q_EMIT SyncConfigure::error(error);
        return;
    }
    m_remoteDatabasesByService.insert(CALENDAR_SERVICE_TYPE, sources);
    configurePeer(QStringList() << CALENDAR_SERVICE_TYPE);
}

void SyncConfigure::configurePeer(const QStringList &services)
{
    SyncEvolutionServerProxy *proxy = SyncEvolutionServerProxy::instance();
    QString peerName = accountSessionName(m_account->account());
    SyncEvolutionSessionProxy *session = proxy->openSession(peerName,
                                                            QStringList() << "all-configs");

    if (session->status() != "queueing") {
        continuePeerConfig(session, services);
    } else {
        connect(session, &SyncEvolutionSessionProxy::statusChanged,
                [this, session, services](const QString &status, uint errorNuber, QSyncStatusMap source) {
            if (errorNuber != 0) {
                qWarning() << "Fail to configure peer" << errorNuber;
                session->destroy();
                Q_EMIT error(-1);
            } else if (status != "queueing") {
                continuePeerConfig(session, services);
            }
        });
    }
}

void SyncConfigure::continuePeerConfig(SyncEvolutionSessionProxy *session, const QStringList &services)
{
    //TODO: should we disconnect statusChanged ???
    SyncEvolutionServerProxy *proxy = SyncEvolutionServerProxy::instance();
    QStringList configs = proxy->configs();

    QString peerName = accountSessionName(m_account->account());
    QString peerConfigName = QString("target-config@%1").arg(peerName);

    // config peer
    QStringMultiMap config;
    if (configs.contains(peerConfigName)) {
        config = session->getConfig(peerConfigName, false);
    } else {
        const QString serviceName = m_settings->value(CALENDAR_SERVICE_TYPE"/uoa-service", "").toString();
        const QString templateName = m_settings->value(GLOBAL_CONFIG_GROUP"/template", "Google").toString();

        qDebug() << "Create New config with template" << templateName << "for service" << serviceName;
        config = session->getConfig(templateName, true);
        //FIXME: use hardcoded calendar service, we only support calendar for now
        config[""]["username"] = QString("uoa:%1,%2").arg(m_account->id()).arg(serviceName);
        config[""]["password"] = QString();
        config[""]["consumerReady"] = "0";
        config[""]["syncURL"] = m_account->host();
        config[""]["dumpData"] = "0";
        config[""]["printChanges"] = "0";
        config[""]["maxlogdirs"] = "2";
        config[""]["loglevel"] = "1";
    }

    static QMap<QString, QString> templates;
    if (templates.isEmpty()) {
        templates.insert(CALENDAR_SERVICE_TYPE, QString("source/calendar"));
    }

    EdsHelper eds;

    bool changed = false;
    // Map [source-name] as key [dbId, inUse] as value
    QMap<QString, QPair<QString, bool> > sourceToDatabase;
    QStringList removedSources;

    Q_FOREACH(const QString &service, services.toSet()) {
        qDebug() << "Configure source for service" << service << "Account" << m_account->id();
        QString templateSource = templates.value(service, "");
        if (templateSource.isEmpty()) {
            qWarning() << "Fail to find template source. Skip service" << service;
            continue;
        }

        // create new sources if necessary
        QStringMap configTemplate = config.value(templateSource);
        if (configTemplate.isEmpty()) {
            qWarning() << "Template not found" << templateSource;
            continue;
        }

        // check for new database
        QArrayOfDatabases dbs = m_remoteDatabasesByService.value(service);
        if (dbs.isEmpty()) {
            qWarning() << "Fail to get remote databases";
            continue;
        }

        // remove sources not in use anymore
        QStringList sourcesToRemove = config.keys();

        // skip template sources
        sourcesToRemove.removeAll("source/addressbook");
        sourcesToRemove.removeAll("source/calendar");

        qDebug() << "Actual sources:" << sourcesToRemove;

        Q_FOREACH(const SyncDatabase &db, dbs) {
            if (db.name.isEmpty()) {
                continue;
            }
            // local dabase
            // WORKAROUND: Keep compatibility with old source
            // check if a source with the same account name already exists
            QString localDbId;
            if (db.name == m_account->displayName()) {
                localDbId = eds.sourceIdByName(db.name, 0);
            }
            // check if there is a source for this remote url already
            if (localDbId.isEmpty()) {
                localDbId = eds.sourceByRemoteId(db.remoteId, m_account->id()).id;
            } else {
                qDebug() << "Using legacy source:" << localDbId << db.name;
            }
            // create new source if not found
            if (localDbId.isEmpty()) {
                QString title = db.title.isEmpty() ? db.name : db.title;
                localDbId = eds.createSource(title,
                                             db.color,
                                             db.remoteId,
                                             db.writable,
                                             m_account->id());
                qDebug() << "Create new EDS source for:" << title << localDbId;
            }
            // remove qorganizer prefix: "qtorganizer:eds::"
            localDbId = localDbId.split(":").last();
            qDebug() << "\tCheck for evolution source:" << localDbId;

            // check if source is already configured
            bool found = false;
            Q_FOREACH(const QString &key, config.keys()) {
                if (key.startsWith("source/")) {
                    if (config[key].value("database") == db.source) {
                        sourcesToRemove.removeAll(key);
                        sourceToDatabase.insert(key, qMakePair(localDbId, true));
                        found = true;
                        qDebug() << "\tLocal database already configured:" << key << localDbId;
                        break;
                    }
                }
            }

            // source already configured
            if (found) {
                continue;
            }

            // remote database
            QString sourceName = formatSourceName(m_account->id(), db.remoteId);
            QString fullSourceName = QString("source/%1").arg(sourceName);
            qDebug() << "\tCreate syncevolution source" << fullSourceName;
            if (config.contains(fullSourceName)) {
                qWarning() << "Source already exists with a different db" << sourceName << config[fullSourceName]["database"];
                sourcesToRemove.removeAll(fullSourceName);
            } else {
                changed = true;

                qDebug() << "\tConfig source" << fullSourceName << sourceName << "for database" << db.name << db.source;
                QStringMap sourceConfig(configTemplate);
                sourceConfig["backend"] = "CalDav";
                sourceConfig["database"] = db.source;
                sourceConfig["syncInterval"] = ACCOUNT_SYNC_INTERVAL;
                config[fullSourceName] = sourceConfig;

                sourceToDatabase.insert(fullSourceName, qMakePair(localDbId, true));
            }
        }

        // remove remote configs not in use
        Q_FOREACH(const QString &sourceName, sourcesToRemove) {
            if (sourceName.isEmpty())
                continue;
            qDebug() << "\tRemove source not in use:" << sourceName;
            // remove config
            config.remove(sourceName);
            removedSources << sourceName;
            changed = true;
        }
    }

    if (changed) {
        bool result = session->saveConfig(peerConfigName, config);
        if (!result) {
            qWarning() << "Fail to save account client config";
            Q_EMIT error(-1);
        } else {
            qDebug() << "\tPeer Saved" << peerName;
        }
    }

    session->destroy();
    if (!changed) {
        qDebug() << "Sources config did not change. No confign needed";
        Q_EMIT done(services);
        return;
    }

    qDebug() << "\tStart local config:"
             << "\n\t-------------------";

    // local session
    session = proxy->openSession("", QStringList() << "all-configs");
    if (session->status() == "queueing") {
        qWarning() << "Fail to open local session";
        session->destroy();
        return;
    }

    // create local sources
    config = session->getConfig("@default", false);
    qDebug() << "\tLocal sources:" << config.keys();

    for(QMap<QString, QPair<QString, bool> >::Iterator i = sourceToDatabase.begin();
        i != sourceToDatabase.end(); i++) {
        const QString configName(i.key());

        // create local source when necessary
        if (!config.contains(configName)) {
            config[configName].insert("backend", "evolution-calendar");
            config[configName].insert("database", i.value().first);
            config[configName].insert("syncInterval", ACCOUNT_SYNC_INTERVAL);
            qDebug() << "\tCreate local source for[" << configName << "] = " << i.value().first;
            changed = true;
        }
    }
    qDebug() << "\t----------------------------------------------------Local config done!";
    qDebug() << "\tRemote dbs" << sourceToDatabase.keys();

    // remove local configs and databases
    Q_FOREACH(const QString &source, config.keys()) {
        const QString backend = config[source].value("backend");
        // source is not a calendar
        if (backend != CALENDAR_EDS_BACKEND)
            continue;

        // source exits on remote side
        if (sourceToDatabase.contains(source))
            continue;

        const QString database = config[source].value("database");
        if (!database.isEmpty()) {
            EdsSource eSource = eds.sourceById("qtorganizer:eds::" + database.trimmed());
            if (eSource.isValid() && (eSource.account == m_account->id())) {
                qDebug() << "Remove local config and database" << source << config[source].value("database");
                Q_EMIT sourceRemoved(source);
                eds.removeSource(eSource.id);
                config.remove(source);
                removedSources << source;
                changed = true;
            }
        }
    }
    if (changed) {
        if (!session->saveConfig("@default", config)) {
            qWarning() << "Fail to save @default config";
        } else {
            qDebug() << "Local config saved!";
        }
    }

    // create sync config
    if (!session->hasConfig(peerName)) {
        qDebug() << "Create peer config on default config" << peerName;
        config = session->getConfig("SyncEvolution_Client", true);
    } else {
        qDebug() << "Update peer config";
        config = session->getConfig(peerName, false);
    }

    config[""]["syncURL"] = QString("local://@%1").arg(peerName);
    config[""]["username"] = QString();
    config[""]["password"] = QString();
    config[""]["loglevel"] = "1";
    config[""]["dumpData"] = "0";
    config[""]["printChanges"] = "0";
    config[""]["maxlogdirs"] = "2";
    if (!session->saveConfig(peerName, config)) {
        qWarning() << "Fail to save sync config" << peerName;
    } else {
        qDebug() << "Local peer saved!";
    }

    session->destroy();
    SyncEvolutionServerProxy::destroy();

    // remove sources dir when necessary
    Q_FOREACH(const QString &key, removedSources) {
        QString sourceName = key.mid(key.indexOf('/') + 1);
        removeAccountSourceConfig(m_account->account(), sourceName);
    }

    Q_EMIT done(services);
}

QString SyncConfigure::normalizeDBName(const QString &name)
{
    QString sourceName;
    for(int i=0; i < name.length(); i++) {
        if (name.at(i).isLetterOrNumber()) {
            sourceName += name.at(i);
        }
    }
    return sourceName.toLower();
}

QString SyncConfigure::formatSourceName(uint accountId, const QString &remoteId)
{
    QString id = QString("%1_%2").arg(accountId).arg(remoteId.split("@").first());
    id = SyncConfigure::normalizeDBName(id);
    // WORKAROUND: trunc source name to 30 chars
    // Syncevolution only support source names with max 30 chars.
    return (id.size() > 30 ? id.left(30) : id);
}

void SyncConfigure::removeAccountSourceConfig(Account *account, const QString &sourceName)
{

    QString configPath;

    if (sourceName.startsWith(account->id())) {
        //./default/sources/<source-name>
        configPath = QString("%1/default/sources/%2")
                .arg(QStandardPaths::locate(QStandardPaths::ConfigLocation,
                                            QStringLiteral("syncevolution"),
                                            QStandardPaths::LocateDirectory))
                .arg(sourceName);
        removeConfigDir(configPath);
    }

    //./default/peers/<provider>-<account-id>/sources/<source-name>
    configPath = QString("%1/default/peers/%2-%3/sources/%4")
            .arg(QStandardPaths::locate(QStandardPaths::ConfigLocation,
                                        QStringLiteral("syncevolution"),
                                        QStandardPaths::LocateDirectory))
            .arg(account->providerName())
            .arg(account->id())
            .arg(sourceName);
    removeConfigDir(configPath);

    // ./<provider>-<account-id>/peers/target-config/sources/<source-name>
    configPath = QString("%1/%2-%3/peers/target-config/sources/%4")
            .arg(QStandardPaths::locate(QStandardPaths::ConfigLocation,
                                        QStringLiteral("syncevolution"),
                                        QStandardPaths::LocateDirectory))
            .arg(account->providerName())
            .arg(account->id())
            .arg(sourceName);
    removeConfigDir(configPath);

    // ./<provider>-<account-id>/sources/<source-name>
    configPath = QString("%1/%2-%3/sources/%4")
            .arg(QStandardPaths::locate(QStandardPaths::ConfigLocation,
                                        QStringLiteral("syncevolution"),
                                        QStandardPaths::LocateDirectory))
            .arg(account->providerName())
            .arg(account->id())
            .arg(sourceName);
    removeConfigDir(configPath);

}

void SyncConfigure::removeAccountConfig(uint accountId)
{
    QString configPath = QString("%1/")
            .arg(QStandardPaths::locate(QStandardPaths::ConfigLocation,
                                        QStringLiteral("syncevolution"),
                                        QStandardPaths::LocateDirectory));
    qDebug() << "Will remove config from old account:" << accountId;
    QDir configDir(configPath);
    configDir.setNameFilters(QStringList() << "*-*");
    Q_FOREACH(const QString &dir, configDir.entryList()) {
        if (dir.endsWith(QString("-%1").arg(accountId))) {
            removeConfigDir(configDir.absoluteFilePath(dir));
        }
    }

    //~/.config/syncevolution/default/peers/<provider>-<account-id>
    configPath = QString("%1/default/peers/")
                .arg(QStandardPaths::locate(QStandardPaths::ConfigLocation,
                                            QStringLiteral("syncevolution"),
                                            QStandardPaths::LocateDirectory));
    configDir = QDir(configPath);
    configDir.setNameFilters(QStringList() << "*-*");
    Q_FOREACH(const QString &dir, configDir.entryList()) {
        if (dir.endsWith(QString("-%1").arg(accountId))) {
            removeConfigDir(configDir.absoluteFilePath(dir));
        }
    }

    //~/.config/syncevolution/default/sources/<source-name>
    configPath = QString("%1/default/sources/")
                .arg(QStandardPaths::locate(QStandardPaths::ConfigLocation,
                                            QStringLiteral("syncevolution"),
                                            QStandardPaths::LocateDirectory));
    configDir = QDir(configPath);
    configDir.setNameFilters(QStringList() << "*");
    EdsHelper eds;

    Q_FOREACH(const QString &dir, configDir.entryList()) {
        QSettings config(configDir.absoluteFilePath(dir) + "/config.ini", QSettings::IniFormat);
        if (config.value("backend").toString() == CALENDAR_EDS_BACKEND) {
            const QString dbId = config.value("database").toString();
            EdsSource eSource = eds.sourceById("qtorganizer:eds::" + dbId);
            if (!eSource.isValid()) {
                removeConfigDir(configDir.absoluteFilePath(dir));
            }
        }
    }
}

bool SyncConfigure::removeConfigDir(const QString &dirPath)
{
    QDir dir(dirPath);
    if (dir.exists()) {
        if (dir.removeRecursively()) {
            qDebug() << "Config dir removed" << dir.absolutePath();
        }
    }
}

void SyncConfigure::dumpMap(const QStringMap &map)
{
    QMapIterator<QString, QString> i(map);
    while (i.hasNext()) {
        i.next();
        qDebug() << i.key() << ": " << i.value() << endl;
    }
}

void SyncConfigure::dumpMap(const QStringMultiMap &map)
{
    for (QStringMultiMap::const_iterator i=map.begin(); i != map.end(); i++) {
        for (QStringMap::const_iterator iv=i.value().begin(); iv != i.value().end(); iv++) {
            qDebug() << QString("[%1][%2] = %3").arg(i.key()).arg(iv.key()).arg(iv.value());
        }
    }
}

