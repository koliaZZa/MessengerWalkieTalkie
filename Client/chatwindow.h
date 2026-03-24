#pragma once

#include <QHash>
#include <QStringList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;
class QTextEdit;
class NetworkClient;

struct ChatMessage {
    QString id;
    QString chatKey;
    QString author;
    QString text;
    QString status;
    bool outgoing = false;
};

class ChatWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ChatWindow(QWidget* parent = nullptr);

private slots:
    void connectToServer();
    void login();
    void registerUser();
    void logout();
    void createPrivateDialog();
    void sendMessage();
    void onAuthSucceeded(const QString& username);
    void onAuthFailed(const QString& message);
    void onUsersUpdated(const QStringList& users);
    void onUserLookupFinished(const QString& username, bool exists, bool online);
    void onPublicMessage(const QString& id, const QString& from, const QString& text);
    void onPrivateMessage(const QString& id, const QString& from, const QString& text);
    void onMessageDelivered(const QString& id);
    void onMessageRead(const QString& id, const QString& from);
    void onStatusChanged(const QString& message);
    void onTransportError(const QString& message);
    void onSocketDisconnected();
    void refreshCurrentChat();

private:
    void buildUi();
    void appendMessage(const ChatMessage& message);
    void clearSession();
    QString currentChatKey() const;
    void ensurePrivateDialog(const QString& username);
    void markCurrentPrivateMessagesRead();
    void rebuildUsersList(const QString& preferredChat = QString());
    void setStatusText(const QString& text);

    NetworkClient* m_client;

    QStackedWidget* m_pages;
    QWidget* m_loginPage;
    QWidget* m_chatPage;
    QLabel* m_loginStatusLabel;
    QLabel* m_chatStatusLabel;
    QLineEdit* m_hostEdit;
    QLineEdit* m_portEdit;
    QLineEdit* m_usernameEdit;
    QLineEdit* m_passwordEdit;
    QListWidget* m_usersList;
    QTextEdit* m_historyView;
    QLineEdit* m_messageEdit;
    QPushButton* m_newDialogButton;
    QPushButton* m_logoutButton;
    QPushButton* m_sendButton;

    QString m_username;
    QStringList m_onlineUsers;
    QStringList m_privateDialogs;
    QHash<QString, ChatMessage> m_messagesById;
    QHash<QString, QStringList> m_chatMessages;
};
