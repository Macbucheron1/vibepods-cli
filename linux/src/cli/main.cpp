#include <QCoreApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QtDBus/QtDBus>

#include <csignal>
#include <unistd.h>

#include <librepods/core/airpods_core_client.h>
#include <librepods/enums.h>

Q_LOGGING_CATEGORY(vibepods, "vibepods")

using namespace AirpodsTrayApp::Enums;
using ManagedObjectList = QMap<QDBusObjectPath, QMap<QString, QVariantMap>>;
Q_DECLARE_METATYPE(ManagedObjectList)

namespace
{
constexpr const char *kAirPodsAacpUuid = "74ec2172-0bad-4d01-8f77-997b2be0722a";

QString normalizeMac(QString mac)
{
    return mac.trimmed().toUpper().replace('-', ':');
}

bool isValidMac(const QString &mac)
{
    static const QRegularExpression re("^([0-9A-F]{2}:){5}[0-9A-F]{2}$");
    return re.match(mac).hasMatch();
}

struct BluezAirPodsDevice
{
    QString address;
    QString name;
    bool paired = false;
    bool connected = false;
};

QList<BluezAirPodsDevice> queryBluezAirPods();

bool isBluezAirPodsConnected(const QString &mac)
{
    const auto devices = queryBluezAirPods();
    for (const auto &d : devices)
    {
        if (d.address.compare(mac, Qt::CaseInsensitive) == 0)
        {
            return d.connected;
        }
    }
    return false;
}

bool runCommand(const QString &program, const QStringList &args, QString &stdoutOut, QString &stderrOut, int timeoutMs = 15000)
{
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted(timeoutMs))
    {
        stderrOut = QString("failed to start %1").arg(program);
        return false;
    }
    if (!p.waitForFinished(timeoutMs))
    {
        p.kill();
        stderrOut = QString("%1 timed out").arg(program);
        return false;
    }
    stdoutOut = QString::fromUtf8(p.readAllStandardOutput());
    stderrOut = QString::fromUtf8(p.readAllStandardError());
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

bool connectViaBluetoothctl(const QString &mac, QString &detail)
{
    QString out;
    QString err;
    if (!runCommand("bluetoothctl", {"connect", mac}, out, err))
    {
        detail = err.trimmed();
        return false;
    }
    detail = out.trimmed();
    return true;
}

bool disconnectViaBluetoothctl(const QString &mac, QString &detail)
{
    QString out;
    QString err;
    if (!runCommand("bluetoothctl", {"disconnect", mac}, out, err))
    {
        detail = err.trimmed();
        return false;
    }
    detail = out.trimmed();
    return true;
}

bool tryRouteAudioToAirPods(const QString &mac, QString &detail)
{
    const QString macUnderscore = mac.toLower().replace(':', '_');

    // Best effort: force A2DP profile first (common issue on PipeWire/Pulse compatibility layer).
    QString cardsOut;
    QString cardsErr;
    if (runCommand("pactl", {"list", "short", "cards"}, cardsOut, cardsErr, 5000))
    {
        QString cardName;
        const QStringList lines = cardsOut.split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines)
        {
            const QString lower = line.toLower();
            if (!lower.contains("bluez_card.") || !lower.contains(macUnderscore))
            {
                continue;
            }
            const QStringList cols = line.split('\t');
            if (cols.size() >= 2)
            {
                cardName = cols.at(1).trimmed();
                break;
            }
        }

        if (!cardName.isEmpty())
        {
            QString pOut;
            QString pErr;
            runCommand("pactl", {"set-card-profile", cardName, "a2dp-sink"}, pOut, pErr, 5000);
        }
    }

    // Wait for sink creation after profile switch (can take a while on PipeWire/BlueZ).
    QElapsedTimer timer;
    timer.start();

    QString out;
    QString err;
    bool gotSinks = false;
    while (timer.elapsed() < 20000)
    {
        if (!runCommand("pactl", {"list", "short", "sinks"}, out, err, 5000))
        {
            break;
        }
        if (out.toLower().contains(macUnderscore))
        {
            gotSinks = true;
            break;
        }
        QThread::msleep(400);
    }

    if (!gotSinks)
    {
        detail = "no AirPods sink appeared (profile may still be HFP/HSP)";
        return false;
    }

    QString sinkName;
    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines)
    {
        const QString lower = line.toLower();
        if (!lower.contains(macUnderscore))
        {
            continue;
        }
        const QStringList cols = line.split('\t');
        if (cols.size() >= 2)
        {
            sinkName = cols.at(1).trimmed();
            break;
        }
    }

    if (sinkName.isEmpty())
    {
        detail = "no AirPods sink found in pactl list";
        return false;
    }

    QString setOut;
    QString setErr;
    runCommand("pactl", {"set-default-sink", sinkName}, setOut, setErr, 5000);
    runCommand("pactl", {"set-sink-mute", sinkName, "0"}, setOut, setErr, 5000);

    QString inputsOut;
    QString inputsErr;
    if (runCommand("pactl", {"list", "short", "sink-inputs"}, inputsOut, inputsErr, 5000))
    {
        const QStringList inputLines = inputsOut.split('\n', Qt::SkipEmptyParts);
        for (const QString &line : inputLines)
        {
            const QString id = line.section('\t', 0, 0).trimmed();
            if (!id.isEmpty())
            {
                QString moveOut;
                QString moveErr;
                runCommand("pactl", {"move-sink-input", id, sinkName}, moveOut, moveErr, 5000);
            }
        }
    }

    detail = QString("audio routed to %1 (default sink + active streams moved)").arg(sinkName);
    return true;
}

