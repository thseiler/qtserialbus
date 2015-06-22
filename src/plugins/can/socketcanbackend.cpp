﻿/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the QtSerialBus module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPLv3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or later as published by the Free
** Software Foundation and appearing in the file LICENSE.GPL included in
** the packaging of this file. Please review the following information to
** ensure the GNU General Public License version 2.0 requirements will be
** met: http://www.gnu.org/licenses/gpl-2.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "socketcanbackend.h"
#include <QtCore/qdebug.h>
#include <QtCore/qdatastream.h>
#include <QtCore/qsocketnotifier.h>
#include <QtCore/qpair.h>

#include <linux/can/raw.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/time.h>


SocketCanBackend::SocketCanBackend(const QString &name) :
    canSocketName(name),
    version(0)
{
    QDataStream stream;
    version = stream.version();

    configuration.append(QPair<QString, QVariant>(QStringLiteral("LoopBack"), 1));
    configuration.append(QPair<QString, QVariant>(QStringLiteral("ReceiveOwnMessages"), 0));
    configuration.append(QPair<QString, QVariant>(QStringLiteral("ErrorMask"), 0));
    configuration.append(QPair<QString, QVariant>(QStringLiteral("CanFilter"), QList<QVariant>()));

    //TODO: remove implicit call to connect() in ctor (related to missing open/close() call in QSerialBusDevice
    connectSocket();
}

qint64 SocketCanBackend::read(char *buffer, qint64 maxSize)
{
    //TODO: make non-blocking
    Q_UNUSED(maxSize);
    const canfd_frame frame = readFrame();

    if (!frame.len)
        return 0;

    struct timeval timeStamp;
    if (ioctl(canSocket, SIOCGSTAMP, &timeStamp) < 0) {
        qWarning() << "ERROR SocketCanBackend: couldn't get timestamp";
        timeStamp.tv_sec = 0;
        timeStamp.tv_usec = 0;
    }
    const QByteArray data = serialize(frame, timeStamp);
    memcpy(buffer, data.constData(), data.size());

    return data.size();
}

QByteArray SocketCanBackend::serialize(const canfd_frame &frame, const timeval &timeStamp)
{
    QByteArray array;
    QDataStream stream(&array, QIODevice::WriteOnly);
    stream.setVersion(version);

    stream << frame.can_id;
    QByteArray payload;
    for (int i = 0; i < frame.len; i++)
        payload.insert(i, frame.data[i]);

    stream << payload
           << qint64(timeStamp.tv_sec)
           << qint64(timeStamp.tv_usec);
    return array;
}

canfd_frame SocketCanBackend::deserialize(const QByteArray &array)
{
    canfd_frame frame;
    QDataStream stream(array);
    stream.setVersion(version);

    QByteArray payload;

    stream >> frame.can_id
           >> payload;

    frame.len = payload.size();
    for (int i = 0; i < frame.len ; i++)
        frame.data[i] = payload.at(i);

    return frame;
}

qint64 SocketCanBackend::write(const char *buffer, qint64 size)
{
    QByteArray data;
    data.setRawData(buffer, size);
    const canfd_frame frame = deserialize(data);

    const qint64 bytesWritten = ::write(canSocket, &frame, CANFD_MTU);
    if (bytesWritten < 0) {
        qWarning() << "ERROR SocketCanBackend: cannot write to socket";
        return -1;
    }
    return bytesWritten;
}

void SocketCanBackend::insertInConfigurations(const QString &key, const QVariant &value)
{
    for (int i = 0; i < configuration.size(); i++) {
        if (configuration.at(i).first == key) {
            QPair<QString, QVariant> conf(key, value);
            configuration.removeAt(i);
            configuration.append(conf);
        }
    }
}

