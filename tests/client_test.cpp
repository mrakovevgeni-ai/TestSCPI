#include <scpi/scpiclient.h>

#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using scpi::ScpiClient;
using Error = ScpiClient::Error;
using State = ScpiClient::SessionState;
using Availability = ScpiClient::Availability;
using Format = ScpiClient::ResponseFormat;
using OperationId = ScpiClient::OperationId;

namespace {

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

    bool listen()
    {
        return server.listen(QHostAddress::LocalHost);
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

void expectSuccess(const QSignalSpy &spy, qsizetype row, OperationId id, const QByteArray &payload = {})
{
    QVERIFY(row < spy.size());
    QCOMPARE(spy.at(row).at(0).toULongLong(), id);
    QCOMPARE(spy.at(row).at(1).toByteArray(), payload);
}

void expectFailure(const QSignalSpy &spy, qsizetype row, OperationId id, Error error)
{
    QVERIFY(row < spy.size());
    QCOMPARE(spy.at(row).at(0).toULongLong(), id);
    QCOMPARE(spy.at(row).at(1).value<Error>(), error);
    QVERIFY(!spy.at(row).at(2).toString().isEmpty());
}

void expectCheck(const QSignalSpy &spy, qsizetype row, OperationId id, Error error = Error::None)
{
    QVERIFY(row < spy.size());
    QCOMPARE(spy.at(row).at(0).toULongLong(), id);
    QCOMPARE(spy.at(row).at(1).toBool(), error == Error::None);
    QCOMPARE(spy.at(row).at(2).value<Error>(), error);
    QCOMPARE(spy.at(row).at(3).toString().isEmpty(), error == Error::None);
}

} // namespace

class ClientTest : public QObject
{
    Q_OBJECT

private slots:
    void fifoAndFragmentedReplies();
    void confirmedKeepsQueue_data();
    void confirmedKeepsQueue();
    void timeoutClosesSession();
    void remoteDisconnectCompletesOnce();
    void completeReplyBeforeDisconnect();
    void invalidBlockClosesSession_data();
    void invalidBlockClosesSession();
    void offlineManualChecks();
    void manualCheckUsesWorkingQueue();
    void monitoringSkipsBusySessionAndDrainsAfterStop();
    void openWaitsForOfflinePeriodicCheck();
    void offlineCheckTimeout();
    void invalidRequestsAndCloseCancellation();
    void connectionRefused();
};

void ClientTest::fifoAndFragmentedReplies()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    ScpiClient client;
    QCOMPARE(client.port(), quint16(5025));
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, instrument.port()));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);
    QSignalSpy states(&client, &ScpiClient::sessionStateChanged);

    const auto open = client.open();
    const auto set = client.send("VOLT 3\r\n");
    const auto text = client.query("MEAS?");
    const auto block = client.query("TRACE?", Format::IeeeBlockCrlf);
    const auto last = client.query("AFTER?");
    QTRY_COMPARE(instrument.commands, (QList<QByteArray>{"VOLT 3", "MEAS?"}));
    QTRY_COMPARE(done.size(), 2);
    expectSuccess(done, 0, open);
    expectSuccess(done, 1, set);
    QCOMPARE(states.size(), 2);
    QCOMPARE(states.at(0).at(0).value<State>(), State::Connecting);
    QCOMPARE(states.at(1).at(0).value<State>(), State::Connected);

    instrument.write(0, "ME");
    QTest::qWait(15);
    instrument.write(0, "AS\r");
    QTest::qWait(15);
    QCOMPARE(done.size(), 2);
    QCOMPARE(instrument.commands.size(), 2);
    instrument.write(0, "\n");

    QTRY_COMPARE(instrument.commands.size(), 3);
    QCOMPARE(instrument.commands.last(), QByteArray("TRACE?"));
    QTRY_COMPARE(done.size(), 3);
    expectSuccess(done, 2, text, "MEAS");

    for (const auto &part : QList<QByteArray>{"#", "2", "0", "5", "a\n", QByteArray("\0b\r", 3), "\r"}) {
        instrument.write(0, part);
        QTest::qWait(15);
        QCOMPARE(done.size(), 3);
        QCOMPARE(instrument.commands.size(), 3);
    }
    instrument.write(0, "\n");

    QTRY_COMPARE(instrument.commands.size(), 4);
    QCOMPARE(instrument.commands.last(), QByteArray("AFTER?"));
    QTRY_COMPARE(done.size(), 4);
    expectSuccess(done, 3, block, QByteArray("a\n\0b\r", 5));

    instrument.write(0, "OK\n");
    QTRY_COMPARE(done.size(), 5);
    expectSuccess(done, 4, last, "OK");
    QCOMPARE(failed.size(), 0);

    const auto close = client.close();
    const auto closeAgain = client.close();
    QTRY_COMPARE(done.size(), 7);
    expectSuccess(done, 5, close);
    expectSuccess(done, 6, closeAgain);
    QCOMPARE(states.size(), 3);
    QCOMPARE(states.last().at(0).value<State>(), State::Closed);
}

