#include "ftmsrower.h"
#include "ftmsbike.h"
#include "ios/lockscreen.h"
#include "virtualbike.h"
#include <QBluetoothLocalDevice>
#include <QDateTime>
#include <QFile>
#include <QMetaEnum>
#include <QSettings>

#include <QThread>
#include <math.h>

#ifdef Q_OS_ANDROID
#include <QLowEnergyConnectionParameters>
#endif
#include "keepawakehelper.h"
#include <chrono>

using namespace std::chrono_literals;

ftmsrower::ftmsrower(bool noWriteResistance, bool noHeartService) {
    m_watt.setType(metric::METRIC_WATT);
    Speed.setType(metric::METRIC_SPEED);
    refresh = new QTimer(this);
    this->noWriteResistance = noWriteResistance;
    this->noHeartService = noHeartService;
    initDone = false;
    connect(refresh, &QTimer::timeout, this, &ftmsrower::update);
    refresh->start(200ms);
}

void ftmsrower::writeCharacteristic(uint8_t *data, uint8_t data_len, const QString &info, bool disable_log,
                                    bool wait_for_response) {
    if (!gattFTMSService || !gattWriteCharControlPointId.isValid()) {
        qDebug() << QStringLiteral("gattWriteCharControlPointId or gattFTMSService not valid!!");
        return;
    }

    QEventLoop loop;
    QTimer timeout;
    if (wait_for_response) {
        connect(gattFTMSService, &QLowEnergyService::characteristicChanged, &loop, &QEventLoop::quit);
        timeout.singleShot(300ms, &loop, &QEventLoop::quit);
    } else {
        connect(gattFTMSService, &QLowEnergyService::characteristicWritten, &loop, &QEventLoop::quit);
        timeout.singleShot(300ms, &loop, &QEventLoop::quit);
    }

    gattFTMSService->writeCharacteristic(gattWriteCharControlPointId, QByteArray((const char *)data, data_len));

    if (!disable_log) {
        emit debug(QStringLiteral(" >> ") + QByteArray((const char *)data, data_len).toHex(' ') +
                   QStringLiteral(" // ") + info);
    }

    loop.exec();
}

