#pragma once

#include <scpi/scpiclient.h>

#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

namespace scpi::test {

using scpi::ScpiClient;
using Error = ScpiClient::Error;
using State = ScpiClient::SessionState;
using Availability = ScpiClient::Availability;
using Format = ScpiClient::ResponseFormat;
using OperationId = ScpiClient::OperationId;

class Instrument : public QObject
{
public:
    Instrument()
    {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *peer = server.nextPendingConnection()) {
                const auto index = peers.size();
                peers.append(peer);
                peer->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                connect(peer, &QTcpSocket::readyRead, this, [this, peer, index] {
                    while (peer->canReadLine()) {
                        auto line = peer->readLine();
                        line.chop(1);
                        commands.append(line);
                        commandConnections.append(index);
                    }
                });
                connect(peer, &QTcpSocket::disconnected, this, [this] { ++disconnections; });
            }
        });
    }

    bool listen(quint16 port = 0)
    {
        return server.listen(QHostAddress::LocalHost, port);
    }

    quint16 port() const
    {
        return server.serverPort();
    }

    void write(qsizetype index, const QByteArray &bytes)
    {
        QCOMPARE(peers.at(index)->write(bytes), bytes.size());
    }

    QList<QTcpSocket *> peers;
    QList<QByteArray> commands;
    QList<qsizetype> commandConnections;
    int disconnections = 0;
    QTcpServer server;
};

inline void expectSuccess(const QSignalSpy &spy, qsizetype row, OperationId id, const QByteArray &payload = {})
{
    QVERIFY(row < spy.size());
    QCOMPARE(spy.at(row).at(0).toULongLong(), id);
    QCOMPARE(spy.at(row).at(1).toByteArray(), payload);
}

inline void expectFailure(const QSignalSpy &spy, qsizetype row, OperationId id, Error error)
{
    QVERIFY(row < spy.size());
    QCOMPARE(spy.at(row).at(0).toULongLong(), id);
    QCOMPARE(spy.at(row).at(1).value<Error>(), error);
    QVERIFY(!spy.at(row).at(2).toString().isEmpty());
}

inline void expectCheck(const QSignalSpy &spy, qsizetype row, OperationId id, Error error = Error::None)
{
    QVERIFY(row < spy.size());
    QCOMPARE(spy.at(row).at(0).toULongLong(), id);
    QCOMPARE(spy.at(row).at(1).toBool(), error == Error::None);
    QCOMPARE(spy.at(row).at(2).value<Error>(), error);
    QCOMPARE(spy.at(row).at(3).toString().isEmpty(), error == Error::None);
}

} // namespace scpi::test