void ClientTest::confirmedKeepsQueue_data()
{
    QTest::addColumn<QByteArray>("partialReply");
    QTest::addColumn<Error>("error");
    QTest::newRow("one") << QByteArray("1") << Error::None;
    QTest::newRow("plus-one") << QByteArray("+1\r") << Error::None;
    QTest::newRow("complete-invalid-value") << QByteArray("0") << Error::UnexpectedResponse;
}

void ClientTest::confirmedKeepsQueue()
{
    QFETCH(QByteArray, partialReply);
    QFETCH(Error, error);

    Instrument instrument;
    QVERIFY(instrument.listen());
    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, instrument.port()));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);

    const auto open = client.open();
    const auto set = client.sendConfirmed("VOLT 5");
    const auto query = client.query("VOLT?");
    QTRY_COMPARE(instrument.commands, (QList<QByteArray>{"VOLT 5", "*OPC?"}));
    QCOMPARE(instrument.commandConnections, (QList<qsizetype>{0, 0}));
    QTRY_COMPARE(done.size(), 1);
    expectSuccess(done, 0, open);

    instrument.write(0, partialReply);
    QTest::qWait(25);
    QCOMPARE(instrument.commands.size(), 2);
    QCOMPARE(done.size(), 1);
    QCOMPARE(failed.size(), 0);
    instrument.write(0, "\n");

    QTRY_COMPARE(instrument.commands.size(), 3);
    QCOMPARE(instrument.commands.last(), QByteArray("VOLT?"));
    const auto successCount = error == Error::None ? 2 : 1;
    if (error == Error::None) {
        QTRY_COMPARE(done.size(), successCount);
        expectSuccess(done, 1, set);
    } else {
        QTRY_COMPARE(failed.size(), 1);
        expectFailure(failed, 0, set, error);
    }

    instrument.write(0, "5.0\n");
    QTRY_COMPARE(done.size(), successCount + 1);
    expectSuccess(done, successCount, query, "5.0");
    QCOMPARE(failed.size(), error == Error::None ? 0 : 1);
    QCOMPARE(client.sessionState(), State::Connected);
}

void ClientTest::timeoutClosesSession()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, instrument.port()));
    constexpr int replyTimeoutMs = 300;
    QVERIFY(client.setTimeouts({1000, replyTimeoutMs, 500}));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);
    QSignalSpy checks(&client, &ScpiClient::availabilityCheckFinished);
    QSignalSpy states(&client, &ScpiClient::sessionStateChanged);

    const auto open = client.open();
    const auto slow = client.query("SLOW?");
    const auto queued = client.query("QUEUED?");
    const auto check = client.checkAvailability();
    QTRY_COMPARE(instrument.commands.size(), 1);
    instrument.write(0, "old fragment");
    QTest::qWait(20);
    // Let the deadline pass before dispatching the client's timer or read events.
    QTest::qSleep(replyTimeoutMs + 20);
    instrument.write(0, "late reply\n");
    QVERIFY(instrument.peers.at(0)->flush());

    QTRY_COMPARE(failed.size(), 2);
    QTRY_COMPARE(checks.size(), 1);
    expectFailure(failed, 0, slow, Error::ResponseTimeout);
    expectFailure(failed, 1, queued, Error::SessionLost);
    expectCheck(checks, 0, check, Error::SessionLost);
    QCOMPARE(client.availability(), Availability::Unknown);
    QCOMPARE(client.sessionState(), State::Closed);
    QTRY_COMPARE(instrument.disconnections, 1);
    QCOMPARE(instrument.commands, QList<QByteArray>{"SLOW?"});
    expectSuccess(done, 0, open);

    QVERIFY(client.setTimeouts({1000, 1000, 500}));
    const auto reopen = client.open();
    const auto fresh = client.query("FRESH?");
    QTRY_COMPARE(instrument.commands.size(), 2);
    QCOMPARE(instrument.commandConnections, (QList<qsizetype>{0, 1}));
    QCOMPARE(done.size(), 2);
    expectSuccess(done, 1, reopen);
    instrument.write(1, "fresh reply\n");
    QTRY_COMPARE(done.size(), 3);
    expectSuccess(done, 2, fresh, "fresh reply");
    QCOMPARE(failed.size(), 2);
    QCOMPARE(checks.size(), 1);
    QCOMPARE(states.size(), 5);
}