QList<BluezAirPodsDevice> queryBluezAirPods()
{
    QList<BluezAirPodsDevice> devices;
    auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected())
    {
        return devices;
    }

    qDBusRegisterMetaType<QDBusObjectPath>();
    qDBusRegisterMetaType<ManagedObjectList>();

    QDBusInterface objectManager("org.bluez", "/", "org.freedesktop.DBus.ObjectManager", bus);
    QDBusMessage reply = objectManager.call("GetManagedObjects");
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty())
    {
        return devices;
    }

    QDBusArgument arg = reply.arguments().constFirst().value<QDBusArgument>();
    ManagedObjectList managedObjects;
    arg >> managedObjects;

    for (auto it = managedObjects.constBegin(); it != managedObjects.constEnd(); ++it)
    {
        const auto &interfaces = it.value();
        if (!interfaces.contains("org.bluez.Device1"))
        {
            continue;
        }

        const QVariantMap props = interfaces.value("org.bluez.Device1");
        const QStringList uuids = props.value("UUIDs").toStringList();
        if (!uuids.contains(kAirPodsAacpUuid))
        {
            continue;
        }

        BluezAirPodsDevice d;
        d.address = props.value("Address").toString();
        d.name = props.value("Name").toString();
        d.paired = props.value("Paired").toBool();
        d.connected = props.value("Connected").toBool();
        devices.push_back(d);
    }
    return devices;
}

QString toNoiseControlString(NoiseControlMode mode)
{
    switch (mode)
    {
    case NoiseControlMode::Off:
        return "off";
    case NoiseControlMode::NoiseCancellation:
        return "noise-cancellation";
    case NoiseControlMode::Transparency:
        return "transparency";
    case NoiseControlMode::Adaptive:
        return "adaptive";
    default:
        return "unknown";
    }
}

#ifndef VIBEPODS_BUILD_DAEMON

bool parseModeValue(const QString &raw, NoiseControlMode &mode)
{
    const QString value = raw.trimmed().toLower();
    if (value == "off")
    {
        mode = NoiseControlMode::Off;
        return true;
    }
    if (value == "nc")
    {
        mode = NoiseControlMode::NoiseCancellation;
        return true;
    }
    if (value == "transparency")
    {
        mode = NoiseControlMode::Transparency;
        return true;
    }
    if (value == "adaptive" || value == "adaptative")
    {
        mode = NoiseControlMode::Adaptive;
        return true;
    }
    return false;
}

bool parseOnOffValue(const QString &raw, bool &enabled)
{
    const QString value = raw.trimmed().toLower();
    if (value == "on")
    {
        enabled = true;
        return true;
    }
    if (value == "off")
    {
        enabled = false;
        return true;
    }
    return false;
}

QString toModelString(AirPodsModel model)
{
    switch (model)
    {
    case AirPodsModel::AirPods1: return "AirPods 1";
    case AirPodsModel::AirPods2: return "AirPods 2";
    case AirPodsModel::AirPods3: return "AirPods 3";
    case AirPodsModel::AirPods4: return "AirPods 4";
    case AirPodsModel::AirPods4ANC: return "AirPods 4 ANC";
    case AirPodsModel::AirPodsPro: return "AirPods Pro";
    case AirPodsModel::AirPodsPro2Lightning: return "AirPods Pro 2 Lightning";
    case AirPodsModel::AirPodsPro2USBC: return "AirPods Pro 2 USB-C";
    case AirPodsModel::AirPodsMaxLightning: return "AirPods Max Lightning";
    case AirPodsModel::AirPodsMaxUSBC: return "AirPods Max USB-C";
    default: return "Unknown";
    }
}

void printStatus(const AirPodsCoreClient &client, bool asJson)
{
    const auto &state = client.state();
    const auto &battery = client.battery();
    const auto &ears = client.earDetection();

    const auto left = battery.getState(Battery::Component::Left);
    const auto right = battery.getState(Battery::Component::Right);
    const auto caze = battery.getState(Battery::Component::Case);
    const auto formatBattery = [](const Battery::BatteryState &s) -> QString
    {
        if (s.status == Battery::BatteryStatus::Disconnected)
        {
            return "n/a";
        }
        return QString::number(static_cast<int>(s.level));
    };

    if (asJson)
    {
        QJsonObject obj;
        obj.insert("connected", true);
        obj.insert("address", state.bluetoothAddress);
        obj.insert("name", state.deviceName.isEmpty() ? "unknown" : state.deviceName);
        obj.insert("model", toModelString(state.model));
        obj.insert("noise_control", toNoiseControlString(state.noiseControlMode));
        obj.insert("conversational_awareness", state.conversationalAwareness);
        obj.insert("hearing_aid", state.hearingAidEnabled);
        obj.insert("left_battery", left.status == Battery::BatteryStatus::Disconnected ? QJsonValue::Null : QJsonValue(static_cast<int>(left.level)));
        obj.insert("right_battery", right.status == Battery::BatteryStatus::Disconnected ? QJsonValue::Null : QJsonValue(static_cast<int>(right.level)));
        obj.insert("case_battery", caze.status == Battery::BatteryStatus::Disconnected ? QJsonValue::Null : QJsonValue(static_cast<int>(caze.level)));
        obj.insert("left_in_ear", ears.isPrimaryInEar());
        obj.insert("right_in_ear", ears.isSecondaryInEar());
        QTextStream(stdout) << QJsonDocument(obj).toJson(QJsonDocument::Compact) << "\n";
        return;
    }

    QTextStream out(stdout);
    out << "connected: yes\n";
    out << "address: " << state.bluetoothAddress << "\n";
    out << "name: " << (state.deviceName.isEmpty() ? "unknown" : state.deviceName) << "\n";
    out << "model: " << toModelString(state.model) << "\n";
    out << "noise_control: " << toNoiseControlString(state.noiseControlMode) << "\n";
    out << "conversational_awareness: " << (state.conversationalAwareness ? "on" : "off") << "\n";
    out << "hearing_aid: " << (state.hearingAidEnabled ? "on" : "off") << "\n";
    out << "left_battery: " << formatBattery(left) << "\n";
    out << "right_battery: " << formatBattery(right) << "\n";
    out << "case_battery: " << formatBattery(caze) << "\n";
    out << "left_in_ear: " << (ears.isPrimaryInEar() ? "yes" : "no") << "\n";
    out << "right_in_ear: " << (ears.isSecondaryInEar() ? "yes" : "no") << "\n";
    out.flush();
}

