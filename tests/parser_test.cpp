#include "responseparser.h"

#include <QtTest>

#include <algorithm>

using scpi::ScpiClient;
using scpi::detail::ParseStatus;
using scpi::detail::ResponseParser;

using Format = ScpiClient::ResponseFormat;
using Error = ScpiClient::Error;

class ParserTest : public QObject
{
    Q_OBJECT

private slots:
    void fragmented_data();
    void fragmented();
    void invalidFrame_data();
    void invalidFrame();
};

void ParserTest::fragmented_data()
{
    QTest::addColumn<Format>("format");
    QTest::addColumn<QByteArray>("frame");
    QTest::addColumn<QByteArray>("payload");

    QTest::newRow("text-lf") << Format::Text << QByteArray("MODEL,123\n") << QByteArray("MODEL,123");
    QTest::newRow("text-crlf") << Format::Text << QByteArray("12.50\r\n") << QByteArray("12.50");
    QTest::newRow("empty-text") << Format::Text << QByteArray("\r\n") << QByteArray();

    const auto binary = QByteArray::fromHex("0a000d23ff");
    QTest::newRow("block-lf") << Format::IeeeBlockLf << ("#205" + binary + '\n') << binary;
    QTest::newRow("block-crlf") << Format::IeeeBlockCrlf << ("#15" + binary + "\r\n") << binary;
    QTest::newRow("block-no-terminator") << Format::IeeeBlockNoTerminator << ("#15" + binary) << binary;
    QTest::newRow("block-empty") << Format::IeeeBlockNoTerminator << QByteArray("#10") << QByteArray();
    QTest::newRow("block-nine-length-digits") << Format::IeeeBlockLf
        << QByteArray("#9000000002AB\n") << QByteArray("AB");
}

void ParserTest::fragmented()
{
    QFETCH(Format, format);
    QFETCH(QByteArray, frame);
    QFETCH(QByteArray, payload);

    const auto limit = std::max<qsizetype>(1, payload.size());
    for (qsizetype split = 0; split < frame.size(); ++split) {
        ResponseParser parser(format, limit);
        QCOMPARE(parser.append(frame.first(split)).status, ParseStatus::NeedMore);
        const auto result = parser.append(frame.sliced(split));
        QCOMPARE(result.status, ParseStatus::Complete);
        QCOMPARE(result.error, Error::None);
        QCOMPARE(result.payload, payload);
    }

    ResponseParser parser(format, limit);
    for (qsizetype i = 0; i < frame.size() - 1; ++i)
        QCOMPARE(parser.append(frame.sliced(i, 1)).status, ParseStatus::NeedMore);
    const auto result = parser.append(frame.last(1));
    QCOMPARE(result.status, ParseStatus::Complete);
    QCOMPARE(result.payload, payload);
}

void ParserTest::invalidFrame_data()
{
    QTest::addColumn<Format>("format");
    QTest::addColumn<QByteArray>("prefix");
    QTest::addColumn<QByteArray>("tail");
    QTest::addColumn<Error>("error");

    QTest::newRow("bad-marker") << Format::IeeeBlockLf << QByteArray() << QByteArray("x")
        << Error::MalformedResponse;
    QTest::newRow("indefinite-block") << Format::IeeeBlockLf << QByteArray("#") << QByteArray("0")
        << Error::UnsupportedFormat;
    QTest::newRow("bad-digit-count") << Format::IeeeBlockLf << QByteArray("#") << QByteArray("x")
        << Error::MalformedResponse;
    QTest::newRow("bad-length") << Format::IeeeBlockLf << QByteArray("#2") << QByteArray("x")
        << Error::MalformedResponse;
    QTest::newRow("bad-lf") << Format::IeeeBlockLf << QByteArray("#11A") << QByteArray("\r")
        << Error::MalformedResponse;
    QTest::newRow("bad-crlf") << Format::IeeeBlockCrlf << QByteArray("#11A\r") << QByteArray("x")
        << Error::MalformedResponse;
    QTest::newRow("binary-limit-from-header") << Format::IeeeBlockCrlf << QByteArray("#2") << QByteArray("04")
        << Error::ResponseTooLarge;
    QTest::newRow("text-limit-before-lf") << Format::Text << QByteArray("abc\r") << QByteArray("x")
        << Error::ResponseTooLarge;
    QTest::newRow("text-limit-with-lf") << Format::Text << QByteArray("abc") << QByteArray("d\n")
        << Error::ResponseTooLarge;
    QTest::newRow("extra-text-bytes") << Format::Text << QByteArray("abc") << QByteArray("\nX")
        << Error::UnexpectedData;
    QTest::newRow("extra-binary-bytes") << Format::IeeeBlockLf << QByteArray("#11") << QByteArray("A\nX")
        << Error::UnexpectedData;
    QTest::newRow("unexpected-terminator") << Format::IeeeBlockNoTerminator << QByteArray("#11") << QByteArray("A\n")
        << Error::UnexpectedData;
}

void ParserTest::invalidFrame()
{
    QFETCH(Format, format);
    QFETCH(QByteArray, prefix);
    QFETCH(QByteArray, tail);
    QFETCH(Error, error);

    ResponseParser parser(format, 3);
    QCOMPARE(parser.append(prefix).status, ParseStatus::NeedMore);
    const auto result = parser.append(tail);
    QCOMPARE(result.status, ParseStatus::Error);
    QCOMPARE(result.error, error);
    QVERIFY(!result.detail.isEmpty());
}

QTEST_GUILESS_MAIN(ParserTest)
#include "parser_test.moc"
