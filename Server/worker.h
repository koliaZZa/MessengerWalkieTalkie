#pragma once

#include <QObject>
#include <QThread>
#include <QTcpSocket>

class Connection;

class Worker : public QObject
{
    Q_OBJECT

public:
    explicit Worker(QObject* parent = nullptr);
    ~Worker() override;

    QThread* threadHandle();
    void attachSocket(QTcpSocket* socket);

signals:
    void connectionReady(Connection* connection);

private:
    QThread m_thread;
};
