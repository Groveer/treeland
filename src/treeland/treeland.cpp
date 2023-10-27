// Copyright (C) 2023 Dingyuan Zhang <lxz@mkacg.com>.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "treeland.h"
#include "MessageHandler.h"
#include "Messages.h"
#include "SocketWriter.h"
#include "treelandhelper.h"
#include "SafeDataStream.h"
#include "qwdisplay.h"
#include "waylandsocketproxy.h"
#include "SignalHandler.h"
#include "UserModel.h"

#include "socketmanager.h"
#include "shortcutmanager.h"

#include <WServer>
#include <WSurface>
#include <WSeat>
#include <WCursor>
#include <qqmlextensionplugin.h>
#include <wsocket.h>

#include <qwbackend.h>
#include <qwdisplay.h>
#include <qwoutput.h>
#include <qwcompositor.h>

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QQmlContext>
#include <QQmlEngine>
#include <QDebug>
#include <QLoggingCategory>
#include <QTimer>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <functional>

Q_LOGGING_CATEGORY(debug, "treeland.kernel.debug", QtDebugMsg);

using namespace SDDM;

namespace TreeLand {
TreeLand::TreeLand(TreeLandAppContext context)
    : QObject()
    , m_context(context)
    , m_socketProxy(new WaylandSocketProxy(this))
{
    connect(this, &TreeLand::requestAddNewSocket, m_socketProxy, &WaylandSocketProxy::newSocket);
    connect(m_socketProxy, &WaylandSocketProxy::socketCreated, this, [=] (std::shared_ptr<WSocket> socket) {
        WServer *server = m_engine->rootObjects().first()->findChild<WServer*>();
        Q_ASSERT(server);
        Q_ASSERT(server->isRunning());
        server->addSocket(socket.get());
    });
    connect(m_socketProxy, &WaylandSocketProxy::socketDeleted, this, [=] (std::shared_ptr<WSocket> socket) {
        Q_UNUSED(socket);
    });

    if (!context.isTestMode) {
        qInstallMessageHandler(GreeterMessageHandler);

        new SignalHandler;
    }

    m_socket = new QLocalSocket(this);

    connect(m_socket, &QLocalSocket::connected, this, &TreeLand::connected);
    connect(m_socket, &QLocalSocket::disconnected, this, &TreeLand::disconnected);
    connect(m_socket, &QLocalSocket::readyRead, this, &TreeLand::readyRead);
    connect(m_socket, &QLocalSocket::errorOccurred, this, &TreeLand::error);

    setup();

    if (!context.isTestMode) {
        m_socket->connectToServer(context.socket);
    }
}

void TreeLand::setup()
{
    m_engine = new QQmlApplicationEngine(this);

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    m_engine->loadFromModule("TreeLand", "Main");
#else
    m_engine->addImportPath(":/qt/qml");
    m_engine->load(QUrl(u"qrc:/qt/qml/TreeLand/Main.qml"_qs));
#endif

    WServer *server = m_engine->rootObjects().first()->findChild<WServer*>();
    treeland_socket_manager_v1* socketManager = treeland_socket_manager_v1_create(server->handle()->handle(), this);

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    TreeLandHelper *helper = m_engine->singletonInstance<TreeLandHelper*>("TreeLand", "TreeLandHelper");
#else
    auto helperTypeId = qmlTypeId("TreeLand", 1, 0, "TreeLandHelper");
    TreeLandHelper *helper = m_engine->singletonInstance<TreeLandHelper*>(helperTypeId);
#endif
    Q_ASSERT(helper);

    treeland_shortcut_manager_v1* shortcutManager = treeland_shortcut_manager_v1_create(server->handle()->handle(), this, helper);

    // ShortcutManager *shortcutManager = new ShortcutManager(this);
    // connect(shortcutManager, &ShortcutManager::shortcutContextCreated, this, [=] (ShortcutContext *context) {
    //     connect(helper, &TreeLandHelper::keyEvent, context, [=](uint32_t key, uint32_t modify) {
    //         context->send_shortcut(key, modify);
    //     });
    // });
}

void TreeLand::connected() {
    // log connection
    qDebug() << "Connected to the daemon.";

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    TreeLandHelper *helper = m_engine->singletonInstance<TreeLandHelper*>("TreeLand", "TreeLandHelper");
#else
    auto helperTypeId = qmlTypeId("TreeLand", 1, 0, "TreeLandHelper");
    TreeLandHelper *helper = m_engine->singletonInstance<TreeLandHelper*>(helperTypeId);
#endif
    Q_ASSERT(helper);

    connect(helper, &Helper::backToNormal, this, [=] {
        SocketWriter(m_socket) << quint32(GreeterMessages::BackToNormal);
    });
    connect(helper, &Helper::reboot, this, [=] {
        SocketWriter(m_socket) << quint32(GreeterMessages::Reboot);
    });

    // send connected message
    SocketWriter(m_socket) << quint32(GreeterMessages::Connect);

    SocketWriter(m_socket) << quint32(GreeterMessages::StartHelper) << helper->socketFile();
}

void TreeLand::disconnected() {
    // log disconnection
    qDebug() << "Disconnected from the daemon.";

    Q_EMIT socketDisconnected();

    qDebug() << "Display Manager is closed socket connect, quiting treeland.";
    qApp->exit();
}

void TreeLand::error() {
    qCritical() << "Socket error: " << m_socket->errorString();
}

void TreeLand::readyRead() {
    // input stream
    QDataStream input(m_socket);

    while (input.device()->bytesAvailable()) {
        // read message
        quint32 message;
        input >> message;

        switch (DaemonMessages(message)) {
            case DaemonMessages::Capabilities: {
                // log message
                qDebug() << "Message received from daemon: Capabilities";
            }
            break;
            case DaemonMessages::WaylandSocketDeleted: {
                QString user;
                input >> user;

                m_socketProxy->deleteSocket(user);
            }
            break;
            case DaemonMessages::UserActivateMessage: {
                QString user;
                input >> user;

                m_socketProxy->activateUser(user);
            }
            break;
            case DaemonMessages::SwitchToGreeter: {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
                Helper *helper = m_engine->singletonInstance<Helper*>("TreeLand", "Helper");
#else
                auto helperTypeId = qmlTypeId("TreeLand", 1, 0, "Helper");
                Helper *helper = m_engine->singletonInstance<Helper*>(helperTypeId);
#endif
                Q_ASSERT(helper);
                helper->greeterVisibleChanged();
            }
            break;
            default:
            break;
        }
    }
}
}

int main (int argc, char *argv[]) {
    qInstallMessageHandler(SDDM::GreeterMessageHandler);

    WServer::initializeQPA();
    // QQuickStyle::setStyle("Material");

    QGuiApplication::setAttribute(Qt::AA_UseOpenGLES);
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QGuiApplication app(argc, argv);

    QCommandLineOption socket({"s", "socket"}, "set ddm socket", "socket");
    QCommandLineOption testMode({"t", "test-mode"}, "use test mode");

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption(socket);
    parser.addOption(testMode);

    parser.process(app);

    TreeLand::TreeLand treeland({
        parser.isSet(testMode),
        parser.value(testMode),
    });

    return app.exec();
}