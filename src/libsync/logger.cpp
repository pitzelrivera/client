/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#include "logger.h"
#include "config.h"
#include "theme.h"

#include <QCoreApplication>
#include <QDir>
#include <QStringList>
#include <QtConcurrent>
#include <QtGlobal>

#include <iostream>

#include <zlib.h>

#ifdef Q_OS_WIN
#include <io.h> // for stdout
#include <fcntl.h>
#include <comdef.h>
#endif

namespace {
constexpr int crashLogSizeC = 20;
constexpr int maxLogSizeC = 1024 * 1024 * 100; // 100 MiB

#ifdef Q_OS_WIN
bool isDebuggerPresent()
{
    BOOL debugged;
    if (!CheckRemoteDebuggerPresent(GetCurrentProcess(), &debugged)) {
        const auto error = GetLastError();
        qDebug() << "Failed to detect debugger" << QString::fromWCharArray(_com_error(error).ErrorMessage());
    }
    return debugged;
}
#endif
}
namespace OCC {

Logger *Logger::instance()
{
    static auto *log = [] {
        auto log = new Logger;
        qAddPostRoutine([] {
            Logger::instance()->close();
            delete Logger::instance();
        });
        return log;
    }();
    return log;
}

Logger::Logger(QObject *parent)
    : QObject(parent)
{
    qSetMessagePattern(loggerPattern());
    _crashLog.resize(crashLogSizeC);
#ifndef NO_MSG_HANDLER
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &ctx, const QString &message) {
            Logger::instance()->doLog(type, ctx, message);
        });
#endif
}

Logger::~Logger()
{
#ifndef NO_MSG_HANDLER
    qInstallMessageHandler(0);
#endif
}

QString Logger::loggerPattern()
{
    return QStringLiteral("%{time MM-dd hh:mm:ss:zzz} [ %{type} %{category} ]%{if-debug}\t[ %{function} ]%{endif}:\t%{message}");
}

bool Logger::isLoggingToFile() const
{
    QMutexLocker lock(&_mutex);
    return _logstream;
}

void Logger::doLog(QtMsgType type, const QMessageLogContext &ctx, const QString &message)
{
    const QString msg = qFormatLogMessage(type, ctx, message) + QLatin1Char('\n');
    {
        QMutexLocker lock(&_mutex);
        _crashLogIndex = (_crashLogIndex + 1) % crashLogSizeC;
        _crashLog[_crashLogIndex] = msg;
        if (_logstream) {
            (*_logstream) << msg;
            if (_doFileFlush)
                _logstream->flush();
        }
#if defined(Q_OS_WIN)
        if (isDebuggerPresent()) {
            OutputDebugStringW(reinterpret_cast<const wchar_t *>(msg.utf16()));
        }
#endif
        if (type == QtFatalMsg) {
            dumpCrashLog();
            close();
#if defined(Q_OS_WIN)
            // Make application terminate in a way that can be caught by the crash reporter
            Utility::crash();
#endif
        }
        if (!_logDirectory.isEmpty()) {
            if (_logFile.size() > maxLogSizeC) {
                rotateLog();
            }
        }
    }
}

void Logger::open(const QString &name)
{
    bool openSucceeded = false;
    if (name == QLatin1Char('-')) {
        attacheToConsole();
        setLogFlush(true);
        openSucceeded = _logFile.open(stdout, QIODevice::WriteOnly);
    } else {
        _logFile.setFileName(name);
        openSucceeded = _logFile.open(QIODevice::WriteOnly);
    }

    if (!openSucceeded) {
        std::cerr << "Failed to open the log file" << std::endl;
        return;
    }
    _logstream.reset(new QTextStream(&_logFile));
    _logstream->setCodec("UTF-8");
    (*_logstream) << Theme::instance()->aboutVersions(Theme::VersionFormat::OneLiner) << Qt::endl;
}

void Logger::close()
{
    if (_logstream)
    {
        _logstream->flush();
        _logFile.close();
        _logstream.reset();
    }
}

void Logger::setLogFile(const QString &name)
{
    QMutexLocker locker(&_mutex);
    if (_logstream) {
        _logstream.reset(nullptr);
        _logFile.close();
    }

    if (name.isEmpty()) {
        return;
    }

    open(name);
}

void Logger::setLogExpire(std::chrono::hours expire)
{
    _logExpire = expire;
    rotateLog();
}

void Logger::setLogDir(const QString &dir)
{
    _logDirectory = dir;
    rotateLog();
}

void Logger::setLogFlush(bool flush)
{
    _doFileFlush = flush;
}

void Logger::setLogDebug(bool debug)
{
    const QSet<QString> rules = {QStringLiteral("sync.*.debug=true"), QStringLiteral("gui.*.debug=true")};
    if (debug) {
        addLogRule(rules);
    } else {
        removeLogRule(rules);
    }
    _logDebug = debug;
}

