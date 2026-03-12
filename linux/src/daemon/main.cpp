#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

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
int gSignalPipe[2] = {-1, -1};

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
    const QDBusMessage reply = objectManager.call("GetManagedObjects");
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

        BluezAirPodsDevice device;
        device.address = props.value("Address").toString();
        device.name = props.value("Name").toString();
        device.paired = props.value("Paired").toBool();
        device.connected = props.value("Connected").toBool();
        devices.push_back(device);
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
          configuredMac_(configuredMac),
          statePath_(std::move(statePath))
    {
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
