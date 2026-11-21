#include "testsupport.h"

using namespace scpi::test;

class RecoveryTest : public QObject
{
    Q_OBJECT

private slots:
    void recoveryAfterTimeout();
    void retryInitialConnection();
    void recoveryLimit();
    void closeCancelsRecovery();
};

void RecoveryTest::recoveryAfterTimeout()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, instrument.port()));
    QVERIFY(client.setTimeouts({1000, 300, 500}));
    QVERIFY(client.setRecoveryPolicy({true, 3, 250, 500}));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);
    QSignalSpy states(&client, &ScpiClient::sessionStateChanged);

    const auto open = client.open();
    const auto slow = client.query("SLOW?", Format::Text, 2);
    const auto setter = client.send("VOLT 99", true);
    QTRY_COMPARE(instrument.commands, QList<QByteArray>{"SLOW?"});
    instrument.write(0, "first\n");
    QTRY_COMPARE(failed.size(), 2);
    expectFailure(failed, 0, slow, Error::ResponseTimeout);
    expectFailure(failed, 1, setter, Error::SessionLost);
    QCOMPARE(client.sessionState(), State::Reconnecting);
    expectSuccess(done, 0, open);
    QCOMPARE(done.size(), 1);

    // This request belongs to the restored connection.
    QVERIFY(client.setTimeouts({1000, 1500, 500}));
    const auto fresh = client.query("FRESH?");
    QTRY_COMPARE(instrument.commands, (QList<QByteArray>{"SLOW?", "FRESH?"}));
    QCOMPARE(instrument.commandConnections, (QList<qsizetype>{0, 1}));
    instrument.write(1, "fresh\n");
    QTRY_COMPARE(done.size(), 2);
    expectSuccess(done, 1, fresh, "fresh");
    QCOMPARE(failed.size(), 2);
    QCOMPARE(states.size(), 4);
    QCOMPARE(states.at(2).at(0).value<State>(), State::Reconnecting);
    QCOMPARE(states.at(3).at(0).value<State>(), State::Connected);
}

void RecoveryTest::retryInitialConnection()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    const auto port = instrument.port();
    instrument.server.close();
    ScpiClient client;
    QVERIFY(client.recoveryPolicy().enabled);
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, port));
    QVERIFY(client.setRecoveryPolicy({true, 4, 100, 400}));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);

    const auto open = client.open();
    const auto identify = client.query("*IDN?");
    QTRY_COMPARE(client.sessionState(), State::Reconnecting);
    QCOMPARE(done.size(), 0);
    QCOMPARE(failed.size(), 0);

    const auto joined = client.open();
    QVERIFY(instrument.listen(port));
    QTRY_COMPARE(instrument.commands, QList<QByteArray>{"*IDN?"});
    instrument.write(0, "ready\n");
    QTRY_COMPARE(done.size(), 3);
    expectSuccess(done, 0, open);
    expectSuccess(done, 1, joined);
    expectSuccess(done, 2, identify, "ready");
    QCOMPARE(failed.size(), 0);
}

void RecoveryTest::recoveryLimit()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    const auto port = instrument.port();
    instrument.server.close();

    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, port));
    QVERIFY(client.setRecoveryPolicy({true, 2, 25, 50}));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);
    QSignalSpy states(&client, &ScpiClient::sessionStateChanged);

    const auto open = client.open();
    const auto query = client.query("*IDN?");
    QTRY_COMPARE(failed.size(), 2);
    expectFailure(failed, 0, open, Error::ConnectionFailed);
    expectFailure(failed, 1, query, Error::SessionLost);
    QTRY_COMPARE(states.size(), 3);
    QCOMPARE(states.at(1).at(0).value<State>(), State::Reconnecting);
    QCOMPARE(client.sessionState(), State::Closed);
    QVERIFY(instrument.listen(port));
    QTest::qWait(120);
    QCOMPARE(instrument.peers.size(), 0);
    QCOMPARE(failed.size(), 2);
    QCOMPARE(done.size(), 0);
}

void RecoveryTest::closeCancelsRecovery()
{
    Instrument instrument;
    QVERIFY(instrument.listen());
    const auto port = instrument.port();
    instrument.server.close();

    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, port));
    QVERIFY(client.setRecoveryPolicy({true, 5, 150, 600}));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);

    const auto open = client.open();
    QTRY_COMPARE(client.sessionState(), State::Reconnecting);
    const auto request = client.query("*IDN?");
    const auto close = client.close();
    QVERIFY(instrument.listen(port));
    QTest::qWait(250);

    QCOMPARE(client.sessionState(), State::Closed);
    QCOMPARE(instrument.peers.size(), 0);
    QCOMPARE(failed.size(), 2);
    expectFailure(failed, 0, open, Error::Cancelled);
    expectFailure(failed, 1, request, Error::Cancelled);
    QCOMPARE(done.size(), 1);
    expectSuccess(done, 0, close);
}

QTEST_GUILESS_MAIN(RecoveryTest)
#include "recovery_test.moc"
