#pragma once

#include <QObject>
#include <QBluetoothSocket>
#include <QString>

#include <librepods/airpods_packets.h>
#include <librepods/battery.hpp>
#include <librepods/eardetection.hpp>
#include <librepods/enums.h>

struct AirPodsCoreState
{
    QString bluetoothAddress;
    QString deviceName;
    AirpodsTrayApp::Enums::AirPodsModel model = AirpodsTrayApp::Enums::AirPodsModel::Unknown;
    AirpodsTrayApp::Enums::NoiseControlMode noiseControlMode = AirpodsTrayApp::Enums::NoiseControlMode::Off;
    bool conversationalAwareness = false;
    bool hearingAidEnabled = false;
};

class AirPodsCoreClient : public QObject
{
    Q_OBJECT
public:
    explicit AirPodsCoreClient(QObject *parent = nullptr);
    ~AirPodsCoreClient() override;

    void connectToDevice(const QString &address);
    void disconnectFromDevice();
    bool isConnected() const;

    bool setNoiseControlMode(AirpodsTrayApp::Enums::NoiseControlMode mode);
    bool sendControlCommand(quint8 identifier, quint8 data1 = 0x00, quint8 data2 = 0x00,
                            quint8 data3 = 0x00, quint8 data4 = 0x00);
    bool setConversationalAwareness(bool enabled);
    bool setHearingAidEnabled(bool enabled);
    bool renameAirPods(const QString &newName);
    bool requestStatusSnapshot();
    bool isProtocolReady() const { return protocolReady_; }

    const AirPodsCoreState &state() const { return state_; }
    const Battery &battery() const { return battery_; }
    const EarDetection &earDetection() const { return earDetection_; }

signals:
    void connected();
    void disconnected();
    void protocolReady();
    void stateChanged();
    void errorOccurred(const QString &error);
    void packetReceived(const QByteArray &packet);

private:
    void setupSocketSignals();
    bool writePacket(const QByteArray &packet, const QString &label);
    void parseData(const QByteArray &data);
    void parseMetadata(const QByteArray &data);

    QBluetoothSocket *socket_ = nullptr;
    bool protocolReady_ = false;
    AirPodsCoreState state_;
    Battery battery_;
    EarDetection earDetection_;
};
