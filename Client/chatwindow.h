#pragma once

#include <QJsonArray>
#include <QWidget>

#include "chatstate.h"
#include "historystore.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextEdit;
class NetworkClient;

class ChatWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ChatWindow(HistoryStore* historyStore, NetworkClient* client, QWidget* parent = nullptr);

    void activateSession(const QString& username, bool offlineMode);
    void deactivateSession();
    void setStatusText(const QString& text);
    QString statusText() const;
    QString sessionUsername() const;
    bool isOfflineMode() const;

signals:
    void logoutRequested();
    void changeServerRequested();

private slots:
    void createPrivateDialog();
    void sendMessage();
    void onUsersUpdated(const QStringList& users);
    void onDialogsReceived(const QStringList& dialogs);
    void onHistoryReceived(const QString& chatUser, const QJsonArray& items);
    void onUserLookupFinished(const QString& username, bool exists, bool online);
    void onPublicMessage(const QString& id, const QString& from, const QString& text, qint64 createdAt);
    void onPrivateMessage(const QString& id, const QString& from, const QString& text, qint64 createdAt);
    void onMessageDelivered(const QString& id, qint64 createdAt);
    void onMessageRead(const QString& id, const QString& from);
    void onStatusChanged(const QString& message);
    void onTransportError(const QString& message);
    void refreshCurrentChat();

private:
    void buildUi();
    void loadLocalHistory(const QString& username);
    void appendMessage(const ChatMessage& message, bool persist = true);
    void clearLoadedHistory();
    QString currentChatKey() const;
    void markCurrentPrivateMessagesRead();
    void rebuildDialogList(const QString& preferredChat = QString());
    void rebuildOnlineUsersList();
    void requestCurrentHistory();
    void updateUiState();

    HistoryStore* m_historyStore;
    NetworkClient* m_client;
    bool m_offlineMode {false};
    bool m_onlineSession {false};

    QLabel* m_statusLabel;
    QListWidget* m_dialogsList;
    QListWidget* m_onlineUsersList;
    QTextEdit* m_historyView;
    QLineEdit* m_messageEdit;
    QPushButton* m_newDialogButton;
    QPushButton* m_changeServerButton;
    QPushButton* m_logoutButton;
    QPushButton* m_sendButton;

    ChatSessionState m_state;
};
