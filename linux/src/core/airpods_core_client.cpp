#include <librepods/core/airpods_core_client.h>

#include <QBluetoothAddress>
#include <QBluetoothUuid>

#include <librepods/logger.h>

using namespace AirpodsTrayApp::Enums;

AirPodsCoreClient::AirPodsCoreClient(QObject *parent)
    : QObject(parent), battery_(this), earDetection_(this)
{
}

AirPodsCoreClient::~AirPodsCoreClient()
{
    disconnectFromDevice();
}

void AirPodsCoreClient::connectToDevice(const QString &address)
{
    if (socket_)
    {
        socket_->close();
        socket_->deleteLater();
        socket_ = nullptr;
    }

    socket_ = new QBluetoothSocket(QBluetoothServiceInfo::L2capProtocol, this);
    setupSocketSignals();
    protocolReady_ = false;

    state_.bluetoothAddress = address;
    socket_->connectToService(QBluetoothAddress(address), QBluetoothUuid("74ec2172-0bad-4d01-8f77-997b2be0722a"));
}

void AirPodsCoreClient::disconnectFromDevice()
{
    if (!socket_)
    {
        return;
    }

    socket_->close();
    socket_->deleteLater();
    socket_ = nullptr;
}

bool AirPodsCoreClient::isConnected() const
{
    return socket_ && socket_->isOpen() && socket_->state() == QBluetoothSocket::SocketState::ConnectedState;
}

bool AirPodsCoreClient::setNoiseControlMode(NoiseControlMode mode)
{
    const QByteArray packet = AirPodsPackets::NoiseControl::getPacketForMode(mode);
    if (packet.isEmpty())
    {
        return false;
    }
    return writePacket(packet, "noise-control");
}

bool AirPodsCoreClient::sendControlCommand(quint8 identifier, quint8 data1, quint8 data2, quint8 data3, quint8 data4)
{
    return writePacket(ControlCommand::createCommand(identifier, data1, data2, data3, data4),
                       QString("control-0x%1").arg(identifier, 2, 16, QChar('0')));
}

bool AirPodsCoreClient::setConversationalAwareness(bool enabled)
{
    return writePacket(enabled ? AirPodsPackets::ConversationalAwareness::ENABLED
                               : AirPodsPackets::ConversationalAwareness::DISABLED,
                       "conversational-awareness");
}

bool AirPodsCoreClient::setHearingAidEnabled(bool enabled)
{
    return writePacket(enabled ? AirPodsPackets::HearingAid::ENABLED
                               : AirPodsPackets::HearingAid::DISABLED,
                       "hearing-aid");
}

bool AirPodsCoreClient::renameAirPods(const QString &newName)
{
    if (newName.isEmpty() || newName.size() > 32)
    {
        emit errorOccurred("Invalid name: must be between 1 and 32 chars");
        return false;
    }
    return writePacket(AirPodsPackets::Rename::getPacket(newName), "rename");
}

void AirPodsCoreClient::setupSocketSignals()
{
    connect(socket_, &QBluetoothSocket::connected, this, [this]()
    {
        LOG_INFO("Connected to AirPods socket");
        writePacket(AirPodsPackets::Connection::HANDSHAKE, "handshake");
        emit connected();
    });

    connect(socket_, &QBluetoothSocket::disconnected, this, [this]()
    {
        LOG_INFO("Disconnected from AirPods socket");
        protocolReady_ = false;
        emit disconnected();
    });

    connect(socket_, &QBluetoothSocket::readyRead, this, [this]()
    {
        const QByteArray data = socket_->readAll();
        parseData(data);
        emit packetReceived(data);
    });

    connect(socket_, &QBluetoothSocket::errorOccurred, this, [this](QBluetoothSocket::SocketError)
    {
        emit errorOccurred(socket_ ? socket_->errorString() : QStringLiteral("Unknown socket error"));
    });
}

