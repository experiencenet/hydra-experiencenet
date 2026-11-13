#include "hydraremoteinput.h"
#include "input.h"

#include <Limelight.h>
#include "SDL_compat.h"

#include <QJsonArray>
#include <QList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {

// The cluster holds one script per head and expires an uncollected one after
// thirty seconds. Three seconds is the interval hydraheadipad already uses for
// its command poll, so an operator waits about the same time on either head.
const int PollIntervalMilliseconds = 3000;

// A script may not pin the gamepad for an unreasonable time, and may not turn
// a keyboard into a file transfer. These match the limits the cluster enforces
// before it queues a script. They are repeated here because this layer must
// stand on its own if it is ever driven from somewhere else, and because a
// malformed script reaching the step loop should be refused rather than played.
const int MaximumSteps = 64;
const int MaximumStepMilliseconds = 10000;
const int MaximumTextBytesPerStep = 256;
const int MaximumTextBytesPerScript = 1024;
const int MaximumModifiersPerStep = 4;

const int SleepSliceMilliseconds = 25;

struct ButtonName {
    const char* name;
    int flag;
};

// The vocabulary matches the fourteen buttons the virtual gamepad declares to
// the host when it arrives, and matches the list the cluster validates against.
// Sending a button the pad never declared is not worth the risk, so the three
// lists are kept identical on purpose. If one grows, all three grow.
const ButtonName ButtonNames[] = {
    { "a", A_FLAG }, { "b", B_FLAG }, { "x", X_FLAG }, { "y", Y_FLAG },
    { "up", UP_FLAG }, { "down", DOWN_FLAG }, { "left", LEFT_FLAG }, { "right", RIGHT_FLAG },
    { "left_shoulder", LB_FLAG }, { "right_shoulder", RB_FLAG },
    { "start", PLAY_FLAG }, { "back", BACK_FLAG },
    { "left_stick_button", LS_CLK_FLAG }, { "right_stick_button", RS_CLK_FLAG },
};

struct KeyName {
    const char* name;
    short code;
};

// Key codes are Windows virtual key codes, which is what LiSendKeyboardEvent
// takes.
//
// The arrow keys are deliberately NOT named "up", "down", "left" and "right".
// Those four names already mean the d-pad in this vocabulary, and a shared
// name would quietly turn a keyboard step into a gamepad step.
const KeyName KeyNames[] = {
    { "enter", 0x0d }, { "escape", 0x1b }, { "backspace", 0x08 },
    { "tab", 0x09 }, { "space", 0x20 },
    { "delete", 0x2e }, { "insert", 0x2d },
    { "home", 0x24 }, { "end", 0x23 }, { "page_up", 0x21 }, { "page_down", 0x22 },
    { "arrow_up", 0x26 }, { "arrow_down", 0x28 }, { "arrow_left", 0x25 }, { "arrow_right", 0x27 },
    { "function_one", 0x70 }, { "function_two", 0x71 }, { "function_three", 0x72 },
    { "function_four", 0x73 }, { "function_five", 0x74 }, { "function_six", 0x75 },
    { "function_seven", 0x76 }, { "function_eight", 0x77 }, { "function_nine", 0x78 },
    { "function_ten", 0x79 }, { "function_eleven", 0x7a }, { "function_twelve", 0x7b },
};

// Returns the flag for a button name, or 0 when the virtual gamepad has no
// button by that name.
int flagForButtonName(const QString& name)
{
    for (const ButtonName& entry : ButtonNames) {
        if (name == QLatin1String(entry.name)) {
            return entry.flag;
        }
    }
    return 0;
}

