#include "loginwindow.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

LoginWindow::LoginWindow(QWidget* parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("Messenger Walkie Talkie"));
    resize(420, 300);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(24, 24, 24, 24);
    rootLayout->setSpacing(12);

    auto* titleLabel = new QLabel(QStringLiteral("Sign in"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);

    m_endpointLabel = new QLabel(QStringLiteral("Server: 127.0.0.1:5555"), this);
    m_statusLabel = new QLabel(QStringLiteral("Connected"), this);
    m_statusLabel->setWordWrap(true);

    m_usernameEdit = new QLineEdit(this);
    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);

    m_loginButton = new QPushButton(QStringLiteral("Login"), this);
    m_registerButton = new QPushButton(QStringLiteral("Register"), this);
    m_changeServerButton = new QPushButton(QStringLiteral("Change server"), this);

    rootLayout->addWidget(titleLabel);
    rootLayout->addWidget(m_endpointLabel);
    rootLayout->addSpacing(8);
    rootLayout->addWidget(new QLabel(QStringLiteral("Username"), this));
    rootLayout->addWidget(m_usernameEdit);
    rootLayout->addWidget(new QLabel(QStringLiteral("Password"), this));
    rootLayout->addWidget(m_passwordEdit);

    auto* actionsLayout = new QHBoxLayout();
    actionsLayout->addWidget(m_loginButton);
    actionsLayout->addWidget(m_registerButton);
    rootLayout->addLayout(actionsLayout);
    rootLayout->addWidget(m_changeServerButton);
    rootLayout->addWidget(m_statusLabel);
    rootLayout->addStretch();

    connect(m_loginButton, &QPushButton::clicked, this, [this] {
        emit loginRequested(m_usernameEdit->text().trimmed(), m_passwordEdit->text());
    });
    connect(m_registerButton, &QPushButton::clicked, this, [this] {
        emit registerRequested(m_usernameEdit->text().trimmed(), m_passwordEdit->text());
    });
    connect(m_changeServerButton, &QPushButton::clicked, this, &LoginWindow::changeServerRequested);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, [this] {
        emit loginRequested(m_usernameEdit->text().trimmed(), m_passwordEdit->text());
    });
}

void LoginWindow::setEndpoint(const QString& host, quint16 port)
{
    m_endpointLabel->setText(QStringLiteral("Server: %1:%2").arg(host).arg(port));
}

void LoginWindow::setStatusText(const QString& text)
{
    m_statusLabel->setText(text);
}

void LoginWindow::setUsername(const QString& username)
{
    m_usernameEdit->setText(username);
}

void LoginWindow::clearPassword()
{
    m_passwordEdit->clear();
}

void LoginWindow::setBusy(bool busy)
{
    m_usernameEdit->setEnabled(!busy);
    m_passwordEdit->setEnabled(!busy);
    m_loginButton->setEnabled(!busy);
    m_registerButton->setEnabled(!busy);
    m_changeServerButton->setEnabled(!busy);
}

QString LoginWindow::endpointText() const
{
    return m_endpointLabel->text();
}

QString LoginWindow::statusText() const
{
    return m_statusLabel->text();
}

QString LoginWindow::username() const
{
    return m_usernameEdit->text();
}

bool LoginWindow::isBusy() const
{
    return !m_loginButton->isEnabled();
}