void ClientTest::remoteDisconnectCompletesOnce()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, instrument.port()));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);
    QSignalSpy states(&client, &ScpiClient::sessionStateChanged);

    const auto open = client.open();
    const auto first = client.query("DATA?");
    const auto pending = client.send("VOLT 10");
    QTRY_COMPARE(instrument.commands.size(), 1);
    instrument.write(0, "incomplete\r");
    instrument.peers.at(0)->disconnectFromHost();

    QTRY_COMPARE(failed.size(), 2);
    expectFailure(failed, 0, first, Error::Disconnected);
    expectFailure(failed, 1, pending, Error::SessionLost);
    QCOMPARE(client.sessionState(), State::Closed);
    QTRY_COMPARE(states.size(), 3);
    QCOMPARE(states.last().at(0).value<State>(), State::Closed);
    QTest::qWait(25);
    QCOMPARE(instrument.commands, QList<QByteArray>{"DATA?"});
    QCOMPARE(done.size(), 1);
    expectSuccess(done, 0, open);
    QCOMPARE(failed.size(), 2);
    QCOMPARE(states.size(), 3);
}

void ClientTest::completeReplyBeforeDisconnect()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, instrument.port()));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);

    const auto open = client.open();
    const auto query = client.query("LAST?");
    QTRY_COMPARE(instrument.commands.size(), 1);
    instrument.write(0, "OK\n");
    instrument.peers.at(0)->disconnectFromHost();
    QTRY_COMPARE(done.size(), 2);
    expectSuccess(done, 0, open);
    expectSuccess(done, 1, query, "OK");
    QTRY_COMPARE(client.sessionState(), State::Closed);
    QCOMPARE(failed.size(), 0);
}

void ClientTest::invalidBlockClosesSession_data()
{
    QTest::addColumn<QByteArray>("block");
    QTest::addColumn<Error>("error");
    QTest::newRow("indefinite") << QByteArray("#0") << Error::UnsupportedFormat;
    QTest::newRow("wrong-terminator") << QByteArray("#11a\r") << Error::MalformedResponse;
    QTest::newRow("too-large") << QByteArray("#19") << Error::ResponseTooLarge;
}

void ClientTest::invalidBlockClosesSession()
{
    QFETCH(QByteArray, block);
    QFETCH(Error, error);

    Instrument instrument;
    QVERIFY(instrument.listen());
    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, instrument.port()));
    QVERIFY(client.setMaximumResponseSize(4));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);

    const auto open = client.open();
    const auto query = client.query("TRACE?", Format::IeeeBlockLf);
    const auto next = client.query("NEXT?");
    QTRY_COMPARE(instrument.commands.size(), 1);
    instrument.write(0, block);
    QTRY_COMPARE(failed.size(), 2);
    expectFailure(failed, 0, query, error);
    expectFailure(failed, 1, next, Error::SessionLost);
    QCOMPARE(client.sessionState(), State::Closed);
    QCOMPARE(done.size(), 1);
    expectSuccess(done, 0, open);
    QCOMPARE(instrument.commands, QList<QByteArray>{"TRACE?"});
}

void ClientTest::offlineManualChecks()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, instrument.port()));
    QVERIFY(client.setAvailabilityCommand(":PING?\r\n"));
    QCOMPARE(client.availabilityCommand(), QByteArray(":PING?"));
    QVERIFY(!client.isMonitoring());

    QSignalSpy checks(&client, &ScpiClient::availabilityCheckFinished);
    QSignalSpy availability(&client, &ScpiClient::availabilityChanged);
    QSignalSpy states(&client, &ScpiClient::sessionStateChanged);
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);

    const auto first = client.checkAvailability();
    const auto second = client.checkAvailability();
    QTRY_COMPARE(instrument.commands, QList<QByteArray>{":PING?"});
    QCOMPARE(instrument.peers.size(), 1);
    instrument.write(0, "OK\r\n");

    QTRY_COMPARE(instrument.commands, (QList<QByteArray>{":PING?", ":PING?"}));
    QCOMPARE(instrument.commandConnections, (QList<qsizetype>{0, 1}));
    instrument.write(1, "OK\n");

    QTRY_COMPARE(checks.size(), 2);
    expectCheck(checks, 0, first);
    expectCheck(checks, 1, second);
    QTRY_COMPARE(instrument.disconnections, 2);
    QCOMPARE(availability.size(), 1);
    QCOMPARE(availability.at(0).at(0).value<Availability>(), Availability::Available);
    QCOMPARE(client.availability(), Availability::Available);
    QCOMPARE(client.sessionState(), State::Closed);
    QCOMPARE(states.size(), 0);
    QCOMPARE(done.size(), 0);
    QCOMPARE(failed.size(), 0);
}

