#include "worker.h"

#include "connection.h"

#include <QMetaObject>
#include <QTcpSocket>

Worker::Worker(QObject* parent)
    : QObject(parent)
{
    moveToThread(&m_thread);
    m_thread.start();
}

Worker::~Worker()
{
    m_thread.quit();
    m_thread.wait();
}

QThread* Worker::threadHandle()
{
    return &m_thread;
}

void Worker::attachSocket(QTcpSocket* socket)
{
    QMetaObject::invokeMethod(this,
                              [this, socket]() {
                                  auto* connection = new Connection(socket);
                                  connection->moveToThread(&m_thread);
                                  emit connectionReady(connection);
                                  connection->start();
                              },
                              Qt::QueuedConnection);
}