// Returns the Windows virtual key code for a key name, or 0 when a script may
// not use that name. A letter or a digit is named by itself, so that a
// modifier combination such as control and a reads the way an operator says
// it. Only lowercase is accepted, because the cluster accepts only lowercase
// and the two layers must refuse the same scripts.
short keyCodeForName(const QString& name)
{
    for (const KeyName& entry : KeyNames) {
        if (name == QLatin1String(entry.name)) {
            return entry.code;
        }
    }
    if (name.length() == 1) {
        const QChar character = name.at(0);
        if (character >= QLatin1Char('a') && character <= QLatin1Char('z')) {
            return (short)(character.unicode() - 'a' + 0x41);
        }
        if (character >= QLatin1Char('0') && character <= QLatin1Char('9')) {
            return (short)(character.unicode() - '0' + 0x30);
        }
    }
    return 0;
}

// Returns the modifier bit for a modifier name, or 0 when the name is unknown.
char modifierBitForName(const QString& name)
{
    if (name == QLatin1String("shift")) { return MODIFIER_SHIFT; }
    if (name == QLatin1String("control")) { return MODIFIER_CTRL; }
    if (name == QLatin1String("alt")) { return MODIFIER_ALT; }
    if (name == QLatin1String("meta")) { return MODIFIER_META; }
    return 0;
}

// Returns the key code of the modifier key itself, so that it can be held down
// around the key the script asked for. A host that only watches the modifier
// keys would miss a bare modifier byte, so both are sent.
short modifierKeyCodeForName(const QString& name)
{
    if (name == QLatin1String("shift")) { return 0x10; }
    if (name == QLatin1String("control")) { return 0x11; }
    if (name == QLatin1String("alt")) { return 0x12; }
    if (name == QLatin1String("meta")) { return 0x5b; }
    return 0;
}

short readAxis(const QJsonObject& step, const char* key)
{
    const int raw = step.value(QLatin1String(key)).toInt(0);
    return (short)qBound(-32768, raw, 32767);
}

unsigned char readTrigger(const QJsonObject& step, const char* key)
{
    const int raw = step.value(QLatin1String(key)).toInt(0);
    return (unsigned char)qBound(0, raw, 255);
}

}

HydraRemoteInput* HydraRemoteInput::createFromEnvironment(SdlInputHandler* inputHandler)
{
    const QString clusterUrl = qEnvironmentVariable("HYDRA_REMOTE_INPUT_URL").trimmed();
    const QString headId = qEnvironmentVariable("HYDRA_REMOTE_INPUT_HEAD_ID").trimmed();
    const QByteArray token = qgetenv("HYDRA_REMOTE_INPUT_TOKEN").trimmed();

    if (clusterUrl.isEmpty() || headId.isEmpty() || token.isEmpty()) {
        // This is the normal state for a head nobody is steering, so it is not
        // a warning. The three names are logged so that a half configured head
        // is easy to spot, and the token value itself is never logged.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Remote input is off: url %s, head id %s, token %s",
                    clusterUrl.isEmpty() ? "missing" : "set",
                    headId.isEmpty() ? "missing" : "set",
                    token.isEmpty() ? "missing" : "set");
        return nullptr;
    }

    return new HydraRemoteInput(inputHandler, clusterUrl, headId, token);
}

HydraRemoteInput::HydraRemoteInput(SdlInputHandler* inputHandler,
                                   const QString& clusterUrl,
                                   const QString& headId,
                                   const QByteArray& token)
    : m_InputHandler(inputHandler),
      m_ClusterUrl(clusterUrl),
      m_HeadId(headId),
      m_Token(token)
{
    // Trailing separators would produce a double separator in the request path,
    // which some proxies answer with a redirect this client does not follow.
    while (m_ClusterUrl.endsWith(QLatin1Char('/'))) {
        m_ClusterUrl.chop(1);
    }
}

HydraRemoteInput::~HydraRemoteInput()
{
    stop();
}