void printDisconnectedStatus(const QString &address, bool asJson)
{
    if (asJson)
    {
        QJsonObject obj;
        obj.insert("connected", false);
        obj.insert("address", address.isEmpty() ? QJsonValue::Null : QJsonValue(address));
        obj.insert("name", QJsonValue::Null);
        obj.insert("model", QJsonValue::Null);
        obj.insert("noise_control", QJsonValue::Null);
        obj.insert("conversational_awareness", QJsonValue::Null);
        obj.insert("hearing_aid", QJsonValue::Null);
        obj.insert("left_battery", QJsonValue::Null);
        obj.insert("right_battery", QJsonValue::Null);
        obj.insert("case_battery", QJsonValue::Null);
        obj.insert("left_in_ear", QJsonValue::Null);
        obj.insert("right_in_ear", QJsonValue::Null);
        QTextStream(stdout) << QJsonDocument(obj).toJson(QJsonDocument::Compact) << "\n";
        return;
    }

    QTextStream out(stdout);
    out << "connected: no\n";
    out << "address: " << (address.isEmpty() ? "n/a" : address) << "\n";
    out.flush();
}

void printCliHelp(QTextStream &out)
{
    out << "Usage: vibepods-cli [options] command [value]\n";
    out << "VibePods CLI\n\n";
    out << "Options:\n";
    out << "  -h, --help               Display this help message\n";
    out << "  -m, --mac <mac>          Bluetooth address of AirPods (optional)\n";
    out << "  -j, --json               Output status in JSON format\n";
    out << "  -d, --debug              Enable verbose debug logs\n";
    out << "  -t, --timeout <seconds>  Timeout in seconds\n\n";
    out << "Arguments:\n";
    out << "  command                  connect | disconnect | status | mode | ca\n";
    out << "  value                    Command value: mode (off|nc|transparency|adaptive), ca (on|off)\n";
    out.flush();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("vibepods-cli");

    QCommandLineParser parser;
    parser.setApplicationDescription("VibePods CLI");
    parser.addOption({{"h", "help"}, "Display this help message"});

    parser.addPositionalArgument("command", "connect | disconnect | status | mode | ca");
    parser.addPositionalArgument("value", "Command value: mode (off|nc|transparency|adaptive), ca (on|off)", "[value]");
    parser.addOption({{"m", "mac"}, "Bluetooth address of AirPods (optional)", "mac"});
    parser.addOption({{"j", "json"}, "Output status in JSON format"});
    parser.addOption({{"d", "debug"}, "Enable verbose debug logs"});
    parser.addOption({{"t", "timeout"}, "Timeout in seconds", "seconds", "12"});

    parser.process(app);
    if (parser.isSet("help"))
    {
        QTextStream out(stdout);
        printCliHelp(out);
        return 0;
    }

    const QStringList positional = parser.positionalArguments();
    const QString command = positional.isEmpty() ? QString() : positional.first().trimmed().toLower();
    const QString valuePositional = positional.size() > 1 ? positional.at(1) : QString();
    const QString mac = normalizeMac(parser.value("mac"));
    const bool jsonOutput = parser.isSet("json");
    const bool debugOutput = parser.isSet("debug");
    const int timeoutSec = parser.value("timeout").toInt();

    if (debugOutput)
    {
        QLoggingCategory::setFilterRules("vibepods.info=true\nvibepods.debug=true");
    }
    else
    {
        QLoggingCategory::setFilterRules("vibepods.info=false\nvibepods.debug=false");
    }

    QTextStream err(stderr);
    if (command.isEmpty())
    {
        err << "Missing command.\n";
        printCliHelp(err);
        return 1;
    }
    if (command != "connect" && command != "disconnect" && command != "status" && command != "mode" && command != "ca")
    {
        QTextStream(stderr) << "unknown command: " << command << "\n";
        return 2;
    }
    if (jsonOutput && command != "status")
    {
        QTextStream(stderr) << "--json is only supported with status\n";
        return 2;
    }

    NoiseControlMode targetMode = NoiseControlMode::Off;
    if (command == "mode")
    {
        if (!parseModeValue(valuePositional, targetMode))
        {
            QTextStream(stderr) << "invalid mode, expected: off|nc|transparency|adaptive\n";
            return 2;
        }
    }
    bool targetCaEnabled = false;
    if (command == "ca")
    {
        if (!parseOnOffValue(valuePositional, targetCaEnabled))
        {
            QTextStream(stderr) << "invalid ca value, expected: on|off\n";
            return 2;
        }
    }

    if (command == "connect")
    {
        QTextStream out(stdout);
        QTextStream in(stdin);

        auto devices = queryBluezAirPods();
        if (devices.isEmpty())
        {
            QTextStream(stderr) << "no AirPods found in BlueZ device list; pair them first\n";
            return 6;
        }

        int selected = -1;
        if (!mac.isEmpty())
        {
            for (int i = 0; i < devices.size(); ++i)
            {
                if (devices[i].address.compare(mac, Qt::CaseInsensitive) == 0)
                {
                    selected = i;
                    break;
                }
            }
            if (selected < 0)
            {
                QTextStream(stderr) << "provided --mac not found in paired list\n";
                return 1;
            }
        }
        else
        {
            out << "Available AirPods:\n";
            for (int i = 0; i < devices.size(); ++i)
            {
                const auto &d = devices.at(i);
                out << "  [" << i + 1 << "] "
                    << (d.name.isEmpty() ? "AirPods" : d.name)
                    << " (" << d.address << ")"
                    << (d.connected ? " [already connected]" : "")
                    << "\n";
            }
            out << "Choose a number: ";
            out.flush();
            const QString line = in.readLine().trimmed();
            bool ok = false;
            const int choice = line.toInt(&ok);
            if (!ok || choice < 1 || choice > devices.size())
            {
                QTextStream(stderr) << "invalid selection\n";
                return 2;
            }
            selected = choice - 1;
        }

        const auto &target = devices.at(selected);
        out << "Connecting to " << target.address << "...\n";
        out.flush();

        QString detail;
        if (!connectViaBluetoothctl(target.address, detail))
        {
            QTextStream(stderr) << "bluetoothctl connect failed: " << detail << "\n";
            return 7;
        }

        QThread::msleep(1200);
        if (!isBluezAirPodsConnected(target.address))
        {
            QTextStream(stderr) << "AirPods still not connected in BlueZ state\n";
            QTextStream(stderr) << "bluetoothctl output: " << detail << "\n";
            return 8;
        }

        QString audioDetail;
        if (tryRouteAudioToAirPods(target.address, audioDetail))
        {
            out << "Connected. " << audioDetail << "\n";
        }
        else
        {
            out << "Connected. Audio routing not forced (" << audioDetail << ").\n";
        }
        out << "You can now start audio playback.\n";
        out.flush();
        return 0;
    }

    if (command == "disconnect")
    {
        QTextStream out(stdout);

        const auto devices = queryBluezAirPods();
        if (devices.isEmpty())
        {
            QTextStream(stderr) << "no AirPods found in BlueZ device list\n";
            return 6;
        }
        if (!mac.isEmpty() && !isValidMac(mac))
        {
            QTextStream(stderr) << "invalid --mac format, expected AA:BB:CC:DD:EE:FF\n";
            return 1;
        }

        QList<BluezAirPodsDevice> targets;
        if (!mac.isEmpty())
        {
            for (const auto &d : devices)
            {
                if (d.address.compare(mac, Qt::CaseInsensitive) == 0)
                {
                    targets.push_back(d);
                    break;
                }
            }
            if (targets.isEmpty())
            {
                QTextStream(stderr) << "provided --mac not found in paired list\n";
                return 1;
            }
        }
        else
        {
            for (const auto &d : devices)
            {
                if (d.connected)
                {
                    targets.push_back(d);
                }
            }
            if (targets.isEmpty())
            {
                out << "No connected AirPods found.\n";
                out.flush();
                return 0;
            }
        }

        bool allOk = true;
        for (const auto &target : targets)
        {
            if (!target.connected)
            {
                out << "Already disconnected: " << target.address << "\n";
                continue;
            }

            out << "Disconnecting " << target.address << "...\n";
            out.flush();

            QString detail;
            if (!disconnectViaBluetoothctl(target.address, detail))
            {
                QTextStream(stderr) << "bluetoothctl disconnect failed for "
                                    << target.address << ": " << detail << "\n";
                allOk = false;
                continue;
            }

            QThread::msleep(900);
            if (isBluezAirPodsConnected(target.address))
            {
                QTextStream(stderr) << "AirPods still connected in BlueZ state: "
                                    << target.address << "\n";
                allOk = false;
                continue;
            }

            out << "Disconnected " << target.address << "\n";
        }

        out.flush();
        return allOk ? 0 : 7;
    }

    QString statusMac = mac;
    if (!statusMac.isEmpty() && !isValidMac(statusMac))
    {
        err << "invalid --mac format, expected AA:BB:CC:DD:EE:FF\n";
        return 1;
    }
    if (statusMac.isEmpty())
    {
        const auto devices = queryBluezAirPods();
        for (const auto &d : devices)
        {
            if (d.connected)
            {
                statusMac = d.address;
                break;
            }
        }
        if (statusMac.isEmpty() && !devices.isEmpty())
        {
            statusMac = devices.first().address;
        }
        if (statusMac.isEmpty())
        {
            if (command == "status")
            {
                printDisconnectedStatus(QString(), jsonOutput);
                return 0;
            }
            err << "no AirPods found in BlueZ list, connect first\n";
            return 6;
        }
    }

    // For mode/ca, try to bring the device online if BlueZ says it's disconnected.
    if (!isBluezAirPodsConnected(statusMac))
    {
        if (command == "status")
        {
            printDisconnectedStatus(statusMac, jsonOutput);
            return 0;
        }

        QString detail;
        if (!connectViaBluetoothctl(statusMac, detail))
        {
            QTextStream(stderr) << "AirPods not connected and auto-connect failed: " << detail << "\n";
            QTextStream(stderr) << "Run `vibepods-cli connect` first.\n";
            return 7;
        }
        QThread::msleep(1200);
        if (!isBluezAirPodsConnected(statusMac))
        {
            QTextStream(stderr) << "AirPods auto-connect attempted but device is still disconnected.\n";
            QTextStream(stderr) << "Run `vibepods-cli connect` first.\n";
            return 8;
        }
    }

    AirPodsCoreClient client;
    bool printed = false;
    bool modeCommandSent = false;
    bool modeConfirmed = false;
    bool initialModeStateSeen = false;
    bool caCommandSent = false;
    bool caConfirmed = false;
    bool caRejected = false;
    bool caRetrySent = false;
    bool batteryDataSeen = false;
    bool earDataSeen = false;
    bool statusEarSettleDone = false;
    bool fatalConnectionError = false;
    QTimer statusPollTimer;
    statusPollTimer.setInterval(900);
    int statusPollAttempts = 0;
    QTimer statusEarSettleTimer;
    statusEarSettleTimer.setSingleShot(true);
    EarDetection::EarDetectionStatus statusLastPrimary = EarDetection::EarDetectionStatus::Disconnected;
    EarDetection::EarDetectionStatus statusLastSecondary = EarDetection::EarDetectionStatus::Disconnected;

    auto maybeFinishStatus = [&]()
    {
        if (command != "status" || printed)
        {
            return;
        }
        if (!earDataSeen || !statusEarSettleDone)
        {
            return;
        }
        statusPollTimer.stop();
        printed = true;
        printStatus(client, jsonOutput);
        QCoreApplication::exit(0);
    };

    QObject::connect(&statusEarSettleTimer, &QTimer::timeout, &app, [&]()
    {
        statusEarSettleDone = true;
        maybeFinishStatus();
    });

    QObject::connect(&statusPollTimer, &QTimer::timeout, &app, [&]()
    {
        if (command != "status" || printed)
        {
            statusPollTimer.stop();
            return;
        }
        if (statusPollAttempts >= 8)
        {
            statusPollTimer.stop();
            return;
        }
        client.requestStatusSnapshot();
        ++statusPollAttempts;
    });

    QObject::connect(&client, &AirPodsCoreClient::errorOccurred, &app, [&](const QString &message)
    {
        QTextStream(stderr) << "error: " << message << "\n";
        if (message.contains("Cannot connect to remote profile", Qt::CaseInsensitive) ||
            message.contains("Cannot find remote device", Qt::CaseInsensitive) ||
            message.contains("Invalid Bluetooth address", Qt::CaseInsensitive))
        {
            fatalConnectionError = true;
            QTextStream(stderr) << "AirPods not ready for AACP. Run `vibepods-cli connect` then retry.\n";
            QCoreApplication::exit(9);
        }
    });

    auto sendModeCommand = [&]()
    {
        if (modeCommandSent)
        {
            return;
        }
        modeCommandSent = client.setNoiseControlMode(targetMode);
        if (!modeCommandSent)
        {
            QTextStream(stderr) << "failed to send mode command\n";
            QCoreApplication::exit(3);
        }
    };

    auto sendCaCommand = [&]()
    {
        if (caCommandSent)
        {
            return;
        }
        caCommandSent = client.setConversationalAwareness(targetCaEnabled);
        if (!caCommandSent)
        {
            QTextStream(stderr) << "failed to send ca command\n";
            QCoreApplication::exit(3);
        }
    };

    if (command == "mode")
    {
        // Better timing: wait for the first ANC state snapshot packet, then send our command.
        QObject::connect(&client, &AirPodsCoreClient::packetReceived, &app, [&](const QByteArray &packet)
        {
            if (packet.size() == 11 && packet.startsWith(AirPodsPackets::NoiseControl::HEADER))
            {
                initialModeStateSeen = true;
                sendModeCommand();
            }
        });

        // Fallback in case the initial snapshot packet is delayed.
        QObject::connect(&client, &AirPodsCoreClient::connected, &app, [&]()
        {
            QTimer::singleShot(2000, &app, [&]()
            {
                if (!modeCommandSent)
                {
                    sendModeCommand();
                }
            });
        });
    }
    if (command == "ca")
    {
        QObject::connect(&client, &AirPodsCoreClient::protocolReady, &app, [&]()
        {
            if (targetCaEnabled &&
                client.state().noiseControlMode != NoiseControlMode::Adaptive &&
                client.state().noiseControlMode != NoiseControlMode::Transparency)
            {
                client.setNoiseControlMode(NoiseControlMode::Adaptive);
                QTimer::singleShot(350, &app, [&]()
                {
                    sendCaCommand();
                });
                return;
            }
            sendCaCommand();
        });

        // Fallback in case protocolReady is missed or delayed.
        QObject::connect(&client, &AirPodsCoreClient::connected, &app, [&]()
        {
            QTimer::singleShot(2000, &app, [&]()
            {
                if (!caCommandSent)
                {
                    sendCaCommand();
                }
            });
        });

        // Retry once if device reports the opposite state after our first command.
        QObject::connect(&client, &AirPodsCoreClient::packetReceived, &app, [&](const QByteArray &packet)
        {
            if (!packet.startsWith(AirPodsPackets::ConversationalAwareness::HEADER))
            {
                return;
            }
            const auto state = AirPodsPackets::ConversationalAwareness::parseState(packet);
            if (!state.has_value())
            {
                return;
            }
            if (caCommandSent && state.value() != targetCaEnabled)
            {
                caRejected = true;
                if (!caRetrySent)
                {
                    caRetrySent = true;
                    QTimer::singleShot(500, &app, [&]()
                    {
                        client.setConversationalAwareness(targetCaEnabled);
                    });
                }
            }
        });
    }
    if (command == "status")
    {
        QObject::connect(&client, &AirPodsCoreClient::protocolReady, &app, [&]()
        {
            // Trigger fresh notifications, then keep polling briefly to avoid stale
            // transitional ear-detection values on connect.
            statusPollAttempts = 0;
            client.requestStatusSnapshot();
            ++statusPollAttempts;
            statusPollTimer.start();
        });
    }

    QObject::connect(&client, &AirPodsCoreClient::stateChanged, &app, [&]()
    {
        const auto left = client.battery().getState(Battery::Component::Left);
        const auto right = client.battery().getState(Battery::Component::Right);
        const auto caze = client.battery().getState(Battery::Component::Case);
        const auto headset = client.battery().getState(Battery::Component::Headset);
        const bool hasBatteryData =
            left.status != Battery::BatteryStatus::Disconnected ||
            right.status != Battery::BatteryStatus::Disconnected ||
            caze.status != Battery::BatteryStatus::Disconnected ||
            headset.status != Battery::BatteryStatus::Disconnected;
        if (hasBatteryData)
        {
            batteryDataSeen = true;
        }
        const bool hasEarData =
            client.earDetection().getprimaryStatus() != EarDetection::EarDetectionStatus::Disconnected ||
            client.earDetection().getsecondaryStatus() != EarDetection::EarDetectionStatus::Disconnected;
        if (hasEarData)
        {
            earDataSeen = true;
            if (command == "status")
            {
                const auto currentPrimary = client.earDetection().getprimaryStatus();
                const auto currentSecondary = client.earDetection().getsecondaryStatus();
                const bool changed =
                    currentPrimary != statusLastPrimary || currentSecondary != statusLastSecondary;
                if (changed)
                {
                    statusLastPrimary = currentPrimary;
                    statusLastSecondary = currentSecondary;
                    statusEarSettleDone = false;

                    // Asymmetric in-ear states tend to be transitional when buds are
                    // moving in/out of ears or case; wait a bit longer.
                    const bool primaryInEar = currentPrimary == EarDetection::EarDetectionStatus::InEar;
                    const bool secondaryInEar = currentSecondary == EarDetection::EarDetectionStatus::InEar;
                    const int settleMs = (primaryInEar != secondaryInEar) ? 2500 : 700;
                    statusEarSettleTimer.start(settleMs);
                }
            }
        }

        if (command == "mode")
        {
            if (modeCommandSent && client.state().noiseControlMode == targetMode)
            {
                modeConfirmed = true;
                if (batteryDataSeen && earDataSeen && !printed)
                {
                    printed = true;
                    printStatus(client, jsonOutput);
                    QCoreApplication::exit(0);
                }
            }
            return;
        }
        if (command == "ca")
        {
            if (caCommandSent && client.state().conversationalAwareness == targetCaEnabled)
            {
                caConfirmed = true;
                if (batteryDataSeen && earDataSeen && !printed)
                {
                    printed = true;
                    printStatus(client, jsonOutput);
                    QCoreApplication::exit(0);
                }
            }
            return;
        }

        maybeFinishStatus();
    });

    QTimer::singleShot(qMax(1, timeoutSec) * 1000, &app, [&]()
    {
        if (fatalConnectionError)
        {
            QCoreApplication::exit(9);
            return;
        }
        if (!printed)
        {
            if (command == "mode")
            {
                if (modeConfirmed)
                {
                    if (batteryDataSeen && earDataSeen)
                    {
                        QTextStream(stderr) << "mode set; printing current status\n";
                    }
                    else
                    {
                        QTextStream(stderr) << "mode set but timeout waiting for battery/ear-detection update, printing partial status\n";
                    }
                }
                else
                {
                    if (!initialModeStateSeen)
                    {
                        QTextStream(stderr) << "initial mode state not observed; ";
                    }
                    QTextStream(stderr) << "timeout waiting for mode confirmation, printing current status\n";
                }
            }
            else if (command == "ca")
            {
                if (caConfirmed)
                {
                    if (batteryDataSeen && earDataSeen)
                    {
                        QTextStream(stderr) << "ca set; printing current status\n";
                    }
                    else
                    {
                        QTextStream(stderr) << "ca set but timeout waiting for battery/ear-detection update, printing partial status\n";
                    }
                }
                else
                {
                    if (caRejected)
                    {
                        QTextStream(stderr) << "device rejected ca change (likely unsupported in current mode/device), printing current status\n";
                    }
                    else
                    {
                        QTextStream(stderr) << "timeout waiting for ca confirmation, printing current status\n";
                    }
                }
            }
            else
            {
                QTextStream(stderr) << "timeout waiting for ear-detection update, printing partial status\n";
            }
            statusPollTimer.stop();
            printStatus(client, jsonOutput);
            QCoreApplication::exit(4);
            return;
        }
        QCoreApplication::exit(0);
    });

    client.connectToDevice(statusMac);
    return app.exec();
}

