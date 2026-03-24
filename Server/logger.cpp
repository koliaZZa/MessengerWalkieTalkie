#include "logger.h"

#include <QDir>
#include <QMetaObject>
#include <QTextStream>

Logger& Logger::instance()
{
    static Logger logger;
    return logger;
}

Logger::Logger()
{
    moveToThread(&m_thread);
}

Logger::~Logger()
{
    stop();
}

void Logger::start()
{
    if (m_started) {
        return;
    }

    QDir().mkpath("logs");
    m_file.setFileName("logs/server.log");
    m_file.open(QIODevice::Append | QIODevice::Text);

    m_started = true;
    m_thread.start();
}

void Logger::stop()
{
    if (!m_started) {
        return;
    }

    m_started = false;
    m_thread.quit();
    m_thread.wait();

    if (m_file.isOpen()) {
        m_file.close();
    }
}

void Logger::log(LogLevel level, const QString& message)
{
    if (!m_started) {
        start();
    }

    const QDateTime timestamp = QDateTime::currentDateTime();
    QMetaObject::invokeMethod(this,
                              [this, level, message, timestamp]() {
                                  writeNow(level, message, timestamp);
                              },
                              Qt::QueuedConnection);
}

void Logger::writeNow(LogLevel level, const QString& message, const QDateTime& timestamp)
{
    if (!m_started || !m_file.isOpen()) {
        return;
    }

    rotateIfNeeded();

    QTextStream stream(&m_file);
    stream << '[' << timestamp.toString("yyyy-MM-dd hh:mm:ss") << "] ["
           << levelName(level) << "] " << message << '\n';
    m_file.flush();
}

void Logger::rotateIfNeeded()
{
    constexpr qint64 kMaxBytes = 5 * 1024 * 1024;
    if (m_file.size() < kMaxBytes) {
        return;
    }

    m_file.close();

    const QString rotatedName = QStringLiteral("logs/server_%1.log")
                                    .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    QFile::rename("logs/server.log", rotatedName);

    m_file.setFileName("logs/server.log");
    m_file.open(QIODevice::Append | QIODevice::Text);
}

QString Logger::levelName(LogLevel level) const
{
    switch (level) {
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARNING";
    case LogLevel::Error:
        return "ERROR";
    }

    return "UNKNOWN";
}
