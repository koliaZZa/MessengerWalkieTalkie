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
    if (m_thread.isRunning()) {
        m_thread.quit();
        m_thread.wait();
    }
}

QThread* Worker::threadHandle()
{
    return &m_thread;
}

void Worker::attachSocket(QTcpSocket* socket)
{
    QMetaObject::invokeMethod(this,
                              [this, socket]() {
                                  auto* connection = new Connection(socket, this);
                                  emit connectionReady(connection);
                                  connection->start();
                              },
                              Qt::QueuedConnection);
}

void Worker::shutdown(QThread* targetThread)
{
    if (!targetThread) {
        targetThread = QThread::currentThread();
    }

    QMetaObject::invokeMethod(this,
                              [this, targetThread]() {
                                  const auto connections = findChildren<Connection*>(QString(), Qt::FindDirectChildrenOnly);
                                  for (auto connection : connections) {
                                      connection->closeConnection();
                                      delete connection;
                                  }
                                  moveToThread(targetThread);
                              },
                              Qt::BlockingQueuedConnection);

    if (m_thread.isRunning()) {
        m_thread.quit();
        m_thread.wait();
    }
}