#else

int gSignalPipe[2] = {-1, -1};

QString toModelString(AirPodsModel model)
{
    switch (model)
    {
    case AirPodsModel::AirPods1: return "AirPods 1";
    case AirPodsModel::AirPods2: return "AirPods 2";
    case AirPodsModel::AirPods3: return "AirPods 3";
    case AirPodsModel::AirPods4: return "AirPods 4";
    case AirPodsModel::AirPods4ANC: return "AirPods 4 ANC";
    case AirPodsModel::AirPodsPro: return "AirPods Pro";
    case AirPodsModel::AirPodsPro2Lightning: return "AirPods Pro 2 Lightning";
    case AirPodsModel::AirPodsPro2USBC: return "AirPods Pro 2 USB-C";
    case AirPodsModel::AirPodsMaxLightning: return "AirPods Max Lightning";
    case AirPodsModel::AirPodsMaxUSBC: return "AirPods Max USB-C";
    default: return "Unknown";
    }
}

QString defaultStatePath()
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (root.isEmpty())
    {
        root = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    }
    if (root.isEmpty())
    {
        root = QDir::homePath() + "/.cache";
    }
    return root + "/vibepods/status.json";
}

bool ensureParentDir(const QString &path)
{
    const QFileInfo info(path);
    return QDir().mkpath(info.dir().absolutePath());
}