void HydraRemoteInput::start()
{
    if (m_Thread.isRunning()) {
        return;
    }

    moveToThread(&m_Thread);

    // The network objects belong to the poll thread, so they are created there
    // rather than here. Creating a QNetworkAccessManager on one thread and
    // using it on another does not work.
    connect(&m_Thread, &QThread::started, this, [this]() {
        m_Network = new QNetworkAccessManager(this);
        m_PollTimer = new QTimer(this);
        m_PollTimer->setInterval(PollIntervalMilliseconds);
        connect(m_PollTimer, &QTimer::timeout, this, &HydraRemoteInput::poll);
        m_PollTimer->start();

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Remote input is on for head %s, polling %s every %d ms",
                    qPrintable(m_HeadId), qPrintable(m_ClusterUrl), PollIntervalMilliseconds);
    });

    // QThread::finished is emitted from the thread itself, so a direct
    // connection runs this there. That is the one moment when the event loop
    // has stopped and the thread has not yet gone, which is where objects
    // created on it must be freed.
    connect(&m_Thread, &QThread::finished, this, [this]() {
        delete m_PollTimer;
        m_PollTimer = nullptr;
        delete m_Network;
        m_Network = nullptr;
    }, Qt::DirectConnection);

    m_Thread.start();
}

void HydraRemoteInput::stop()
{
    // Set this first. A script already playing checks it between slices and
    // returns within one slice, so the thread is free to quit.
    m_Abandon = true;

    if (m_InputHandler != nullptr) {
        // Release before the thread goes away. A button left held on the host
        // is worse than a late release: nothing else will ever lift it.
        m_InputHandler->releaseVirtualGamepad();
    }

    if (m_Thread.isRunning()) {
        m_Thread.quit();
        if (!m_Thread.wait(5000)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Remote input thread did not stop in time");
        }
    }

    m_InputHandler = nullptr;
}

void HydraRemoteInput::poll()
{
    if (m_Running || m_Abandon) {
        return;
    }

    QNetworkRequest request(QUrl(QString("%1/api/v1/heads/%2/input").arg(m_ClusterUrl, m_HeadId)));
    request.setRawHeader("Authorization", "Bearer " + m_Token);
    request.setRawHeader("Accept", "application/json");

    QNetworkReply* reply = m_Network->get(request);
    connect(reply, &QNetworkReply::finished, this, &HydraRemoteInput::handleScriptReply);
}

void HydraRemoteInput::handleScriptReply()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (reply == nullptr) {
        return;
    }
    reply->deleteLater();

    if (m_Abandon) {
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        // A head streams for hours, and a poll that fails every three seconds
        // would fill the log with one line per failure. The reason is logged
        // at debug level, and the stream is unaffected either way.
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                     "Remote input poll failed: %s", qPrintable(reply->errorString()));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Remote input poll returned something that is not a script");
        return;
    }

    const QJsonArray steps = document.object().value(QLatin1String("steps")).toArray();
    if (steps.isEmpty()) {
        // Nothing queued. This is the answer to almost every poll.
        return;
    }

    QString refusal;
    if (!validateScript(steps, refusal)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Refusing the script: %s", qPrintable(refusal));
        return;
    }

    m_Running = true;
    runScript(steps);
    m_Running = false;
}