QString Logger::temporaryFolderLogDirPath() const
{
    return QDir::temp().filePath(QStringLiteral(APPLICATION_SHORTNAME "-logdir"));
}

void Logger::setupTemporaryFolderLogDir()
{
    auto dir = temporaryFolderLogDirPath();
    if (!QDir().mkpath(dir))
        return;
    setLogDebug(true);
    setLogDir(dir);
    _temporaryFolderLogDir = true;
}

void Logger::disableTemporaryFolderLogDir()
{
    if (!_temporaryFolderLogDir)
        return;
    setLogDir(QString());
    setLogDebug(false);
    setLogFile(QString());
    _temporaryFolderLogDir = false;
}

void Logger::setLogRules(const QSet<QString> &rules)
{
    static const QString defaultRule = qEnvironmentVariable("QT_LOGGING_RULES").replace(QLatin1Char(';'), QLatin1Char('\n'));
    _logRules = rules;
    QString tmp;
    QTextStream out(&tmp);
    for (const auto &p : rules) {
        out << p << QLatin1Char('\n');
    }
    out << defaultRule;
    qDebug() << tmp;
    QLoggingCategory::setFilterRules(tmp);
}

void Logger::dumpCrashLog()
{
    QFile logFile(QDir::tempPath() + QStringLiteral("/" APPLICATION_NAME "-crash.log"));
    if (logFile.open(QFile::WriteOnly)) {
        QTextStream out(&logFile);
        out.setCodec("UTF-8");
        for (int i = 1; i <= crashLogSizeC; ++i) {
            out << _crashLog[(_crashLogIndex + i) % crashLogSizeC];
        }
    }
}

static bool compressLog(const QString &originalName, const QString &targetName)
{
    QFile original(originalName);
    if (!original.open(QIODevice::ReadOnly))
        return false;
    auto compressed = gzopen(targetName.toUtf8().constData(), "wb");
    if (!compressed) {
        return false;
    }

    while (!original.atEnd()) {
        auto data = original.read(1024 * 1024);
        auto written = gzwrite(compressed, data.constData(), data.size());
        if (written != data.size()) {
            gzclose(compressed);
            return false;
        }
    }
    gzclose(compressed);
    return true;
}

void Logger::rotateLog()
{
    if (!_logDirectory.isEmpty()) {

        QDir dir(_logDirectory);
        if (!dir.exists()) {
            dir.mkpath(QStringLiteral("."));
        }

        // Tentative new log name, will be adjusted if one like this already exists
        const QString logName = dir.filePath(QStringLiteral(APPLICATION_SHORTNAME ".log"));
        QString previousLog;

        if (_logFile.isOpen()) {
            _logFile.close();
        }
        // rename previous log file if size != 0
        const auto info = QFileInfo(logName);
        if (info.exists(logName) && !info.size() == 0) {
            previousLog = dir.filePath(QStringLiteral(APPLICATION_SHORTNAME "-%1.log").arg(info.created().toString(QStringLiteral("MMdd_hh.mm.ss.zzz"))));
            if (!QFile(logName).rename(previousLog)) {
                std::cerr << "Failed to rename: " << qPrintable(logName) << " to " << qPrintable(previousLog) << std::endl;
            }
        }

        const auto now = QDateTime::currentDateTime();
        open(logName);
        // set the creation time to now
        _logFile.setFileTime(now, QFileDevice::FileTime::FileBirthTime);

        QtConcurrent::run([now, previousLog, dir, logExpire = _logExpire] {
            // Expire old log files and deal with conflicts
            const auto &files = dir.entryList(QStringList(QStringLiteral("*" APPLICATION_SHORTNAME "-*.log.gz")),
                QDir::Files, QDir::Name);
            for (const auto &s : files) {
                if (logExpire.count() > 0) {
                    std::chrono::seconds expireSeconds(logExpire);
                    QFileInfo fileInfo(dir.absoluteFilePath(s));
                    if (fileInfo.lastModified().addSecs(expireSeconds.count()) < now) {
                        QFile::remove(fileInfo.absoluteFilePath());
                    }
                }
            }
            // Compress the previous log file.
            if (!previousLog.isEmpty() && QFileInfo::exists(previousLog)) {
                QString compressedName = QStringLiteral("%1.gz").arg(previousLog);
                if (compressLog(previousLog, compressedName)) {
                    QFile::remove(previousLog);
                } else {
                    QFile::remove(compressedName);
                }
            }
        });
    }
}

void OCC::Logger::attacheToConsole()
{
    if (_consoleIsAttached) {
        return;
    }
    _consoleIsAttached = true;
#ifdef Q_OS_WIN
    if (!isDebuggerPresent() && AttachConsole(ATTACH_PARENT_PROCESS)) {
        // atache to the parent console output, if its an interactive terminal
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
    }
#endif
}

} // namespace OCC