bool writeTextFile(const QString &path, const QByteArray &content)
{
    if (!ensureParentDir(path))
    {
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    if (file.write(content) != content.size())
    {
        return false;
    }
    return file.commit();
}

void signalHandler(int signalNumber)
{
    if (gSignalPipe[1] == -1)
    {
        return;
    }

    char code = '\0';
    switch (signalNumber)
    {
    case SIGUSR1:
        code = 'R';
        break;
    case SIGINT:
    case SIGTERM:
        code = 'Q';
        break;
    default:
        return;
    }

    const auto ignored = write(gSignalPipe[1], &code, sizeof(code));
    (void)ignored;
}

bool installSignalHandlers()
{
    if (pipe(gSignalPipe) != 0)
    {
        return false;
    }

    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGUSR1, &sa, nullptr) != 0)
    {
        return false;
    }
    if (sigaction(SIGINT, &sa, nullptr) != 0)
    {
        return false;
    }
    if (sigaction(SIGTERM, &sa, nullptr) != 0)
    {
        return false;
    }

    return true;
}

class VibepodsDaemon : public QObject
{
public:
    VibepodsDaemon(QString configuredMac, QString statePath, int bluezPollSeconds,
                   int snapshotIntervalSeconds, QObject *parent = nullptr)
        : QObject(parent),
          configuredMac_(std::move(configuredMac)),
          statePath_(std::move(statePath))
    {
        qDBusRegisterMetaType<QDBusObjectPath>();
        qDBusRegisterMetaType<ManagedObjectList>();

        lastStateChangeAt_ = QDateTime::currentDateTimeUtc();
        bluezPollTimer_.setInterval(qMax(1, bluezPollSeconds) * 1000);
        snapshotTimer_.setInterval(snapshotIntervalSeconds > 0 ? snapshotIntervalSeconds * 1000 : 0);

        connect(&bluezPollTimer_, &QTimer::timeout, this, [this]()
        {
            syncBluezState();
        });

        connect(&snapshotTimer_, &QTimer::timeout, this, [this]()
        {
            requestSnapshot("interval");
        });

        connect(&client_, &AirPodsCoreClient::connected, this, [this]()
        {
            lastError_.clear();
            rememberKnownState();
            markStateDirty();
            writeStateFile();
        });

        connect(&client_, &AirPodsCoreClient::disconnected, this, [this]()
        {
            markStateDirty();
            writeStateFile();
        });

        connect(&client_, &AirPodsCoreClient::protocolReady, this, [this]()
        {
            lastError_.clear();
            rememberKnownState();
            markStateDirty();
            writeStateFile();
            requestSnapshot("initial");
        });

        connect(&client_, &AirPodsCoreClient::stateChanged, this, [this]()
        {
            lastError_.clear();
            rememberKnownState();
            markStateDirty();
            writeStateFile();
        });

        connect(&client_, &AirPodsCoreClient::errorOccurred, this, [this](const QString &message)
        {
            lastError_ = message;
            markStateDirty();
            QTextStream(stderr) << "error: " << message << "\n";
            writeStateFile();
        });
    }

