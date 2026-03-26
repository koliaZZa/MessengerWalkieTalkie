#pragma once

#include <QWidget>

class QLabel;
class QPushButton;

class ConnectionWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ConnectionWindow(QWidget* parent = nullptr);

    void setEndpoint(const QString& host, quint16 port);
    void setStatusText(const QString& text);
    void setOfflineModeAvailable(bool available);

signals:
    void settingsRequested();
    void offlineModeRequested();

private:
    QLabel* m_endpointLabel;
    QLabel* m_statusLabel;
    QPushButton* m_settingsButton;
    QPushButton* m_offlineButton;
};
