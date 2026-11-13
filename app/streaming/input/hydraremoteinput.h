#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>

class QJsonArray;
class QJsonObject;
class QNetworkAccessManager;
class QTimer;
class SdlInputHandler;

// HydraRemoteInput lets the cluster steer this head, so that an operator or an
// agent can drive an experience with no person in front of the screen. It is
// the macOS and Linux half of what hydraheadipad already does on an iPad
// (issue 1184, built on issues 1155 and 1159).
//
// It polls the cluster for a script and plays the script into the live stream.
// A gamepad step goes through SdlInputHandler, which merges it with whatever a
// physical pad is doing, so a script and a hand on a controller do not fight.
// A keyboard step goes straight to the host.
//
// This is NOT a credential channel. A script arrives as JSON from the cluster,
// which holds it in memory, and the sender prints it. Use it for menus, search
// boxes, chat and in-game text, never for a password.
//
// Configuration comes from the environment, never from the command line:
//
//   HYDRA_REMOTE_INPUT_URL       the cluster base address
//   HYDRA_REMOTE_INPUT_HEAD_ID   this head's identifier
//   HYDRA_REMOTE_INPUT_TOKEN     this head's bearer token
//
// A token on the command line is readable by every account on the machine
// through the process list. An environment variable is not, so hydraheadflatscreen
// passes all three that way and this class reads them here.
//
// When any of the three is missing, the poller does not start and the stream
// behaves exactly as it did before. That is the normal state for a head nobody
// is steering.
class HydraRemoteInput : public QObject
{
    Q_OBJECT

public:
    // Reads the three environment variables and returns a poller, or nullptr
    // when the head is not configured for remote input. The caller owns the
    // result and must call stop() before deleting it.
    static HydraRemoteInput* createFromEnvironment(SdlInputHandler* inputHandler);

    ~HydraRemoteInput();

    // Starts the poll on a thread of its own, so that a script holding a
    // button for a second never delays the video.
    void start();

    // Puts the virtual gamepad back to neutral, abandons anything still
    // running and stops the thread. Safe to call more than once, and called
    // for every way a stream can end.
    void stop();

private slots:
    void poll();
    void handleScriptReply();

private:
    HydraRemoteInput(SdlInputHandler* inputHandler,
                     const QString& clusterUrl,
                     const QString& headId,
                     const QByteArray& token);

    // Checks a whole script before any of it runs. A script that is half
    // applied and then refused leaves the experience in a state nobody asked
    // for, so the whole script is refused instead. The reason is logged. The
    // text a script types is never logged.
    bool validateScript(const QJsonArray& steps, QString& refusal) const;

    void runScript(const QJsonArray& steps);
    void runGamepadStep(const QJsonObject& step, int stepNumber, int stepCount);
    void typeText(const QString& text, int stepNumber, int stepCount);
    void pressKey(const QJsonObject& step, int holdMilliseconds, int stepNumber, int stepCount);

    // Sleeps in short slices so that stop() takes effect promptly and a long
    // hold never delays the teardown of a stream that has already ended.
    void sleepMilliseconds(int milliseconds);

    SdlInputHandler* m_InputHandler;
    QString m_ClusterUrl;
    QString m_HeadId;
    QByteArray m_Token;

    QThread m_Thread;
    QNetworkAccessManager* m_Network = nullptr;
    QTimer* m_PollTimer = nullptr;

    // Written by the thread that calls stop() and read on the poll thread.
    std::atomic<bool> m_Abandon { false };

    // True while a script is playing. A poll that lands during a script is
    // skipped rather than queued: the cluster holds one script at a time and
    // refuses a second, so there is never a backlog to catch up on.
    bool m_Running = false;
};