    void start()
    {
        writeStateFile();
        syncBluezState();
        bluezPollTimer_.start();
        if (snapshotTimer_.interval() > 0)
        {
            snapshotTimer_.start();
        }
    }

    void refreshNow()
    {
        if (!requestSnapshot("signal"))
        {
            syncBluezState();
        }
    }

    void cleanup()
    {
        daemonRunning_ = false;
        bluezConnected_ = false;
        if (client_.isConnected())
        {
            client_.disconnectFromDevice();
        }
        markStateDirty();
        writeStateFile();
    }

private:
    bool requestSnapshot(const QString &reason)
    {
        if (!bluezConnected_ || !client_.isConnected() || !client_.isProtocolReady())
        {
            return false;
        }
        if (!client_.requestStatusSnapshot())
        {
            lastError_ = QString("failed to request snapshot (%1)").arg(reason);
            writeStateFile();
            return false;
        }
        lastRefreshAt_ = QDateTime::currentDateTimeUtc();
        writeStateFile();
        return true;
    }

    void syncBluezState()
    {
        const auto devices = queryBluezAirPods();
        bool stateChanged = false;

        if (configuredMac_.isEmpty())
        {
            QString selectedMac;
            for (const auto &device : devices)
            {
                if (device.connected)
                {
                    selectedMac = device.address;
                    break;
                }
            }
            if (selectedMac.isEmpty() && !devices.isEmpty())
            {
                selectedMac = devices.first().address;
            }

            if (selectedMac != selectedMac_)
            {
                if (client_.isConnected())
                {
                    client_.disconnectFromDevice();
                }
                selectedMac_ = selectedMac;
                if (!lastError_.isEmpty())
                {
                    lastError_.clear();
                }
                stateChanged = true;
            }
        }

        BluezAirPodsDevice selectedDevice;
        bool deviceFound = false;
        const QString targetMac = configuredMac_.isEmpty() ? selectedMac_ : configuredMac_;
        for (const auto &device : devices)
        {
            if (device.address.compare(targetMac, Qt::CaseInsensitive) == 0)
            {
                selectedDevice = device;
                deviceFound = true;
                break;
            }
        }

        const bool newBluezConnected = deviceFound && selectedDevice.connected;
        const QString newBluezName = deviceFound ? selectedDevice.name : QString();
        if (newBluezConnected != bluezConnected_)
        {
            bluezConnected_ = newBluezConnected;
            stateChanged = true;
        }
        if (newBluezName != bluezName_)
        {
            bluezName_ = newBluezName;
            if (!bluezName_.isEmpty())
            {
                lastKnownName_ = bluezName_;
            }
            stateChanged = true;
        }
        if (!targetMac.isEmpty())
        {
            lastKnownAddress_ = targetMac;
        }

        if (targetMac.isEmpty())
        {
            if (client_.isConnected())
            {
                client_.disconnectFromDevice();
            }
            if (stateChanged)
            {
                markStateDirty();
                writeStateFile();
            }
            return;
        }

        if (!deviceFound || !bluezConnected_)
        {
            if (client_.isConnected())
            {
                client_.disconnectFromDevice();
            }
            if (stateChanged)
            {
                markStateDirty();
                writeStateFile();
            }
            return;
        }

        const QString currentMac = client_.state().bluetoothAddress;
        if (client_.isConnected() && currentMac.compare(targetMac, Qt::CaseInsensitive) != 0)
        {
            client_.disconnectFromDevice();
        }

        if (!client_.isConnected())
        {
            client_.connectToDevice(targetMac);
        }

        if (stateChanged)
        {
            markStateDirty();
            writeStateFile();
        }
    }