void ClientTest::manualCheckUsesWorkingQueue()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, instrument.port()));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy checks(&client, &ScpiClient::availabilityCheckFinished);

    const auto open = client.open();
    const auto first = client.query("FIRST?");
    const auto check = client.checkAvailability();
    const auto second = client.query("SECOND?");
    QTRY_COMPARE(instrument.commands, QList<QByteArray>{"FIRST?"});
    instrument.write(0, "one\n");
    QTRY_COMPARE(instrument.commands, (QList<QByteArray>{"FIRST?", "*IDN?"}));
    QCOMPARE(checks.size(), 0);
    instrument.write(0, "instrument\n");
    QTRY_COMPARE(instrument.commands, (QList<QByteArray>{"FIRST?", "*IDN?", "SECOND?"}));
    instrument.write(0, "two\n");

    QTRY_COMPARE(done.size(), 3);
    expectSuccess(done, 0, open);
    expectSuccess(done, 1, first, "one");
    expectSuccess(done, 2, second, "two");
    QTRY_COMPARE(checks.size(), 1);
    expectCheck(checks, 0, check);
    QCOMPARE(instrument.peers.size(), 1);
    QCOMPARE(client.sessionState(), State::Connected);
}

void ClientTest::monitoringSkipsBusySessionAndDrainsAfterStop()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, instrument.port()));
    QSignalSpy availability(&client, &ScpiClient::availabilityChanged);
    QSignalSpy checks(&client, &ScpiClient::availabilityCheckFinished);
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);

    const auto open = client.open();
    const auto busy = client.query("BUSY?");
    QTRY_COMPARE(instrument.commands, QList<QByteArray>{"BUSY?"});
    QVERIFY(client.startMonitoring(25));
    QTest::qWait(100);
    QCOMPARE(instrument.commands.size(), 1);
    instrument.write(0, "ready\n");

    QTRY_COMPARE(instrument.commands.size(), 2);
    instrument.write(0, "instrument\n");
    QTRY_COMPARE(availability.size(), 1);
    QCOMPARE(availability.last().at(0).value<Availability>(), Availability::Available);
    QTRY_COMPARE(instrument.commands.size(), 3);
    instrument.write(0, "same instrument\n");
    QTRY_COMPARE(instrument.commands.size(), 4);
    QCOMPARE(availability.size(), 1);

    instrument.write(0, "\r\n");
    QTRY_COMPARE(availability.size(), 2);
    QCOMPARE(availability.last().at(0).value<Availability>(), Availability::Unavailable);
    QTRY_COMPARE(instrument.commands.size(), 5);
    instrument.write(0, " \t\n");
    QTRY_COMPARE(instrument.commands.size(), 6);
    QCOMPARE(availability.size(), 2);

    client.stopMonitoring();
    QVERIFY(!client.isMonitoring());
    const auto next = client.query("NEXT?");
    instrument.write(0, "instrument\r");
    QTest::qWait(60);
    QCOMPARE(instrument.commands.size(), 6);
    QCOMPARE(availability.size(), 2);
    instrument.write(0, "\n");
    QTRY_COMPARE(availability.size(), 3);
    QCOMPARE(availability.last().at(0).value<Availability>(), Availability::Available);
    QTRY_COMPARE(instrument.commands.size(), 7);
    for (qsizetype i = 1; i <= 5; ++i)
        QCOMPARE(instrument.commands.at(i), QByteArray("*IDN?"));
    QCOMPARE(instrument.commands.last(), QByteArray("NEXT?"));
    instrument.write(0, "next reply\n");

    QTRY_COMPARE(done.size(), 3);
    expectSuccess(done, 0, open);
    expectSuccess(done, 1, busy, "ready");
    expectSuccess(done, 2, next, "next reply");
    QTest::qWait(80);
    QCOMPARE(instrument.commands.size(), 7);
    QCOMPARE(instrument.peers.size(), 1);
    QCOMPARE(availability.size(), 3);
    QCOMPARE(checks.size(), 0);
    QCOMPARE(failed.size(), 0);
}