bool HydraRemoteInput::validateScript(const QJsonArray& steps, QString& refusal) const
{
    if (steps.count() > MaximumSteps) {
        refusal = QString("it holds %1 steps and the limit is %2")
                      .arg(steps.count()).arg(MaximumSteps);
        return false;
    }

    int totalTextBytes = 0;
    for (const QJsonValue& value : steps) {
        if (!value.isObject()) {
            refusal = QLatin1String("a step is not an object");
            return false;
        }
        const QJsonObject step = value.toObject();

        if (step.contains(QLatin1String("buttons"))) {
            const QJsonValue buttons = step.value(QLatin1String("buttons"));
            if (!buttons.isArray()) {
                refusal = QLatin1String("buttons is not a list");
                return false;
            }
            for (const QJsonValue& name : buttons.toArray()) {
                if (!name.isString() || flagForButtonName(name.toString()) == 0) {
                    refusal = QString("the virtual gamepad has no button named %1")
                                  .arg(name.toVariant().toString());
                    return false;
                }
            }
        }

        const bool hasText = step.contains(QLatin1String("text"));
        const bool hasKey = step.contains(QLatin1String("key"));

        if (hasText) {
            const QJsonValue text = step.value(QLatin1String("text"));
            if (!text.isString()) {
                refusal = QLatin1String("text is not a string");
                return false;
            }
            const int bytes = text.toString().toUtf8().size();
            if (bytes > MaximumTextBytesPerStep) {
                refusal = QString("one step holds %1 bytes of text and the limit is %2")
                              .arg(bytes).arg(MaximumTextBytesPerStep);
                return false;
            }
            totalTextBytes += bytes;
        }

        if (hasKey) {
            const QJsonValue key = step.value(QLatin1String("key"));
            if (!key.isString()) {
                refusal = QLatin1String("key is not a string");
                return false;
            }
            if (keyCodeForName(key.toString()) == 0) {
                refusal = QString("there is no key named %1 (an arrow key is arrow_up, "
                                  "arrow_down, arrow_left or arrow_right)")
                              .arg(key.toString());
                return false;
            }
        }

        if (hasText && hasKey) {
            refusal = QLatin1String("a step sets both text and key, and one hold "
                                    "cannot mean two things");
            return false;
        }

        if (step.contains(QLatin1String("modifiers"))) {
            const QJsonValue modifiers = step.value(QLatin1String("modifiers"));
            if (!modifiers.isArray()) {
                refusal = QLatin1String("modifiers is not a list");
                return false;
            }
            if (!hasKey) {
                refusal = QLatin1String("a step sets modifiers and no key, and a "
                                        "modifier needs a key to hold");
                return false;
            }
            if (modifiers.toArray().count() > MaximumModifiersPerStep) {
                refusal = QString("a step holds more than %1 modifiers")
                              .arg(MaximumModifiersPerStep);
                return false;
            }
            for (const QJsonValue& name : modifiers.toArray()) {
                if (!name.isString() || modifierBitForName(name.toString()) == 0) {
                    refusal = QString("there is no modifier named %1, the modifiers are "
                                      "shift, control, alt and meta")
                                  .arg(name.toVariant().toString());
                    return false;
                }
            }
        }
    }

    if (totalTextBytes > MaximumTextBytesPerScript) {
        refusal = QString("it holds %1 bytes of text and the limit for one script is %2")
                      .arg(totalTextBytes).arg(MaximumTextBytesPerScript);
        return false;
    }

    return true;
}

void HydraRemoteInput::runScript(const QJsonArray& steps)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Running a remote input script of %d steps", (int)steps.count());

    if (!m_InputHandler->acquireVirtualGamepad()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "No virtual gamepad is available, the script is not run");
        return;
    }

    const int stepCount = steps.count();
    int stepNumber = 0;

    for (const QJsonValue& value : steps) {
        stepNumber++;

        if (m_Abandon) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Remote input script abandoned at step %d of %d", stepNumber, stepCount);
            m_InputHandler->releaseVirtualGamepad();
            return;
        }

        const QJsonObject step = value.toObject();
        const int waitMilliseconds = step.value(QLatin1String("wait_milliseconds")).toInt(0);
        const int holdMilliseconds = step.value(QLatin1String("hold_milliseconds")).toInt(0);

        if (waitMilliseconds > 0) {
            m_InputHandler->setVirtualGamepadNeutral();
            sleepMilliseconds(waitMilliseconds);
            continue;
        }
        if (holdMilliseconds <= 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Remote input step %d has no duration, skipping it", stepNumber);
            continue;
        }

        const QString text = step.value(QLatin1String("text")).toString();
        if (!text.isEmpty()) {
            typeText(text, stepNumber, stepCount);
            sleepMilliseconds(holdMilliseconds);
            continue;
        }

        const QString key = step.value(QLatin1String("key")).toString();
        if (!key.isEmpty()) {
            pressKey(step, holdMilliseconds, stepNumber, stepCount);
            continue;
        }

        runGamepadStep(step, stepNumber, stepCount);
        sleepMilliseconds(holdMilliseconds);
        m_InputHandler->setVirtualGamepadNeutral();
    }

    m_InputHandler->setVirtualGamepadNeutral();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Remote input script finished");
}