    QJsonValue batteryValue(const Battery::BatteryState &state) const
    {
        if (state.status == Battery::BatteryStatus::Disconnected)
        {
            return QJsonValue::Null;
        }
        return static_cast<int>(state.level);
    }

    QJsonObject buildStateObject() const
    {
        const auto &state = client_.state();
        const auto &battery = client_.battery();
        const auto &ears = client_.earDetection();
        const auto left = battery.getState(Battery::Component::Left);
        const auto right = battery.getState(Battery::Component::Right);
        const auto caze = battery.getState(Battery::Component::Case);
        const auto headset = battery.getState(Battery::Component::Headset);
        const bool hasEarData =
            ears.getprimaryStatus() != EarDetection::EarDetectionStatus::Disconnected ||
            ears.getsecondaryStatus() != EarDetection::EarDetectionStatus::Disconnected;

        QJsonObject obj;
        obj.insert("connected", bluezConnected_);
        obj.insert("daemon_running", daemonRunning_);
        obj.insert("protocol_connected", client_.isConnected() && client_.isProtocolReady());
        obj.insert("address", state.bluetoothAddress.isEmpty()
                                  ? (lastKnownAddress_.isEmpty() ? QJsonValue::Null : QJsonValue(lastKnownAddress_))
                                  : QJsonValue(state.bluetoothAddress));
        obj.insert("name", state.deviceName.isEmpty()
                               ? (lastKnownName_.isEmpty() ? QJsonValue::Null : QJsonValue(lastKnownName_))
                               : QJsonValue(state.deviceName));
        obj.insert("model", state.model == AirPodsModel::Unknown ? QJsonValue::Null
                                                                  : QJsonValue(toModelString(state.model)));
        obj.insert("noise_control", client_.isProtocolReady() ? QJsonValue(toNoiseControlString(state.noiseControlMode))
                                                              : QJsonValue::Null);
        obj.insert("conversational_awareness", client_.isProtocolReady() ? QJsonValue(state.conversationalAwareness)
                                                                          : QJsonValue::Null);
        obj.insert("hearing_aid", client_.isProtocolReady() ? QJsonValue(state.hearingAidEnabled)
                                                             : QJsonValue::Null);
        obj.insert("left_battery", batteryValue(left));
        obj.insert("right_battery", batteryValue(right));
        obj.insert("case_battery", batteryValue(caze));
        obj.insert("headset_battery", batteryValue(headset));
        obj.insert("left_in_ear", hasEarData ? QJsonValue(ears.isPrimaryInEar()) : QJsonValue::Null);
        obj.insert("right_in_ear", hasEarData ? QJsonValue(ears.isSecondaryInEar()) : QJsonValue::Null);
        obj.insert("updated_at", lastStateChangeAt_.toString(Qt::ISODateWithMs));
        obj.insert("last_refresh_at", lastRefreshAt_.isValid() ? QJsonValue(lastRefreshAt_.toString(Qt::ISODateWithMs))
                                                               : QJsonValue::Null);
        obj.insert("state_path", statePath_);
        obj.insert("source", "daemon");
        obj.insert("last_error", lastError_.isEmpty() ? QJsonValue::Null : QJsonValue(lastError_));
        return obj;
    }