void ftmsrower::forceResistance(int8_t requestResistance) {

    uint8_t write[] = {FTMS_SET_INDOOR_BIKE_SIMULATION_PARAMS, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    write[3] = ((uint16_t)requestResistance * 100) & 0xFF;
    write[4] = ((uint16_t)requestResistance * 100) >> 8;

    writeCharacteristic(write, sizeof(write), QStringLiteral("forceResistance ") + QString::number(requestResistance));
}

void ftmsrower::update() {
    if (m_control->state() == QLowEnergyController::UnconnectedState) {

        emit disconnected();
        return;
    }

    if (initRequest) {
        uint8_t write[] = {FTMS_START_RESUME};
        writeCharacteristic(write, sizeof(write), "start simulation", false, true);

        initRequest = false;
    } else if (bluetoothDevice.isValid() &&

               m_control->state() == QLowEnergyController::DiscoveredState //&&
                                                                           // gattCommunicationChannelService &&
                                                                           // gattWriteCharacteristic.isValid() &&
                                                                           // gattNotify1Characteristic.isValid() &&
               /*initDone*/) {

        update_metrics(false, watts());

        // updating the treadmill console every second
        if (sec1Update++ == (500 / refresh->interval())) {

            sec1Update = 0;
            // updateDisplay(elapsed);
        }

        if (requestResistance != -1) {
            if (requestResistance > 100) {
                requestResistance = 100;
            } // TODO, use the bluetooth value
            else if (requestResistance == 0) {
                requestResistance = 1;
            }

            if (requestResistance != currentResistance().value()) {
                emit debug(QStringLiteral("writing resistance ") + QString::number(requestResistance));

                forceResistance(requestResistance);
            }
            requestResistance = -1;
        }
        if (requestStart != -1) {
            emit debug(QStringLiteral("starting..."));

            // btinit();

            requestStart = -1;
            emit bikeStarted();
        }
        if (requestStop != -1) {
            emit debug(QStringLiteral("stopping..."));

            // writeCharacteristic(initDataF0C800B8, sizeof(initDataF0C800B8), "stop tape");
            requestStop = -1;
        }
    }
}

void ftmsrower::serviceDiscovered(const QBluetoothUuid &gatt) {
    emit debug(QStringLiteral("serviceDiscovered ") + gatt.toString());
}

void ftmsrower::characteristicChanged(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue) {

    // qDebug() << "characteristicChanged" << characteristic.uuid() << newValue << newValue.length();
    Q_UNUSED(characteristic);
    QSettings settings;
    QString heartRateBeltName =
        settings.value(QStringLiteral("heart_rate_belt_name"), QStringLiteral("Disabled")).toString();

    qDebug() << QStringLiteral(" << ") << characteristic.uuid() << " " << newValue.toHex(' ');

    if (characteristic.uuid() != QBluetoothUuid((quint16)0x2AD1)) {
        return;
    }

    lastPacket = newValue;

    union flags {
        struct {

            uint16_t moreData : 1;
            uint16_t avgStroke : 1;
            uint16_t totDistance : 1;
            uint16_t instantPace : 1;
            uint16_t avgPace : 1;
            uint16_t instantPower : 1;
            uint16_t avgPower : 1;
            uint16_t resistanceLvl : 1;
            uint16_t expEnergy : 1;
            uint16_t heartRate : 1;
            uint16_t metabolic : 1;
            uint16_t elapsedTime : 1;
            uint16_t remainingTime : 1;
            uint16_t spare : 3;
        };

        uint16_t word_flags;
    };

    flags Flags;
    int index = 0;
    Flags.word_flags = (newValue.at(1) << 8) | newValue.at(0);
    index += 2;

    if (!Flags.moreData) {

        Cadence = ((uint8_t)newValue.at(index)) / 2;
        StrokesCount =
            (((uint16_t)((uint8_t)newValue.at(index + 2)) << 8) | (uint16_t)((uint8_t)newValue.at(index + 1)));

        index += 3;

        /*
         * the concept 2 sends the pace in 2 frames, so this condition will create a bugus speed
        if (!Flags.instantPace) {
            // eredited by echelon rower, probably we need to change this
            Speed = (0.37497622 * ((double)Cadence.value())) / 2.0;
            emit debug(QStringLiteral("Current Speed: ") + QString::number(Speed.value()));
        }*/
        emit debug(QStringLiteral("Strokes Count: ") + QString::number(StrokesCount.value()));
    }

    if (Flags.avgStroke) {

        double avgStroke;
        avgStroke = ((double)(uint16_t)((uint8_t)newValue.at(index))) / 2.0;
        index += 1;
        emit debug(QStringLiteral("Current Average Stroke: ") + QString::number(avgStroke));
    }

    if (Flags.totDistance) {
        Distance = ((double)((((uint32_t)((uint8_t)newValue.at(index + 2)) << 16) |
                              (uint32_t)((uint8_t)newValue.at(index + 1)) << 8) |
                             (uint32_t)((uint8_t)newValue.at(index)))) /
                   1000.0;
        index += 3;
    } else {
        Distance += ((Speed.value() / 3600000.0) *
                     ((double)lastRefreshCharacteristicChanged.msecsTo(QDateTime::currentDateTime())));
    }

    emit debug(QStringLiteral("Current Distance: ") + QString::number(Distance.value()));

    if (Flags.instantPace) {

        double instantPace;
        instantPace =
            ((double)(((uint16_t)((uint8_t)newValue.at(index + 1)) << 8) | (uint16_t)((uint8_t)newValue.at(index))));
        index += 2;
        emit debug(QStringLiteral("Current Pace: ") + QString::number(instantPace));

        Speed = (60.0 / instantPace) *
                30.0; // translating pace (min/500m) to km/h in order to match the pace function in the rower.cpp
        emit debug(QStringLiteral("Current Speed: ") + QString::number(Speed.value()));
    }

    if (Flags.avgPace) {

        double avgPace;
        avgPace =
            ((double)(((uint16_t)((uint8_t)newValue.at(index + 1)) << 8) | (uint16_t)((uint8_t)newValue.at(index))));
        index += 2;
        emit debug(QStringLiteral("Current Average Pace: ") + QString::number(avgPace));
    }

    if (Flags.instantPower) {
        m_watt =
            ((double)(((uint16_t)((uint8_t)newValue.at(index + 1)) << 8) | (uint16_t)((uint8_t)newValue.at(index))));
        index += 2;
        emit debug(QStringLiteral("Current Watt: ") + QString::number(m_watt.value()));
    }

    if (Flags.avgPower) {

        double avgPower;
        avgPower =
            ((double)(((uint16_t)((uint8_t)newValue.at(index + 1)) << 8) | (uint16_t)((uint8_t)newValue.at(index))));
        index += 2;
        emit debug(QStringLiteral("Current Average Watt: ") + QString::number(avgPower));
    }

    if (Flags.resistanceLvl) {
        Resistance =
            ((double)(((uint16_t)((uint8_t)newValue.at(index + 1)) << 8) | (uint16_t)((uint8_t)newValue.at(index))));
        emit resistanceRead(Resistance.value());
        index += 2;
        emit debug(QStringLiteral("Current Resistance: ") + QString::number(Resistance.value()));
    }

    if (Flags.expEnergy) {

        KCal = ((double)(((uint16_t)((uint8_t)newValue.at(index + 1)) << 8) | (uint16_t)((uint8_t)newValue.at(index))));
        index += 2;

        // energy per hour
        index += 2;

        // energy per minute
        index += 1;
    } else {
        if (watts())
            KCal +=
                ((((0.048 * ((double)watts()) + 1.19) * settings.value(QStringLiteral("weight"), 75.0).toFloat() *
                   3.5) /
                  200.0) /
                 (60000.0 / ((double)lastRefreshCharacteristicChanged.msecsTo(
                                QDateTime::currentDateTime())))); //(( (0.048* Output in watts +1.19) * body weight in
                                                                  // kg * 3.5) / 200 ) / 60
    }

    emit debug(QStringLiteral("Current KCal: ") + QString::number(KCal.value()));

#ifdef Q_OS_ANDROID
    if (settings.value("ant_heart", false).toBool())
        Heart = (uint8_t)KeepAwakeHelper::heart();
    else
#endif
    {
        if (Flags.heartRate) {
            if (index < newValue.length()) {
                Heart = ((double)((newValue.at(index))));
                // index += 1; //NOTE: clang-analyzer-deadcode.DeadStores
                emit debug(QStringLiteral("Current Heart: ") + QString::number(Heart.value()));
            } else
                emit debug(QStringLiteral("Error on parsing heart"));
        }
    }

    if (Flags.metabolic) {

        // todo
    }

    if (Flags.elapsedTime) {

        // todo
    }

    if (Flags.remainingTime) {

        // todo
    }

    if (Cadence.value() > 0) {

        CrankRevs++;
        LastCrankEventTime += (uint16_t)(1024.0 / (((double)(Cadence.value())) / 60.0));
    }

    lastRefreshCharacteristicChanged = QDateTime::currentDateTime();

    if (heartRateBeltName.startsWith(QStringLiteral("Disabled"))) {

#ifdef Q_OS_IOS
#ifndef IO_UNDER_QT
        lockscreen h;
        long appleWatchHeartRate = h.heartRate();
        h.setKcal(KCal.value());
        h.setDistance(Distance.value());
        Heart = appleWatchHeartRate;
        debug("Current Heart from Apple Watch: " + QString::number(appleWatchHeartRate));
#endif
#endif
    }

#ifdef Q_OS_IOS
#ifndef IO_UNDER_QT
    bool cadence = settings.value("bike_cadence_sensor", false).toBool();
    bool ios_peloton_workaround = settings.value("ios_peloton_workaround", true).toBool();
    if (ios_peloton_workaround && cadence && h && firstStateChanged) {

        h->virtualbike_setCadence(currentCrankRevolutions(), lastCrankEventTime());
        h->virtualbike_setHeartRate((uint8_t)metrics_override_heartrate());
    }
#endif
#endif

    emit debug(QStringLiteral("Current CrankRevs: ") + QString::number(CrankRevs));
    emit debug(QStringLiteral("Last CrankEventTime: ") + QString::number(LastCrankEventTime));

    if (m_control->error() != QLowEnergyController::NoError) {
        qDebug() << QStringLiteral("QLowEnergyController ERROR!!") << m_control->errorString();
    }
}

void ftmsrower::stateChanged(QLowEnergyService::ServiceState state) {

    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceState>();
    emit debug(QStringLiteral("BTLE stateChanged ") + QString::fromLocal8Bit(metaEnum.valueToKey(state)));

    for (QLowEnergyService *s : qAsConst(gattCommunicationChannelService)) {
        qDebug() << QStringLiteral("stateChanged") << s->serviceUuid() << s->state();
        if (s->state() != QLowEnergyService::ServiceDiscovered && s->state() != QLowEnergyService::InvalidService) {
            qDebug() << QStringLiteral("not all services discovered");

            return;
        }
    }

    qDebug() << QStringLiteral("all services discovered!");

    for (QLowEnergyService *s : qAsConst(gattCommunicationChannelService)) {
        if (s->state() == QLowEnergyService::ServiceDiscovered) {

            // establish hook into notifications
            connect(s, &QLowEnergyService::characteristicChanged, this, &ftmsrower::characteristicChanged);
            connect(s, &QLowEnergyService::characteristicWritten, this, &ftmsrower::characteristicWritten);
            connect(s, &QLowEnergyService::characteristicRead, this, &ftmsrower::characteristicRead);
            connect(
                s, static_cast<void (QLowEnergyService::*)(QLowEnergyService::ServiceError)>(&QLowEnergyService::error),
                this, &ftmsrower::errorService);
            connect(s, &QLowEnergyService::descriptorWritten, this, &ftmsrower::descriptorWritten);
            connect(s, &QLowEnergyService::descriptorRead, this, &ftmsrower::descriptorRead);

            qDebug() << s->serviceUuid() << QStringLiteral("connected!");

            auto characteristics_list = s->characteristics();
            for (const QLowEnergyCharacteristic &c : qAsConst(characteristics_list)) {
                qDebug() << "char uuid" << c.uuid() << QStringLiteral("handle") << c.handle();
                auto descriptors_list = c.descriptors();
                for (const QLowEnergyDescriptor &d : qAsConst(descriptors_list)) {
                    qDebug() << QStringLiteral("descriptor uuid") << d.uuid() << QStringLiteral("handle") << d.handle();
                }

                if ((c.properties() & QLowEnergyCharacteristic::Notify) == QLowEnergyCharacteristic::Notify) {

                    QByteArray descriptor;
                    descriptor.append((char)0x01);
                    descriptor.append((char)0x00);
                    if (c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration).isValid()) {
                        s->writeDescriptor(c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
                    } else {
                        qDebug() << QStringLiteral("ClientCharacteristicConfiguration") << c.uuid()
                                 << c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration).uuid()
                                 << c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration).handle()
                                 << QStringLiteral(" is not valid");
                    }

                    qDebug() << s->serviceUuid() << c.uuid() << QStringLiteral("notification subscribed!");
                } else if ((c.properties() & QLowEnergyCharacteristic::Indicate) ==
                           QLowEnergyCharacteristic::Indicate) {
                    QByteArray descriptor;
                    descriptor.append((char)0x02);
                    descriptor.append((char)0x00);
                    if (c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration).isValid()) {
                        s->writeDescriptor(c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
                    } else {
                        qDebug() << QStringLiteral("ClientCharacteristicConfiguration") << c.uuid()
                                 << c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration).uuid()
                                 << c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration).handle()
                                 << QStringLiteral(" is not valid");
                    }

                    qDebug() << s->serviceUuid() << c.uuid() << QStringLiteral("indication subscribed!");
                } else if ((c.properties() & QLowEnergyCharacteristic::Read) == QLowEnergyCharacteristic::Read) {
                    // s->readCharacteristic(c);
                    // qDebug() << s->serviceUuid() << c.uuid() << "reading!";
                }

                QBluetoothUuid _gattWriteCharControlPointId((quint16)0x2AD9);
                if (c.properties() & QLowEnergyCharacteristic::Write && c.uuid() == _gattWriteCharControlPointId) {
                    qDebug() << QStringLiteral("FTMS service and Control Point found");

                    gattWriteCharControlPointId = c;
                    gattFTMSService = s;
                }
            }
        }
    }

    // ******************************************* virtual bike init *************************************
    if (!firstStateChanged && !virtualBike
#ifdef Q_OS_IOS
#ifndef IO_UNDER_QT
        && !h
#endif
#endif
    ) {

        QSettings settings;
        bool virtual_device_enabled = settings.value(QStringLiteral("virtual_device_enabled"), true).toBool();
#ifdef Q_OS_IOS
#ifndef IO_UNDER_QT
        bool cadence = settings.value("bike_cadence_sensor", false).toBool();
        bool ios_peloton_workaround = settings.value("ios_peloton_workaround", true).toBool();
        if (ios_peloton_workaround && cadence) {

            qDebug() << "ios_peloton_workaround activated!";
            h = new lockscreen();
            h->virtualbike_ios();
        } else

#endif
#endif
            if (virtual_device_enabled) {
            emit debug(QStringLiteral("creating virtual bike interface..."));

            virtualBike = new virtualbike(this, noWriteResistance, noHeartService);
            // connect(virtualBike,&virtualbike::debug ,this,&ftmsrower::debug);
        }
    }
    firstStateChanged = 1;
    // ********************************************************************************************************
}

