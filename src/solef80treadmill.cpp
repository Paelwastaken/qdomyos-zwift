#include "solef80treadmill.h"

#include "ios/lockscreen.h"
#include "virtualtreadmill.h"
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

QBluetoothUuid _gattWriteCharCustomService(QStringLiteral("49535343-fe7d-4ae5-8fa9-9fafd205e455"));
QBluetoothUuid _gattWriteCharControlPointId(QStringLiteral("49535343-8841-43f4-a8d4-ecbe34729bb3"));
QBluetoothUuid _gattNotifyCharId(QStringLiteral("49535343-1e4d-4bd9-ba61-23c647249616"));

#ifdef Q_OS_IOS
extern quint8 QZ_EnableDiscoveryCharsAndDescripttors;
#endif

solef80treadmill::solef80treadmill(bool noWriteResistance, bool noHeartService) {
#ifdef Q_OS_IOS
    QZ_EnableDiscoveryCharsAndDescripttors = true;
#endif

    m_watt.setType(metric::METRIC_WATT);
    Speed.setType(metric::METRIC_SPEED);
    refresh = new QTimer(this);
    this->noWriteResistance = noWriteResistance;
    this->noHeartService = noHeartService;
    initDone = false;
    connect(refresh, &QTimer::timeout, this, &solef80treadmill::update);
    refresh->start(200ms);
}

void solef80treadmill::writeCharacteristic(uint8_t *data, uint8_t data_len, QString info, bool disable_log,
                                           bool wait_for_response) {
    QEventLoop loop;
    QTimer timeout;

    if (!gattCustomService) {
        qDebug() << "no gattCustomService available";
        return;
    }

    if (wait_for_response) {
        connect(this, &solef80treadmill::packetReceived, &loop, &QEventLoop::quit);
        timeout.singleShot(2000, &loop, SLOT(quit()));
    } else {
        connect(gattCustomService, SIGNAL(characteristicWritten(QLowEnergyCharacteristic, QByteArray)), &loop,
                SLOT(quit()));
        timeout.singleShot(2000, &loop, SLOT(quit()));
    }

    gattCustomService->writeCharacteristic(gattWriteCharCustomService, QByteArray((const char *)data, data_len));

    if (!disable_log)
        qDebug() << " >> " << QByteArray((const char *)data, data_len).toHex(' ') << " // " << info;

    loop.exec();
}

void solef80treadmill::btinit() {
    uint8_t initData01[] = {0x5b, 0x01, 0xf0, 0x5d};
    uint8_t initData02[] = {0x5b, 0x02, 0x03, 0x01, 0x5d};
    /*uint8_t initData03[] = {0x5b, 0x04, 0x00, 0x09, 0x4f, 0x4b, 0x5d};
    uint8_t initData04[] = {0x5b, 0x06, 0x07, 0x01, 0x23, 0x00, 0x9b, 0x43, 0x5d};
    uint8_t initData05[] = {0x5b, 0x03, 0x08, 0x10, 0x01, 0x5d};
    uint8_t initData06[] = {0x5b, 0x05, 0x04, 0x0a, 0x00, 0x00, 0x00, 0x5d};
    uint8_t initData07[] = {0x5b, 0x02, 0x22, 0x09, 0x5d};
    uint8_t initData08[] = {0x5b, 0x02, 0x02, 0x02, 0x5d};
    uint8_t initData09[] = {0x5b, 0x04, 0x00, 0x10, 0x4f, 0x4b, 0x5d};*/
    // uint8_t initData10[] = {0x5b, 0x02, 0x03, 0x04, 0x5d};

    if (gattCustomService) {
        writeCharacteristic(initData01, sizeof(initData01), QStringLiteral("init1"), false, true);
        waitForAPacket();
        writeCharacteristic(initData02, sizeof(initData02), QStringLiteral("init2"), false, true);
        writeCharacteristic(initData02, sizeof(initData02), QStringLiteral("init2"), false, true);
        writeCharacteristic(initData02, sizeof(initData02), QStringLiteral("init2"), false, true);

        /*
         * from here on seems that the treadmill started with a speed
        writeCharacteristic(initData03, sizeof(initData03), QStringLiteral("init3"), false, true);
        writeCharacteristic(initData04, sizeof(initData04), QStringLiteral("init4"), false, true);
        writeCharacteristic(initData05, sizeof(initData05), QStringLiteral("init5"), false, true);
        writeCharacteristic(initData05, sizeof(initData05), QStringLiteral("init5"), false, true);
        writeCharacteristic(initData06, sizeof(initData06), QStringLiteral("init6"), false, true);
        writeCharacteristic(initData07, sizeof(initData07), QStringLiteral("init7"), false, true);
        writeCharacteristic(initData08, sizeof(initData08), QStringLiteral("init8"), false, true);
        writeCharacteristic(initData09, sizeof(initData09), QStringLiteral("init9"), false, true);
        */

        // the treadmill auto start to a workout, i need to figure out which lines start it
        // writeCharacteristic(initData10, sizeof(initData10), QStringLiteral("init10"), false, true);
        // writeCharacteristic(initData10, sizeof(initData10), QStringLiteral("init10"), false, true);
        // writeCharacteristic(initData10, sizeof(initData10), QStringLiteral("init10"), false, true);
    }

    initDone = true;
}

