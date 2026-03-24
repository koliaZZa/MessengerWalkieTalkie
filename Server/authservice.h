#pragma once

#include <QObject>
#include <QSqlDatabase>

class AuthService : public QObject
{
    Q_OBJECT

public:
    explicit AuthService(QObject* parent = nullptr);
    ~AuthService() override;

    bool init(const QString& databasePath = QStringLiteral("users.db"));
    bool registerUser(const QString& username, const QString& password, QString& errorMessage);
    bool loginUser(const QString& username, const QString& password, QString& errorMessage) const;
    bool userExists(const QString& username) const;

private:
    QString hashPassword(const QString& password) const;
    bool isUsernameValid(const QString& username) const;

    QString m_connectionName;
    QSqlDatabase m_database;
};