bool AirPodsCoreClient::writePacket(const QByteArray &packet, const QString &label)
{
    if (!isConnected())
    {
        emit errorOccurred(QString("Socket not connected, cannot send %1").arg(label));
        return false;
    }

    const qint64 written = socket_->write(packet);
    if (written <= 0)
    {
        emit errorOccurred(QString("Failed to write %1 packet").arg(label));
        return false;
    }

    LOG_DEBUG("Wrote packet" << label << ":" << packet.toHex());
    return true;
}

void AirPodsCoreClient::parseMetadata(const QByteArray &data)
{
    if (!data.startsWith(AirPodsPackets::Parse::METADATA))
    {
        return;
    }

    int pos = AirPodsPackets::Parse::METADATA.size();
    if (data.size() < pos + 6)
    {
        return;
    }
    pos += 6;

    auto extractString = [&data, &pos]() -> QString
    {
        if (pos >= data.size())
        {
            return {};
        }
        const int start = pos;
        while (pos < data.size() && data.at(pos) != '\0')
        {
            ++pos;
        }
        const QString out = QString::fromUtf8(data.mid(start, pos - start));
        if (pos < data.size())
        {
            ++pos;
        }
        return out;
    };

    state_.deviceName = extractString();
    const QString modelNumber = extractString();
    const QString manufacturer = extractString();
    Q_UNUSED(manufacturer);
    state_.model = parseModelNumber(modelNumber);
}

void AirPodsCoreClient::parseData(const QByteArray &data)
{
    LOG_DEBUG("Received packet:" << data.toHex());

    if (data.startsWith(AirPodsPackets::Parse::HANDSHAKE_ACK))
    {
        writePacket(AirPodsPackets::Connection::SET_SPECIFIC_FEATURES, "set-features");
        return;
    }

    if (data.startsWith(AirPodsPackets::Parse::FEATURES_ACK))
    {
        writePacket(AirPodsPackets::Connection::REQUEST_NOTIFICATIONS, "request-notifications");
        if (!protocolReady_)
        {
            protocolReady_ = true;
            emit protocolReady();
        }
        return;
    }

    if (data.startsWith(AirPodsPackets::ConversationalAwareness::HEADER))
    {
        if (const auto result = AirPodsPackets::ConversationalAwareness::parseState(data))
        {
            state_.conversationalAwareness = result.value();
            emit stateChanged();
        }
        return;
    }

    // Some firmware variants report CA changes via DATA_HEADER events:
    // ...0108 => disabled, ...0109 => enabled.
    if (data.size() == 10 && data.startsWith(AirPodsPackets::ConversationalAwareness::DATA_HEADER))
    {
        const quint8 flag = static_cast<quint8>(data.at(9));
        if (flag == 0x08)
        {
            state_.conversationalAwareness = false;
            emit stateChanged();
        }
        else if (flag == 0x09)
        {
            state_.conversationalAwareness = true;
            emit stateChanged();
        }
        return;
    }

    if (data.startsWith(AirPodsPackets::HearingAid::HEADER))
    {
        if (const auto result = AirPodsPackets::HearingAid::parseState(data))
        {
            state_.hearingAidEnabled = result.value();
            emit stateChanged();
        }
        return;
    }

    if (data.size() == 11 && data.startsWith(AirPodsPackets::NoiseControl::HEADER))
    {
        if (const auto mode = AirPodsPackets::NoiseControl::parseMode(data))
        {
            state_.noiseControlMode = mode.value();
            emit stateChanged();
        }
        return;
    }

    if (data.size() == 8 && data.startsWith(AirPodsPackets::Parse::EAR_DETECTION))
    {
        earDetection_.parseData(data);
        emit stateChanged();
        return;
    }

    if ((data.size() == 22 || data.size() == 12) && data.startsWith(AirPodsPackets::Parse::BATTERY_STATUS))
    {
        battery_.parsePacket(data);
        emit stateChanged();
        return;
    }

    if (data.startsWith(AirPodsPackets::Parse::METADATA))
    {
        parseMetadata(data);
        emit stateChanged();
        return;
    }
}
