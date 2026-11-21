#include "instrumentemulator.h"
#include "testsupport.h"

using namespace scpi::test;

class EmulatorTest : public QObject
{
    Q_OBJECT

private slots:
    void roundTrip();
};

void EmulatorTest::roundTrip()
{
    scpi::emulator::InstrumentEmulator emulator({8192, 1, "\r\n"});
    QVERIFY(emulator.listen(QHostAddress::LocalHost, 0));
    ScpiClient client;
    QVERIFY(client.setEndpoint(QHostAddress::LocalHost, emulator.port()));
    QVERIFY(client.setTimeouts({1000, 6000, 500}));
    QVERIFY(client.setRecoveryPolicy({true, 3, 50, 200}));
    QSignalSpy done(&client, &ScpiClient::operationFinished);
    QSignalSpy failed(&client, &ScpiClient::operationFailed);
    QSignalSpy states(&client, &ScpiClient::sessionStateChanged);

    const auto open = client.open();
    const auto set = client.send("SOUR:VOLT 12.5", true);
    const auto readback = client.query("SOUR:VOLT?");
    const auto lines = client.query("SIM:TEXT? 70000,3", Format::Text, 3);
    const auto trace = client.query("TRAC:DATA? 524288,3", Format::IeeeBlockCrlf, 3);
    const auto drop = client.query("SIM:DROP?");
    const auto skipped = client.send("SOUR:VOLT 99");

    QByteArray linePayload = QByteArray(70000, 'A') + '\n' + QByteArray(70000, 'B')
                             + '\n' + QByteArray(70000, 'C');
    QByteArray blockPayload(524288, '\0');
    for (qsizetype i = 0; i < blockPayload.size(); ++i)
        blockPayload[i] = static_cast<char>(i % 128);

    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 5, 10000);
    expectSuccess(done, 0, open);
    expectSuccess(done, 1, set);
    expectSuccess(done, 2, readback, "12.5");
    expectSuccess(done, 3, lines, linePayload);
    expectSuccess(done, 4, trace, blockPayload.repeated(3));
    QTRY_COMPARE(failed.size(), 2);
    expectFailure(failed, 0, drop, Error::Disconnected);
    expectFailure(failed, 1, skipped, Error::SessionLost);
    QTRY_COMPARE(states.size(), 4);
    QCOMPARE(client.sessionState(), State::Connected);

    const auto idn = client.query("*IDN?");
    const auto persisted = client.query("SOUR:VOLT?");
    const auto confirmed = client.sendConfirmed("SOUR:VOLT 13");
    QTRY_COMPARE(done.size(), 8);
    expectSuccess(done, 5, idn, "SCPI,Instrument emulator,0001,1.1");
    expectSuccess(done, 6, persisted, "12.5");
    expectSuccess(done, 7, confirmed);
    QCOMPARE(failed.size(), 2);
}

QTEST_GUILESS_MAIN(EmulatorTest)
#include "emulator_test.moc"