void ftmsrower::descriptorWritten(const QLowEnergyDescriptor &descriptor, const QByteArray &newValue) {
    emit debug(QStringLiteral("descriptorWritten ") + descriptor.name() + " " + newValue.toHex(' '));

    initRequest = true;
    emit connectedAndDiscovered();
}

void ftmsrower::descriptorRead(const QLowEnergyDescriptor &descriptor, const QByteArray &newValue) {
    qDebug() << QStringLiteral("descriptorRead ") << descriptor.name() << descriptor.uuid() << newValue.toHex(' ');
}

void ftmsrower::characteristicWritten(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue) {

    Q_UNUSED(characteristic);
    emit debug(QStringLiteral("characteristicWritten ") + newValue.toHex(' '));
}

void ftmsrower::characteristicRead(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue) {
    qDebug() << QStringLiteral("characteristicRead ") << characteristic.uuid() << newValue.toHex(' ');
}

void ftmsrower::serviceScanDone(void) {
    emit debug(QStringLiteral("serviceScanDone"));

#ifdef Q_OS_ANDROID
    QLowEnergyConnectionParameters c;
    c.setIntervalRange(24, 40);
    c.setLatency(0);
    c.setSupervisionTimeout(420);
    m_control->requestConnectionUpdate(c);
#endif

    auto services_list = m_control->services();
    for (const QBluetoothUuid &s : qAsConst(services_list)) {
        gattCommunicationChannelService.append(m_control->createServiceObject(s));
        connect(gattCommunicationChannelService.constLast(), &QLowEnergyService::stateChanged, this,
                &ftmsrower::stateChanged);
        gattCommunicationChannelService.constLast()->discoverDetails();
    }
}