void SocketCanBackend::setConfigurationParameter(const QString &key, const QVariant &value)
{
    if (key == QStringLiteral("Loopback")) {
        const int loopback = value.toBool() ? 1 : 0;
        if (setsockopt(canSocket, SOL_CAN_RAW, CAN_RAW_LOOPBACK, &loopback, sizeof(loopback)) < 0) {
            qWarning() << "ERROR SocketCanBackend: setsockopt CAN_RAW_LOOPBACK failed";
            return;
        }
    } else if (key == QStringLiteral("ReceiveOwnMessages")) {
        const int receiveOwnMessages = value.toBool() ? 1 : 0;
        if (setsockopt(canSocket, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS,
                       &receiveOwnMessages, sizeof(receiveOwnMessages)) < 0) {
            qWarning() << "ERROR SocketCanBackend: setsockopt CAN_RAW_RECV_OWN_MSGS failed";
            return;
        }
    } else if (key == QStringLiteral("ErrorMask")) {
        const int errorMask = value.toInt();
        if (setsockopt(canSocket, SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
                                  &errorMask, sizeof(errorMask)) < 0) {
            qWarning() << "ERROR SocketCanBackend: setsockopt CAN_RAW_ERR_FILTER failed";
            return;
        }
    } else if (key == QStringLiteral("CanFilter")) {
        const QList<QVariant> filterList = value.toList();
        const int size = filterList.size();
        if (size == 0)
            qWarning() << "ERROR SocketCanBackend: \"CanFilter\" QList<QVariant> empty or not valid";
        can_filter filters[size];
        for (int i = 0; i < size; i++) {
            can_filter filter;
            const QHash<QString, QVariant> filterHash = filterList.at(i).toHash();
            bool ok = true;
            filter.can_id = filterHash.value("FilterId").toInt(&ok);
            if (!ok) {
                qWarning() << "ERROR SocketCanBackend: \"CanFilter\" FilterId key not found or value is not valid in index:" << i;
                return;
            }
            filter.can_mask = filterHash.value("CanMask").toInt(&ok);
            if (!ok) {
                qWarning() << "ERROR SocketCanBackend: \"CanFilter\" CanMask key not found or value is not valid in index:" << i;
                return;
            }
            filters[i] = filter;
        }
        if (setsockopt(canSocket, SOL_CAN_RAW, CAN_RAW_FILTER, filters, sizeof(filters)) < 0)
            qWarning() << "ERROR SocketCanBackend: setsockopt CAN_RAW_FILTER failed";
    } else {
        qWarning() << "SocketCanBackend: No such configuration as" << key << "in SocketCanBackend";
    }
    insertInConfigurations(key, value);
}

QVariant SocketCanBackend::configurationParameter(const QString &key) const
{
    QVariant value;
    for (int i = 0; i < configuration.size(); i++)
        if (configuration.at(i).first == key)
            value = configuration.at(i).second;

    return value;
}

QVector<QString> SocketCanBackend::configurationKeys() const
{
    QVector<QString> keys;
    for (int i = 0; i < configuration.size(); i++) {
        keys.append(configuration.at(i).first);
    }
    return keys;
}

qint64 SocketCanBackend::connectSocket()
{
    struct sockaddr_can address;
    struct ifreq interface;

    if ((canSocket = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
        qWarning() << "ERROR SocketCanBackend: cannot open socket";
        return -1;
    }

    strcpy(interface.ifr_name, canSocketName.toLatin1().data());
    if (ioctl(canSocket, SIOCGIFINDEX, &interface) < 0) {
        qWarning() << "ERROR SocketCanBackend: failed to retrieve the interface index";
        return -1;
    }

    address.can_family  = AF_CAN;
    address.can_ifindex = interface.ifr_ifindex;

    if (bind(canSocket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        qWarning() << "ERROR SocketCanBackend: cannot bind socket";
        return -2;
    }

    const int fd_frames = 1;
    if (setsockopt(canSocket, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &fd_frames, sizeof(fd_frames)) < 0) {
        qWarning() << "ERROR SocketCanBackend: setsockopt CAN_RAW_FD_FRAMES failed";
    }

    notifier = new QSocketNotifier(canSocket, QSocketNotifier::Read);
    connect(notifier.data(), &QSocketNotifier::activated, this, &SocketCanBackend::readyRead);

    //TODO: improve the error reporting
    return 0;
}

void SocketCanBackend::setDataStreamVersion(int version)
{
    version = version;
}

int SocketCanBackend::dataStreamVersion() const
{
    return version;
}

qint64 SocketCanBackend::bytesAvailable() const
{
    qint64 bytes = 0;
    if (canSocket)
        ioctl(canSocket, FIONREAD, &bytes);
    return bytes;
}

canfd_frame SocketCanBackend::readFrame()
{
    struct canfd_frame frame;
    int bytesReceived = 0;

    struct timeval timeout = {0, 0};
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(canSocket, &readSet);
    if (select((canSocket + 1), &readSet, NULL, NULL, &timeout) >= 0) {
        if (FD_ISSET(canSocket, &readSet)) {
            bytesReceived = ::read(canSocket, &frame, CANFD_MTU);
            if (bytesReceived)
                return frame;
        }
    }
    frame.len = 0;
    return frame;
}