void HydraRemoteInput::runGamepadStep(const QJsonObject& step, int stepNumber, int stepCount)
{
    int buttonFlags = 0;
    for (const QJsonValue& name : step.value(QLatin1String("buttons")).toArray()) {
        buttonFlags |= flagForButtonName(name.toString());
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "Remote input gamepad step %d of %d, buttons 0x%x",
                 stepNumber, stepCount, buttonFlags);

    m_InputHandler->setVirtualGamepadState(buttonFlags,
                                           readTrigger(step, "left_trigger"),
                                           readTrigger(step, "right_trigger"),
                                           readAxis(step, "left_stick_x"),
                                           readAxis(step, "left_stick_y"),
                                           readAxis(step, "right_stick_x"),
                                           readAxis(step, "right_stick_y"));
}

void HydraRemoteInput::typeText(const QString& text, int stepNumber, int stepCount)
{
    const QByteArray utf8Text = text.toUtf8();

    // The text itself is never logged. It is operator input that travels in
    // the clear through the cluster, and a log that repeats it would be one
    // more place it lands.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Remote input is typing %d bytes of text at step %d of %d",
                utf8Text.size(), stepNumber, stepCount);

    LiSendUtf8TextEvent(utf8Text.constData(), (unsigned int)utf8Text.size());
}

void HydraRemoteInput::pressKey(const QJsonObject& step, int holdMilliseconds,
                                int stepNumber, int stepCount)
{
    const QString keyName = step.value(QLatin1String("key")).toString();
    const short keyCode = keyCodeForName(keyName);
    if (keyCode == 0) {
        return;
    }

    char modifierByte = 0;
    QList<short> modifierKeyCodes;
    for (const QJsonValue& name : step.value(QLatin1String("modifiers")).toArray()) {
        const char bit = modifierBitForName(name.toString());
        const short modifierKeyCode = modifierKeyCodeForName(name.toString());
        if (bit == 0 || modifierKeyCode == 0) {
            continue;
        }
        modifierByte |= bit;
        modifierKeyCodes.append(modifierKeyCode);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Remote input is pressing key %s with %d modifier(s) at step %d of %d",
                qPrintable(keyName), modifierKeyCodes.count(), stepNumber, stepCount);

    for (short modifierKeyCode : modifierKeyCodes) {
        LiSendKeyboardEvent(modifierKeyCode, KEY_ACTION_DOWN, modifierByte);
    }
    LiSendKeyboardEvent(keyCode, KEY_ACTION_DOWN, modifierByte);

    sleepMilliseconds(holdMilliseconds);

    // The release always runs, even when the hold was cut short by stop() or
    // by the stream ending. A key left down on the host is worse than a late
    // release: nothing else will ever lift it.
    LiSendKeyboardEvent(keyCode, KEY_ACTION_UP, modifierByte);
    for (int i = modifierKeyCodes.count() - 1; i >= 0; i--) {
        LiSendKeyboardEvent(modifierKeyCodes.at(i), KEY_ACTION_UP, modifierByte);
    }
}

void HydraRemoteInput::sleepMilliseconds(int milliseconds)
{
    if (milliseconds > MaximumStepMilliseconds) {
        milliseconds = MaximumStepMilliseconds;
    }

    int slept = 0;
    while (slept < milliseconds) {
        if (m_Abandon) {
            return;
        }
        const int slice = qMin(SleepSliceMilliseconds, milliseconds - slept);
        QThread::msleep((unsigned long)slice);
        slept += slice;
    }
}
