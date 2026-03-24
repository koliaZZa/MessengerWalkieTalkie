#pragma once

#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QThread>

enum class LogLevel {
    Info,
    Warning,
    Error
};

class Logger : public QObject
{
    Q_OBJECT

public:
    static Logger& instance();

    void start();
    void stop();
    void log(LogLevel level, const QString& message);

private:
    Logger();
    ~Logger() override;

    void writeNow(LogLevel level, const QString& message, const QDateTime& timestamp);
    void rotateIfNeeded();
    QString levelName(LogLevel level) const;

    QFile m_file;
    QThread m_thread;
    bool m_started {false};
};