    void markStateDirty()
    {
        lastStateChangeAt_ = QDateTime::currentDateTimeUtc();
    }

    void rememberKnownState()
    {
        if (!client_.state().bluetoothAddress.isEmpty())
        {
            lastKnownAddress_ = client_.state().bluetoothAddress;
        }
        if (!client_.state().deviceName.isEmpty())
        {
            lastKnownName_ = client_.state().deviceName;
        }
    }

    void writeStateFile()
    {
        const QByteArray payload = QJsonDocument(buildStateObject()).toJson(QJsonDocument::Compact) + "\n";
        if (payload == lastWrittenPayload_)
        {
            return;
        }
        if (!writeTextFile(statePath_, payload))
        {
            QTextStream(stderr) << "warning: failed to write state file " << statePath_ << "\n";
            return;
        }
        lastWrittenPayload_ = payload;
    }

    QString configuredMac_;
    QString selectedMac_;
    QString bluezName_;
    QString lastKnownAddress_;
    QString lastKnownName_;
    QString statePath_;
    QString lastError_;
    QDateTime lastRefreshAt_;
    QDateTime lastStateChangeAt_;
    QByteArray lastWrittenPayload_;
    bool daemonRunning_ = true;
    bool bluezConnected_ = false;
    AirPodsCoreClient client_;
    QTimer bluezPollTimer_;
    QTimer snapshotTimer_;
};

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("vibepods-daemon");
    QCoreApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Persistent VibePods daemon for cache-driven status consumers");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({{"m", "mac"}, "Bluetooth address of AirPods (optional)", "mac"});
    parser.addOption({{"o", "output"}, "JSON state file path", "path", defaultStatePath()});
    parser.addOption({{"b", "bluez-poll"}, "BlueZ poll interval in seconds", "seconds", "5"});
    parser.addOption({{"s", "snapshot-interval"}, "Periodic status snapshot interval in seconds (0 disables it)",
                      "seconds", "0"});
    parser.addOption({{"d", "debug"}, "Enable verbose debug logs"});
    parser.process(app);

    if (parser.isSet("debug"))
    {
        QLoggingCategory::setFilterRules(QStringLiteral("vibepods.debug=true"));
    }

    const QString mac = normalizeMac(parser.value("mac"));
    if (!mac.isEmpty() && !isValidMac(mac))
    {
        QTextStream(stderr) << "invalid mac: " << mac << "\n";
        return 2;
    }

    bool bluezPollOk = false;
    const int bluezPollSeconds = parser.value("bluez-poll").toInt(&bluezPollOk);
    if (!bluezPollOk || bluezPollSeconds <= 0)
    {
        QTextStream(stderr) << "invalid --bluez-poll value\n";
        return 2;
    }

    bool snapshotIntervalOk = false;
    const int snapshotIntervalSeconds = parser.value("snapshot-interval").toInt(&snapshotIntervalOk);
    if (!snapshotIntervalOk || snapshotIntervalSeconds < 0)
    {
        QTextStream(stderr) << "invalid --snapshot-interval value\n";
        return 2;
    }

    if (!installSignalHandlers())
    {
        QTextStream(stderr) << "failed to install signal handlers\n";
        return 3;
    }

    VibepodsDaemon daemon(mac, parser.value("output"), bluezPollSeconds, snapshotIntervalSeconds);
    QSocketNotifier signalNotifier(gSignalPipe[0], QSocketNotifier::Read);

    QObject::connect(&signalNotifier, &QSocketNotifier::activated, &app, [&](QSocketDescriptor)
    {
        char buffer[32];
        const auto bytesRead = read(gSignalPipe[0], buffer, sizeof(buffer));
        if (bytesRead <= 0)
        {
            return;
        }

        for (ssize_t i = 0; i < bytesRead; ++i)
        {
            if (buffer[i] == 'R')
            {
                daemon.refreshNow();
            }
            else if (buffer[i] == 'Q')
            {
                app.quit();
            }
        }
    });

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&]()
    {
        daemon.cleanup();
        if (gSignalPipe[0] != -1)
        {
            close(gSignalPipe[0]);
            gSignalPipe[0] = -1;
        }
        if (gSignalPipe[1] != -1)
        {
            close(gSignalPipe[1]);
            gSignalPipe[1] = -1;
        }
    });

    daemon.start();
    return app.exec();
}

#endif
