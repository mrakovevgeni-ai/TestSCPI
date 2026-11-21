#include <scpi/scpiclient.h>

#include <QCoreApplication>
#include <QMetaEnum>
#include <QStringList>
#include <QTextStream>

#include <cstdio>

using scpi::ScpiClient;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() < 2 || args.size() > 3) {
        QTextStream(stderr) << "Usage: " << args.at(0) << " IP [PORT]\n";
        return 2;
    }

    QHostAddress address;
    bool portOk = true;
    const quint16 port = args.size() == 3 ? args.at(2).toUShort(&portOk) : ScpiClient::DefaultPort;
    ScpiClient client;
    if (!address.setAddress(args.at(1)) || !portOk || !client.setEndpoint(address, port)) {
        QTextStream(stderr) << "Invalid IP address or TCP port.\n";
        return 2;
    }

    ScpiClient::OperationId openId = 0;
    ScpiClient::OperationId queryId = 0;
    ScpiClient::OperationId closeId = 0;
    QObject::connect(&client, &ScpiClient::operationFinished, &app,
                     [&](quint64 id, const QByteArray &response) {
        if (id == openId) {
            queryId = client.query("*IDN?");
        } else if (id == queryId) {
            QTextStream(stdout) << QString::fromLatin1(response) << Qt::endl;
            closeId = client.close();
        } else if (id == closeId) {
            app.quit();
        }
    });
    QObject::connect(&client, &ScpiClient::operationFailed, &app,
                     [&](quint64 id, ScpiClient::Error error, const QString &detail) {
        const auto errorName = QMetaEnum::fromType<ScpiClient::Error>().valueToKey(static_cast<int>(error));
        QTextStream(stderr) << "Operation " << id << " (" << errorName << ") " << detail << Qt::endl;
        app.exit(1);
    });

    openId = client.open();
    return app.exec();
}
