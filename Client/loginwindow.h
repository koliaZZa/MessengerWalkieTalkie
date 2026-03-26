#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

class LoginWindow : public QWidget
{
    Q_OBJECT

public:
    explicit LoginWindow(QWidget* parent = nullptr);

    void setEndpoint(const QString& host, quint16 port);
    void setStatusText(const QString& text);
    void setUsername(const QString& username);
    void setPassword(const QString& password);
    void clearPassword();
    void setBusy(bool busy);

signals:
    void loginRequested(const QString& username, const QString& password);
    void registerRequested(const QString& username, const QString& password);
    void changeServerRequested();

private:
    QLabel* m_endpointLabel;
    QLabel* m_statusLabel;
    QLineEdit* m_usernameEdit;
    QLineEdit* m_passwordEdit;
    QPushButton* m_loginButton;
    QPushButton* m_registerButton;
    QPushButton* m_changeServerButton;
};