void solef80treadmill::waitForAPacket() {
    QEventLoop loop;
    QTimer timeout;
    connect(this, &solef80treadmill::packetReceived, &loop, &QEventLoop::quit);
    timeout.singleShot(3000, &loop, SLOT(quit()));
    loop.exec();
}

void solef80treadmill::update() {
    if (m_control->state() == QLowEnergyController::UnconnectedState) {

        emit disconnected();
        return;
    }

    if (initRequest && firstStateChanged) {
        btinit();
        initRequest = false;
    } else if (bluetoothDevice.isValid() //&&

               // m_control->state() == QLowEnergyController::DiscoveredState //&&
               // gattCommunicationChannelService &&
               // gattWriteCharacteristic.isValid() &&
               // gattNotify1Characteristic.isValid() &&
               /*initDone*/) {

        QSettings settings;
        update_metrics(true, watts(settings.value(QStringLiteral("weight"), 75.0).toFloat()));

        // updating the treadmill console every second
        if (sec1Update++ == (1000 / refresh->interval())) {

            sec1Update = 0;
            // updateDisplay(elapsed);
            uint8_t noop[] = {0x5b, 0x04, 0x00, 0x10, 0x4f, 0x4b, 0x5d};

            if (gattCustomService) {
                writeCharacteristic(noop, sizeof(noop), QStringLiteral("noop"), false, true);
            }
        }

        if (requestSpeed != -1) {
            if (requestSpeed != currentSpeed().value() && requestSpeed >= 0 && requestSpeed <= 22) {
                emit debug(QStringLiteral("writing speed ") + QString::number(requestSpeed));
                forceSpeed(requestSpeed);
            }
            requestSpeed = -1;
        }
        if (requestInclination != -1) {
            if (requestInclination != currentInclination().value() && requestInclination >= 0 &&
                requestInclination <= 15) {
                emit debug(QStringLiteral("writing incline ") + QString::number(requestInclination));
                forceIncline(requestInclination);
            }
            requestInclination = -1;
        }
        if (requestStart != -1) {
            emit debug(QStringLiteral("starting..."));
            if (lastSpeed == 0.0) {

                lastSpeed = 0.5;
            }
            uint8_t start[] = {0x5b, 0x04, 0x00, 0x40, 0x4f, 0x4b, 0x5d};

            if (gattCustomService) {
                writeCharacteristic(start, sizeof(start), QStringLiteral("start"), false, true);
            }
            requestStart = -1;
            emit tapeStarted();
        }
        if (requestStop != -1) {
            emit debug(QStringLiteral("stopping..."));

            requestStop = -1;
        }
        if (requestIncreaseFan != -1) {
            emit debug(QStringLiteral("increasing fan speed..."));

            // sendChangeFanSpeed(FanSpeed + 1);
            requestIncreaseFan = -1;
        } else if (requestDecreaseFan != -1) {
            emit debug(QStringLiteral("decreasing fan speed..."));

            // sendChangeFanSpeed(FanSpeed - 1);
            requestDecreaseFan = -1;
        }
    }
}

// example frame: 55aa320003050400532c00150000
void solef80treadmill::forceSpeed(double requestSpeed) {}

// example frame: 55aa3800030603005d0b0a0000
void solef80treadmill::forceIncline(double requestIncline) {}

void solef80treadmill::serviceDiscovered(const QBluetoothUuid &gatt) {
    emit debug(QStringLiteral("serviceDiscovered ") + gatt.toString());
}