void ftmsrower::errorService(QLowEnergyService::ServiceError err) {

    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceError>();
    emit debug(QStringLiteral("ftmsrower::errorService") + QString::fromLocal8Bit(metaEnum.valueToKey(err)) +
               m_control->errorString());
}

void ftmsrower::error(QLowEnergyController::Error err) {

    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyController::Error>();
    emit debug(QStringLiteral("ftmsrower::error") + QString::fromLocal8Bit(metaEnum.valueToKey(err)) +
               m_control->errorString());
}

void ftmsrower::deviceDiscovered(const QBluetoothDeviceInfo &device) {
    emit debug(QStringLiteral("Found new device: ") + device.name() + QStringLiteral(" (") +
               device.address().toString() + ')');
    {
        bluetoothDevice = device;

        m_control = QLowEnergyController::createCentral(bluetoothDevice, this);
        connect(m_control, &QLowEnergyController::serviceDiscovered, this, &ftmsrower::serviceDiscovered);
        connect(m_control, &QLowEnergyController::discoveryFinished, this, &ftmsrower::serviceScanDone);
        connect(m_control,
                static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, &ftmsrower::error);
        connect(m_control, &QLowEnergyController::stateChanged, this, &ftmsrower::controllerStateChanged);

        connect(m_control,
                static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, [this](QLowEnergyController::Error error) {
                    Q_UNUSED(error);
                    Q_UNUSED(this);
                    emit debug(QStringLiteral("Cannot connect to remote device."));
                    emit disconnected();
                });
        connect(m_control, &QLowEnergyController::connected, this, [this]() {
            Q_UNUSED(this);
            emit debug(QStringLiteral("Controller connected. Search services..."));
            m_control->discoverServices();
        });
        connect(m_control, &QLowEnergyController::disconnected, this, [this]() {
            Q_UNUSED(this);
            emit debug(QStringLiteral("LowEnergy controller disconnected"));
            emit disconnected();
        });

        // Connect
        m_control->connectToDevice();
        return;
    }
}

bool ftmsrower::connected() {
    if (!m_control) {

        return false;
    }
    return m_control->state() == QLowEnergyController::DiscoveredState;
}

void *ftmsrower::VirtualBike() { return virtualBike; }

void *ftmsrower::VirtualDevice() { return VirtualBike(); }

uint16_t ftmsrower::watts() {
    if (currentCadence().value() == 0) {
        return 0;
    }

    return m_watt.value();
}

void ftmsrower::controllerStateChanged(QLowEnergyController::ControllerState state) {
    qDebug() << QStringLiteral("controllerStateChanged") << state;
    if (state == QLowEnergyController::UnconnectedState && m_control) {
        qDebug() << QStringLiteral("trying to connect back again...");

        initDone = false;
        m_control->connectToDevice();
    }
}