void ClientTest::openWaitsForOfflinePeriodicCheck()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, instrument.port()));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy checks(&client, &ScpiClient::availabilityCheckFinished);

    QVERIFY(client.startMonitoring(20));
    QTRY_COMPARE(instrument.commands, QList<QByteArray>{"*IDN?"});
    QCOMPARE(client.sessionState(), State::Closed);
    const auto open = client.open();
    client.stopMonitoring();
    QTest::qWait(50);
    QCOMPARE(client.sessionState(), State::Connecting);
    QCOMPARE(instrument.peers.size(), 1);
    instrument.write(0, "instrument\n");

    QTRY_COMPARE(instrument.disconnections, 1);
    QTRY_COMPARE(instrument.peers.size(), 2);
    QTRY_COMPARE(done.size(), 1);
    expectSuccess(done, 0, open);
    QCOMPARE(client.sessionState(), State::Connected);
    QCOMPARE(client.availability(), Availability::Available);
    QCOMPARE(checks.size(), 0);
}

void ClientTest::offlineCheckTimeout()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, instrument.port()));
    QVERIFY(client.setTimeouts({1000, 1000, 250}));
    QSignalSpy checks(&client, &ScpiClient::availabilityCheckFinished);
    QSignalSpy changes(&client, &ScpiClient::availabilityChanged);

    const auto slow = client.checkAvailability();
    QTRY_COMPARE(instrument.commands, QList<QByteArray>{"*IDN?"});
    instrument.write(0, "partial");
    QTRY_COMPARE(checks.size(), 1);
    expectCheck(checks, 0, slow, Error::AvailabilityTimeout);
    QTRY_COMPARE(instrument.disconnections, 1);
    QCOMPARE(client.availability(), Availability::Unavailable);
    QCOMPARE(client.sessionState(), State::Closed);
    QCOMPARE(changes.size(), 1);
}

void ClientTest::invalidRequestsAndCloseCancellation()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    ScpiClient client;
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);
    QSignalSpy checks(&client, &ScpiClient::availabilityCheckFinished);

    const auto noEndpoint = client.open();
    const auto noCheckEndpoint = client.checkAvailability();
    const auto closed = client.send("VOLT 5");
    const auto invalid = client.query("FIRST?\nSECOND?");
    const auto format = client.query("TRACE?", static_cast<Format>(-1));
    QCOMPARE(failed.size(), 0);
    QTRY_COMPARE(failed.size(), 4);
    expectFailure(failed, 0, noEndpoint, Error::InvalidEndpoint);
    expectFailure(failed, 1, closed, Error::NotConnected);
    expectFailure(failed, 2, invalid, Error::InvalidCommand);
    expectFailure(failed, 3, format, Error::UnsupportedFormat);
    QTRY_COMPARE(checks.size(), 1);
    expectCheck(checks, 0, noCheckEndpoint, Error::InvalidEndpoint);

    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, instrument.port()));
    const auto open = client.open();
    const auto anotherOpen = client.open();
    const auto active = client.query("ACTIVE?");
    const auto queued = client.query("QUEUED?");
    const auto check = client.checkAvailability();
    QTRY_COMPARE(instrument.commands, QList<QByteArray>{"ACTIVE?"});
    const auto close = client.close();

    QTRY_COMPARE(done.size(), 3);
    expectSuccess(done, 0, open);
    expectSuccess(done, 1, anotherOpen);
    expectSuccess(done, 2, close);
    QTRY_COMPARE(failed.size(), 6);
    expectFailure(failed, 4, active, Error::Cancelled);
    expectFailure(failed, 5, queued, Error::Cancelled);
    QTRY_COMPARE(checks.size(), 2);
    expectCheck(checks, 1, check, Error::Cancelled);
    QTRY_COMPARE(instrument.disconnections, 1);
    QCOMPARE(instrument.commands.size(), 1);
    QCOMPARE(instrument.peers.size(), 1);
    QCOMPARE(client.availability(), Availability::Unknown);
}

void ClientTest::connectionRefused()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    const auto port = instrument.port();
    instrument.server.close();

    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, port));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);
    QSignalSpy states(&client, &ScpiClient::sessionStateChanged);

    const auto open = client.open();
    const auto queued = client.query("*IDN?");
    QTRY_COMPARE(failed.size(), 2);
    expectFailure(failed, 0, open, Error::ConnectionFailed);
    expectFailure(failed, 1, queued, Error::SessionLost);
    QCOMPARE(done.size(), 0);
    QCOMPARE(client.sessionState(), State::Closed);
    QTRY_COMPARE(states.size(), 2);
    QCOMPARE(states.at(0).at(0).value<State>(), State::Connecting);
    QCOMPARE(states.at(1).at(0).value<State>(), State::Closed);
}

QTEST_GUILESS_MAIN(ClientTest)
#include "client_test.moc"