void solef80treadmill::characteristicChanged(const QLowEnergyCharacteristic &characteristic,
                                             const QByteArray &newValue) {
    double heart = 0; // NOTE : Should be initialized with a value to shut clang-analyzer's
                      // UndefinedBinaryOperatorResult
    // qDebug() << "characteristicChanged" << characteristic.uuid() << newValue << newValue.length();
    Q_UNUSED(characteristic);
    QSettings settings;
    QString heartRateBeltName =
        settings.value(QStringLiteral("heart_rate_belt_name"), QStringLiteral("Disabled")).toString();

    emit debug(QStringLiteral(" << ") + characteristic.uuid().toString() + " " + QString::number(newValue.length()) +
               " " + newValue.toHex(' '));

    if (characteristic.uuid() == _gattNotifyCharId)
        emit packetReceived();

    if (characteristic.uuid() == _gattNotifyCharId && newValue.length() == 18) {

        // the treadmill send the speed in miles always
        Speed = ((double)((uint8_t)newValue.at(10)) / 10.0) * 1.60934;
        emit debug(QStringLiteral("Current Speed: ") + QString::number(Speed.value()));

        Inclination = (double)((uint8_t)newValue.at(11));
        emit debug(QStringLiteral("Current Inclination: ") + QString::number(Inclination.value()));

        Distance += ((Speed.value() / 3600000.0) *
                     ((double)lastRefreshCharacteristicChanged.msecsTo(QDateTime::currentDateTime())));

        if (watts(settings.value(QStringLiteral("weight"), 75.0).toFloat()))
            KCal +=
                ((((0.048 * ((double)watts(settings.value(QStringLiteral("weight"), 75.0).toFloat())) + 1.19) *
                   settings.value(QStringLiteral("weight"), 75.0).toFloat() * 3.5) /
                  200.0) /
                 (60000.0 / ((double)lastRefreshCharacteristicChanged.msecsTo(
                                QDateTime::currentDateTime())))); //(( (0.048* Output in watts +1.19) * body weight in
                                                                  // kg * 3.5) / 200 ) / 60
    } else if (characteristic.uuid() == _gattNotifyCharId && newValue.length() == 5 && newValue.at(0) == 0x5b &&
               newValue.at(1) == 0x02 && newValue.at(2) == 0x03) {
        // stop event from the treadmill
        qDebug() << "stop/pause event detected from the treadmill";
        initRequest = true;

    } else if (characteristic.uuid() == QBluetoothUuid((quint16)0x2ACD)) {
        lastPacket = newValue;

        // default flags for this treadmill is 84 04

        union flags {
            struct {

                uint16_t moreData : 1;
                uint16_t avgSpeed : 1;
                uint16_t totalDistance : 1;
                uint16_t inclination : 1;
                uint16_t elevation : 1;
                uint16_t instantPace : 1;
                uint16_t averagePace : 1;
                uint16_t expEnergy : 1;
                uint16_t heartRate : 1;
                uint16_t metabolic : 1;
                uint16_t elapsedTime : 1;
                uint16_t remainingTime : 1;
                uint16_t forceBelt : 1;
                uint16_t spare : 3;
            };

            uint16_t word_flags;
        };

        flags Flags;
        int index = 0;
        Flags.word_flags = (newValue.at(1) << 8) | newValue.at(0);
        index += 2;

        if (!Flags.moreData) {
            Speed = ((double)(((uint16_t)((uint8_t)newValue.at(index + 1)) << 8) |
                              (uint16_t)((uint8_t)newValue.at(index)))) /
                    100.0;
            index += 2;
            emit debug(QStringLiteral("Current Speed: ") + QString::number(Speed.value()));
        }

        if (Flags.avgSpeed) {
            double avgSpeed;
            avgSpeed = ((double)(((uint16_t)((uint8_t)newValue.at(index + 1)) << 8) |
                                 (uint16_t)((uint8_t)newValue.at(index)))) /
                       100.0;
            index += 2;
            emit debug(QStringLiteral("Current Average Speed: ") + QString::number(avgSpeed));
        }

        if (Flags.totalDistance) {
            // ignoring the distance, because it's a total life odometer
            // Distance = ((double)((((uint32_t)((uint8_t)newValue.at(index + 2)) << 16) |
            // (uint32_t)((uint8_t)newValue.at(index + 1)) << 8) | (uint32_t)((uint8_t)newValue.at(index)))) / 1000.0;
            index += 3;
        }
        // else
        {
            Distance += ((Speed.value() / 3600000.0) *
                         ((double)lastRefreshCharacteristicChanged.msecsTo(QDateTime::currentDateTime())));
        }

        emit debug(QStringLiteral("Current Distance: ") + QString::number(Distance.value()));

        if (Flags.inclination) {
            Inclination = ((double)(((uint16_t)((uint8_t)newValue.at(index + 1)) << 8) |
                                    (uint16_t)((uint8_t)newValue.at(index)))) /
                          10.0;
            index += 4; // the ramo value is useless
            emit debug(QStringLiteral("Current Inclination: ") + QString::number(Inclination.value()));
        }

        if (Flags.elevation) {
            index += 4; // TODO
        }

        if (Flags.instantPace) {
            index += 1; // TODO
        }

        if (Flags.averagePace) {
            index += 1; // TODO
        }

        if (Flags.expEnergy) {
            KCal = ((double)(((uint16_t)((uint8_t)newValue.at(index + 1)) << 8) |
                             (uint16_t)((uint8_t)newValue.at(index))));
            index += 2;

            // energy per hour
            index += 2;

            // energy per minute
            index += 1;
        } else {
            if (watts(settings.value(QStringLiteral("weight"), 75.0).toFloat()))
                KCal += ((((0.048 * ((double)watts(settings.value(QStringLiteral("weight"), 75.0).toFloat())) + 1.19) *
                           settings.value(QStringLiteral("weight"), 75.0).toFloat() * 3.5) /
                          200.0) /
                         (60000.0 /
                          ((double)lastRefreshCharacteristicChanged.msecsTo(
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

                    heart = ((double)((newValue.at(index))));
                    emit debug(QStringLiteral("Current Heart: ") + QString::number(heart));
                } else {
                    emit debug(QStringLiteral("Error on parsing heart!"));
                }
                // index += 1; //NOTE: clang-analyzer-deadcode.DeadStores
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

        if (Flags.forceBelt) {
            // todo
        }
    }

    if (heartRateBeltName.startsWith(QStringLiteral("Disabled"))) {
        if (heart == 0.0 || settings.value(QStringLiteral("heart_ignore_builtin"), false).toBool()) {

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
        } else {

            Heart = heart;
        }
    }

    lastRefreshCharacteristicChanged = QDateTime::currentDateTime();

    if (m_control->error() != QLowEnergyController::NoError) {
        qDebug() << QStringLiteral("QLowEnergyController ERROR!!") << m_control->errorString();
    }
}

void solef80treadmill::stateChanged(QLowEnergyService::ServiceState state) {
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
            connect(s, &QLowEnergyService::characteristicChanged, this, &solef80treadmill::characteristicChanged);
            connect(s, &QLowEnergyService::characteristicWritten, this, &solef80treadmill::characteristicWritten);
            connect(s, &QLowEnergyService::characteristicRead, this, &solef80treadmill::characteristicRead);
            connect(
                s, static_cast<void (QLowEnergyService::*)(QLowEnergyService::ServiceError)>(&QLowEnergyService::error),
                this, &solef80treadmill::errorService);
            connect(s, &QLowEnergyService::descriptorWritten, this, &solef80treadmill::descriptorWritten);
            connect(s, &QLowEnergyService::descriptorRead, this, &solef80treadmill::descriptorRead);

            qDebug() << s->serviceUuid() << QStringLiteral("connected!");

            auto characteristics_list = s->characteristics();
            for (const QLowEnergyCharacteristic &c : qAsConst(characteristics_list)) {
                qDebug() << QStringLiteral("char uuid") << c.uuid() << QStringLiteral("handle") << c.handle();
                auto descriptors_list = c.descriptors();
                for (const QLowEnergyDescriptor &d : qAsConst(descriptors_list)) {
                    qDebug() << QStringLiteral("descriptor uuid") << d.uuid() << QStringLiteral("handle") << d.handle();
                }

                if ((c.properties() & QLowEnergyCharacteristic::Notify) == QLowEnergyCharacteristic::Notify &&
                    c.uuid() == _gattNotifyCharId) {
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
                    /*} else if ((c.properties() & QLowEnergyCharacteristic::Indicate) ==
                               QLowEnergyCharacteristic::Indicate) {
                        QByteArray descriptor;
                        descriptor.append((char)0x02);
                        descriptor.append((char)0x00);
                        if (c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration).isValid()) {
                            s->writeDescriptor(c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration),
                       descriptor); } else { qDebug() << QStringLiteral("ClientCharacteristicConfiguration") << c.uuid()
                                     << c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration).uuid()
                                     << c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration).handle()
                                     << QStringLiteral(" is not valid");
                        }

                        qDebug() << s->serviceUuid() << c.uuid() << QStringLiteral("indication subscribed!");*/
                } else if ((c.properties() & QLowEnergyCharacteristic::Read) == QLowEnergyCharacteristic::Read) {
                    // s->readCharacteristic(c);
                    // qDebug() << s->serviceUuid() << c.uuid() << "reading!";
                }

                if (c.properties() & QLowEnergyCharacteristic::Write && c.uuid() == _gattWriteCharControlPointId) {
                    qDebug() << QStringLiteral("Custom service and Control Point found");
                    gattWriteCharCustomService = c;
                    gattCustomService = s;
                }
            }
        }
    }

    // ******************************************* virtual treadmill init *************************************
    if (!firstStateChanged && !virtualTreadmill
#ifdef Q_OS_IOS
#ifndef IO_UNDER_QT
        && !h
#endif
#endif
    ) {

        QSettings settings;
        bool virtual_device_enabled = settings.value(QStringLiteral("virtual_device_enabled"), true).toBool();
        if (virtual_device_enabled) {
            emit debug(QStringLiteral("creating virtual treadmill interface..."));

            virtualTreadmill = new virtualtreadmill(this, noHeartService);
            connect(virtualTreadmill, &virtualtreadmill::debug, this, &solef80treadmill::debug);
        }
    }
    firstStateChanged = 1;
    // ********************************************************************************************************
}

void solef80treadmill::descriptorWritten(const QLowEnergyDescriptor &descriptor, const QByteArray &newValue) {
    emit debug(QStringLiteral("descriptorWritten ") + descriptor.name() + QStringLiteral(" ") + newValue.toHex(' '));

    initRequest = true;
    emit connectedAndDiscovered();
}

void solef80treadmill::descriptorRead(const QLowEnergyDescriptor &descriptor, const QByteArray &newValue) {
    qDebug() << QStringLiteral("descriptorRead ") << descriptor.name() << descriptor.uuid() << newValue.toHex(' ');
}

void solef80treadmill::characteristicWritten(const QLowEnergyCharacteristic &characteristic,
                                             const QByteArray &newValue) {
    Q_UNUSED(characteristic);
    emit debug(QStringLiteral("characteristicWritten ") + newValue.toHex(' '));
}

void solef80treadmill::characteristicRead(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue) {
    qDebug() << QStringLiteral("characteristicRead ") << characteristic.uuid() << newValue.toHex(' ');
}

void solef80treadmill::serviceScanDone(void) {
    emit debug(QStringLiteral("serviceScanDone"));

    initRequest = false;
    firstStateChanged = 0;
    auto services_list = m_control->services();
    for (const QBluetoothUuid &s : qAsConst(services_list)) {
        gattCommunicationChannelService.append(m_control->createServiceObject(s));
        connect(gattCommunicationChannelService.constLast(), &QLowEnergyService::stateChanged, this,
                &solef80treadmill::stateChanged);
        gattCommunicationChannelService.constLast()->discoverDetails();
    }
}

void solef80treadmill::errorService(QLowEnergyService::ServiceError err) {

    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceError>();
    emit debug(QStringLiteral("solef80treadmill::errorService") + QString::fromLocal8Bit(metaEnum.valueToKey(err)) +
               m_control->errorString());
}

void solef80treadmill::error(QLowEnergyController::Error err) {

    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyController::Error>();
    emit debug(QStringLiteral("solef80treadmill::error") + QString::fromLocal8Bit(metaEnum.valueToKey(err)) +
               m_control->errorString());
}

void solef80treadmill::deviceDiscovered(const QBluetoothDeviceInfo &device) {

    emit debug(QStringLiteral("Found new device: ") + device.name() + QStringLiteral(" (") +
               device.address().toString() + ')');
    {
        bluetoothDevice = device;

        m_control = QLowEnergyController::createCentral(bluetoothDevice, this);
        connect(m_control, &QLowEnergyController::serviceDiscovered, this, &solef80treadmill::serviceDiscovered);
        connect(m_control, &QLowEnergyController::discoveryFinished, this, &solef80treadmill::serviceScanDone);
        connect(m_control,
                static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, &solef80treadmill::error);
        connect(m_control, &QLowEnergyController::stateChanged, this, &solef80treadmill::controllerStateChanged);

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

bool solef80treadmill::connected() {
    if (!m_control) {

        return false;
    }
    return m_control->state() == QLowEnergyController::DiscoveredState;
}

void *solef80treadmill::VirtualTreadmill() { return virtualTreadmill; }

void *solef80treadmill::VirtualDevice() { return VirtualTreadmill(); }

void solef80treadmill::controllerStateChanged(QLowEnergyController::ControllerState state) {
    qDebug() << QStringLiteral("controllerStateChanged") << state;
    if (state == QLowEnergyController::UnconnectedState && m_control) {
        qDebug() << QStringLiteral("trying to connect back again...");

        initDone = false;
        m_control->connectToDevice();
    }
}
