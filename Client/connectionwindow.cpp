#include "connectionwindow.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

ConnectionWindow::ConnectionWindow(QWidget* parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("Messenger Walkie Talkie"));
    resize(420, 220);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(24, 24, 24, 24);
    rootLayout->setSpacing(16);

    auto* titleLabel = new QLabel(QStringLiteral("Server connection"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);

    m_endpointLabel = new QLabel(QStringLiteral("127.0.0.1:5555"), this);
    m_statusLabel = new QLabel(QStringLiteral("Starting..."), this);
    m_statusLabel->setWordWrap(true);

    auto* buttonLayout = new QHBoxLayout();
    m_settingsButton = new QPushButton(QStringLiteral("Settings"), this);
    m_offlineButton = new QPushButton(QStringLiteral("Offline mode"), this);
    m_offlineButton->setEnabled(false);
    buttonLayout->addWidget(m_settingsButton);
    buttonLayout->addWidget(m_offlineButton);
    buttonLayout->addStretch();

    rootLayout->addWidget(titleLabel);
    rootLayout->addWidget(m_endpointLabel);
    rootLayout->addWidget(m_statusLabel);
    rootLayout->addStretch();
    rootLayout->addLayout(buttonLayout);

    connect(m_settingsButton, &QPushButton::clicked, this, &ConnectionWindow::settingsRequested);
    connect(m_offlineButton, &QPushButton::clicked, this, &ConnectionWindow::offlineModeRequested);
}

void ConnectionWindow::setEndpoint(const QString& host, quint16 port)
{
    m_endpointLabel->setText(QStringLiteral("Server: %1:%2").arg(host).arg(port));
}

void ConnectionWindow::setStatusText(const QString& text)
{
    m_statusLabel->setText(text);
}

void ConnectionWindow::setOfflineModeAvailable(bool available)
{
    m_offlineButton->setEnabled(available);
}
