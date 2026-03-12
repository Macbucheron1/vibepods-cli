#include <QCoreApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QtDBus/QtDBus>

#include <librepods/core/airpods_core_client.h>
#include <librepods/enums.h>

Q_LOGGING_CATEGORY(librepods, "librepods")

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

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("librepods-cli");

    QCommandLineParser parser;
    parser.setApplicationDescription("LibrePods core CLI");
    parser.addHelpOption();

    parser.addPositionalArgument("command", "connect | status | mode | ca");
    parser.addPositionalArgument("value", "Command value: mode (off|nc|transparency|adaptive), ca (on|off)", "[value]");
    parser.addOption({{"m", "mac"}, "Bluetooth address of AirPods (optional)", "mac"});
    parser.addOption({{"j", "json"}, "Output status in JSON format"});
    parser.addOption({{"d", "debug"}, "Enable verbose debug logs"});
    parser.addOption({{"t", "timeout"}, "Timeout in seconds", "seconds", "12"});

    parser.process(app);

    const QStringList positional = parser.positionalArguments();
    const QString command = positional.isEmpty() ? QString() : positional.first().trimmed().toLower();
    const QString valuePositional = positional.size() > 1 ? positional.at(1) : QString();
    const QString mac = normalizeMac(parser.value("mac"));
    const bool jsonOutput = parser.isSet("json");
    const bool debugOutput = parser.isSet("debug");
    const int timeoutSec = parser.value("timeout").toInt();

    if (debugOutput)
    {
        QLoggingCategory::setFilterRules("librepods.info=true\nlibrepods.debug=true");
    }
    else
    {
        QLoggingCategory::setFilterRules("librepods.info=false\nlibrepods.debug=false");
    }

    QTextStream err(stderr);
    if (command.isEmpty())
    {
        err << "Missing command.\n";
        parser.showHelp(1);
    }
    if (command != "connect" && command != "status" && command != "mode" && command != "ca")
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
            out << "AirPods disponibles:\n";
            for (int i = 0; i < devices.size(); ++i)
            {
                const auto &d = devices.at(i);
                out << "  [" << i + 1 << "] "
                    << (d.name.isEmpty() ? "AirPods" : d.name)
                    << " (" << d.address << ")"
                    << (d.connected ? " [already connected]" : "")
                    << "\n";
            }
            out << "Choisis un numero: ";
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
        out << "Tu peux maintenant lancer ton audio.\n";
        out.flush();
        return 0;
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
            err << "no AirPods found in BlueZ list, connect first\n";
            return 6;
        }
    }

    // For status/mode, try to bring the device online if BlueZ says it's disconnected.
    if (!isBluezAirPodsConnected(statusMac))
    {
        QString detail;
        if (!connectViaBluetoothctl(statusMac, detail))
        {
            QTextStream(stderr) << "AirPods not connected and auto-connect failed: " << detail << "\n";
            QTextStream(stderr) << "Run `librepods-cli connect` first.\n";
            return 7;
        }
        QThread::msleep(1200);
        if (!isBluezAirPodsConnected(statusMac))
        {
            QTextStream(stderr) << "AirPods auto-connect attempted but device is still disconnected.\n";
            QTextStream(stderr) << "Run `librepods-cli connect` first.\n";
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
    bool fatalConnectionError = false;

    QObject::connect(&client, &AirPodsCoreClient::errorOccurred, &app, [&](const QString &message)
    {
        QTextStream(stderr) << "error: " << message << "\n";
        if (message.contains("Cannot connect to remote profile", Qt::CaseInsensitive) ||
            message.contains("Cannot find remote device", Qt::CaseInsensitive) ||
            message.contains("Invalid Bluetooth address", Qt::CaseInsensitive))
        {
            fatalConnectionError = true;
            QTextStream(stderr) << "AirPods not ready for AACP. Run `librepods-cli connect` then retry.\n";
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

        if (command == "mode")
        {
            if (modeCommandSent && client.state().noiseControlMode == targetMode)
            {
                modeConfirmed = true;
                if (batteryDataSeen && !printed)
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
                if (batteryDataSeen && !printed)
                {
                    printed = true;
                    printStatus(client, jsonOutput);
                    QCoreApplication::exit(0);
                }
            }
            return;
        }

        if (batteryDataSeen && !printed)
        {
            printed = true;
            printStatus(client, jsonOutput);
            QCoreApplication::exit(0);
        }
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
                    if (batteryDataSeen)
                    {
                        QTextStream(stderr) << "mode set; printing current status\n";
                    }
                    else
                    {
                        QTextStream(stderr) << "mode set but timeout waiting for battery update, printing partial status\n";
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
                    if (batteryDataSeen)
                    {
                        QTextStream(stderr) << "ca set; printing current status\n";
                    }
                    else
                    {
                        QTextStream(stderr) << "ca set but timeout waiting for battery update, printing partial status\n";
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
                QTextStream(stderr) << "timeout waiting for battery update, printing partial status\n";
            }
            printStatus(client, jsonOutput);
            QCoreApplication::exit(4);
            return;
        }
        QCoreApplication::exit(0);
    });

    client.connectToDevice(statusMac);
    return app.exec();
}
