#include "instrumentemulator.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

#include <cstdio>

using scpi::emulator::InstrumentEmulator;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("scpi_emulator"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("TCP emulator of a SCPI instrument."));
    parser.addHelpOption();
    const QCommandLineOption addressOption("address", "Listen address.", "ip", "127.0.0.1");
    const QCommandLineOption portOption("port", "Listen port. Use 0 to choose a free port.", "port", "5025");
    const QCommandLineOption chunkOption("chunk-bytes", "Maximum bytes per write.", "bytes", "4096");
    const QCommandLineOption delayOption("chunk-delay-ms", "Delay between writes.", "ms", "0");
    const QCommandLineOption terminatorOption("binary-terminator", "Binary block terminator.", "lf|crlf|none", "lf");
    parser.addOptions({addressOption, portOption, chunkOption, delayOption, terminatorOption});
    parser.process(app);

    QHostAddress address;
    bool portOk = false;
    bool chunkOk = false;
    bool delayOk = false;
    const auto port = parser.value(portOption).toUShort(&portOk);
    InstrumentEmulator::Options options;
    options.chunkBytes = parser.value(chunkOption).toInt(&chunkOk);
    options.chunkDelayMs = parser.value(delayOption).toInt(&delayOk);
    if (!address.setAddress(parser.value(addressOption)) || !portOk || !chunkOk
        || options.chunkBytes < 1 || !delayOk || options.chunkDelayMs < 0) {
        QTextStream(stderr) << "Invalid address, port, chunk size or delay.\n";
        return 2;
    }

    const auto terminator = parser.value(terminatorOption);
    if (terminator == "none")
        options.binaryTerminator.clear();
    else if (terminator == "crlf")
        options.binaryTerminator = "\r\n";
    else if (terminator != "lf") {
        QTextStream(stderr) << "Expected lf, crlf or none for --binary-terminator.\n";
        return 2;
    }

    InstrumentEmulator instrument(options);
    if (!instrument.listen(address, port)) {
        QTextStream(stderr) << instrument.errorString() << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "SCPI emulator listening on " << address.toString()
                        << " port " << instrument.port() << Qt::endl;
    return app.exec();
}
