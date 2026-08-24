#include "kitkatviewer.h"

#include <QContextMenuEvent>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTcpServer>
#include <QTimer>
#include <QtEndian>
#include <QWindow>

// kitkat server protocol constants
static const quint16 KITKAT_SERVER_PORT = 6612;          // TCP port on the device
static const int BANNER_LENGTH = 24;                     // version+size+pid+sizes+orientation+quirks
static const qint32 ROTATION_CHUNK = 4;                  // chunk length marking a rotation event

// control message types (scrcpy 1.x dialect, big endian)
static const quint8 MSG_INJECT_KEYCODE = 0;
static const quint8 MSG_INJECT_TOUCH_EVENT = 2;
static const quint8 MSG_INJECT_SCROLL_EVENT = 3;

// android MotionEvent actions used for injection
static const int AMOTION_ACTION_DOWN = 0;
static const int AMOTION_ACTION_UP = 1;
static const int AMOTION_ACTION_MOVE = 2;

// SurfaceControl display power modes used by the fork
static const int POWER_MODE_OFF = 0;
static const int POWER_MODE_NORMAL = 2;

static const qint64 POINTER_ID_MOUSE = -1;               // generic pointer id accepted by the fork
static const quint16 PRESSURE_PRESSED = 0xFFFF;
static const char *REMOTE_JAR = "/data/local/tmp/scrcpy-server-kitkat.jar";
static const char *SERVER_CLASS = "com.genymobile.scrcpy.Server";

KitkatViewer::KitkatViewer(const QString &serial, const QString &serverJarPath,
                           const QString &adbPath, int scale, int quality, int fps,
                           bool landscape, bool stayOnTop, bool frameless, QWidget *parent)
    : QWidget(parent)
    , m_serial(serial)
    , m_serverJarPath(serverJarPath)
    , m_adbPath(adbPath)
    , m_scale(scale)
    , m_quality(quality)
    , m_fps(fps)
    , m_landscape(landscape)
    , m_stayOnTop(stayOnTop)
    , m_frameless(frameless)
{
    setWindowTitle(QString("QtScrcpy-%1 [kitkat]").arg(m_serial));
    if (m_stayOnTop) {
        setWindowFlag(Qt::WindowStaysOnTopHint, true);
    }
    if (m_frameless) {
        setWindowFlag(Qt::FramelessWindowHint, true);
    }
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    setAutoFillBackground(true);

    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);

    m_videoSocket = new QTcpSocket(this);
    connect(m_videoSocket, &QTcpSocket::connected, this, &KitkatViewer::onVideoConnected);
    connect(m_videoSocket, &QTcpSocket::readyRead, this, &KitkatViewer::onReadyRead);
    connect(m_videoSocket, &QTcpSocket::disconnected, this, &KitkatViewer::onVideoDisconnected);

    // the control socket carries no data back, its signals are not needed
    m_ctrlSocket = new QTcpSocket(this);

    m_serverProc = new QProcess(this);

    // light reconnect: server already bound, just dial again
    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    m_retryTimer->setInterval(1000);
    connect(m_retryTimer, &QTimer::timeout, this, &KitkatViewer::attemptConnect);

    // full restart: teardown + fresh server spawn (stream lost mid-session)
    m_restartTimer = new QTimer(this);
    m_restartTimer->setSingleShot(true);
    m_restartTimer->setInterval(1000);
    connect(m_restartTimer, &QTimer::timeout, this, &KitkatViewer::beginSession);

    m_fpsTimer = new QTimer(this);
    m_fpsTimer->setInterval(1000);
    connect(m_fpsTimer, &QTimer::timeout, this, &KitkatViewer::onFpsTick);
}

void KitkatViewer::setFpsVisible(bool visible)
{
    if (visible && !m_fpsLabel) {
        m_fpsLabel = new QLabel(this);
        m_fpsLabel->setStyleSheet("color: lime; background-color: rgba(0,0,0,120); padding: 2px;");
        m_fpsLabel->move(6, 6);
        m_fpsLabel->show();
        m_fpsTimer->start();
    } else if (!visible && m_fpsLabel) {
        m_fpsLabel->deleteLater();
        m_fpsLabel = nullptr;
        m_fpsTimer->stop();
    }
}

void KitkatViewer::onFpsTick()
{
    if (!m_fpsLabel) {
        return;
    }
    const int delta = m_frameCount - m_lastFpsSample;
    m_lastFpsSample = m_frameCount;
    m_fpsLabel->setText(QString("%1 fps").arg(delta));
}

KitkatViewer::~KitkatViewer()
{
    cleanup(false);
}

void KitkatViewer::start()
{
    m_shuttingDown = false;
    beginSession();
}

// report to the GUI log panel (and stderr) so failures are visible to users
void KitkatViewer::logKitkat(const QString &message)
{
    emit logMessage(QString("[kitkat] %1").arg(message));
    qWarning("kitkat viewer: %s", message.toUtf8().constData());
}

bool KitkatViewer::runAdb(const QStringList &args, int timeoutMs)
{
    QProcess adb;
    adb.start(m_adbPath, args);
    if (!adb.waitForStarted(3000)) {
        logKitkat(QString("failed to start adb: %1").arg(m_adbPath));
        return false;
    }
    if (!adb.waitForFinished(timeoutMs)) {
        adb.kill();
        adb.waitForFinished(2000);
        return false;
    }
    return adb.exitStatus() == QProcess::NormalExit && adb.exitCode() == 0;
}

void KitkatViewer::beginSession()
{
    if (m_shuttingDown) {
        return;
    }

    // pick an ephemeral local port for the forward
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost);
    m_forwardPort = probe.serverPort();
    probe.close();

    // kill leftovers first: the server binds the device side port exclusively,
    // a stale instance makes every new bind fail with EADDRINUSE
    runAdb(QStringList() << "-s" << m_serial << "shell"
                         << "pkill -f com.genymobile.scrcpy.Server", 5000);
    runAdb(QStringList() << "-s" << m_serial << "push" << m_serverJarPath << REMOTE_JAR);
    runAdb(QStringList() << "-s" << m_serial << "forward"
                         << QString("tcp:%1").arg(m_forwardPort)
                         << QString("tcp:%1").arg(KITKAT_SERVER_PORT));

    // native libraries are loaded from /data/local/tmp by the fork
    QString libDir = QString::fromLocal8Bit(qgetenv("QTSCRCPY_KITKAT_LIB_DIR"));
    if (libDir.isEmpty()) {
        libDir = QCoreApplication::applicationDirPath() + "/scrcpy-server-kitkat-libs";
        if (!QFileInfo::exists(libDir)) {
            libDir = QFileInfo(m_serverJarPath).dir().filePath("scrcpy-server-kitkat-libs");
        }
    }
    static const char *libs[] = { "libturbojpeg.so", "libjpeg.so", "libcompress.so" };
    bool libsOk = true;
    for (const char *lib : libs) {
        const QString path = libDir + "/" + lib;
        if (!QFileInfo::exists(path) || !runAdb(QStringList() << "-s" << m_serial << "push"
                                                              << path << QString("/data/local/tmp/%1").arg(lib))) {
            libsOk = false;
        }
    }
    if (!libsOk) {
        logKitkat(QString("missing native libs in %1").arg(libDir));
    }
    logKitkat(QString("session start, forward port %1").arg(m_forwardPort));

    const QString serverCmd = QString("CLASSPATH=%1 app_process / %2 -r %3 -Q %4 -P %5%6")
            .arg(REMOTE_JAR).arg(SERVER_CLASS).arg(m_fps).arg(m_quality).arg(m_scale)
            .arg(m_landscape ? " -L" : "");
    const QStringList serverArgs = QStringList() << "-s" << m_serial << "shell" << serverCmd;
    logKitkat(QString("server: %1").arg(serverCmd));
    if (m_serverProc->state() != QProcess::NotRunning) {
        m_serverProc->kill();
        m_serverProc->waitForFinished(2000);
    }
    m_serverProc->start(m_adbPath, serverArgs);

    m_bannerDone = false;
    m_sessionEstablished = false;
    m_buf.clear();
    m_retriesLeft = 30;

    // NOTE: setup runs ONCE per session. Retrying must NOT come back here:
    // pkill would murder the freshly spawned server before it ever binds.
    // The device needs a few seconds to boot the VM and bind port 6612,
    // meanwhile connections get accepted by adb and dropped - keep trying.
    attemptConnect();
}

void KitkatViewer::attemptConnect()
{
    if (m_shuttingDown || m_videoSocket->state() == QAbstractSocket::ConnectingState) {
        return;
    }
    // connect the video socket first: the server pairs its accepts in order,
    // so the control socket must only be dialed after video is established
    m_videoSocket->abort();
    m_ctrlSocket->abort();
    m_videoSocket->connectToHost(QHostAddress::LocalHost, m_forwardPort);
}

void KitkatViewer::onVideoConnected()
{
    // the server only starts streaming after BOTH connections are accepted:
    // video (this signal) is up, now open the control channel
    if (m_ctrlSocket->state() != QAbstractSocket::ConnectedState) {
        m_ctrlSocket->connectToHost(QHostAddress::LocalHost, m_forwardPort);
    }
}

void KitkatViewer::onReadyRead()
{
    m_buf.append(m_videoSocket->readAll());

    if (!m_bannerDone && m_buf.size() >= BANNER_LENGTH) {
        applyBanner();
    }
    processBuffer();
}

void KitkatViewer::applyBanner()
{
    // layout: u8 version, u8 size, i32 pid, i32 realW/H, i32 desiredW/H, u8 orientation, u8 quirks
    m_deviceSize.setWidth(qFromLittleEndian<qint32>((const uchar *)m_buf.constData() + 14));
    m_deviceSize.setHeight(qFromLittleEndian<qint32>((const uchar *)m_buf.constData() + 18));
    m_buf.remove(0, BANNER_LENGTH); // keep the buffer pointing at the first chunk
    m_bannerDone = true;
    m_frameCount = 0;
    m_sessionEstablished = true;
    logKitkat(QString("banner ok, device %1x%2").arg(m_deviceSize.width()).arg(m_deviceSize.height()));

    QSize winSize = m_deviceSize;
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        const QSize maxSize(avail.width() * 85 / 100, avail.height() * 85 / 100);
        if (winSize.width() > maxSize.width() || winSize.height() > maxSize.height()) {
            winSize.scale(maxSize, Qt::KeepAspectRatio);
        }
    }
    resize(winSize);
}

void KitkatViewer::onVideoDisconnected()
{
    if (m_shuttingDown) {
        return;
    }
    const bool hadStream = m_sessionEstablished;
    m_bannerDone = false;
    m_buf.clear();
    if (hadStream) {
        // the stream died mid-session: rebuild everything (the fork stops
        // accepting after the two channels are taken)
        scheduleRetry(tr("video stream lost, restarting"), true);
    } else {
        // server probably not bound yet: light reconnect, do NOT restart it
        scheduleRetry(tr("video stream closed"), false);
    }
}

void KitkatViewer::scheduleRetry(const QString &reason, bool fullRestart)
{
    if (m_shuttingDown || m_retryTimer->isActive() || m_restartTimer->isActive()) {
        return;
    }
    if (--m_retriesLeft <= 0) {
        logKitkat(QString("giving up: %1").arg(reason));
        return;
    }
    logKitkat(QString("retry (%1 left): %2").arg(m_retriesLeft).arg(reason));
    if (fullRestart) {
        m_restartTimer->start();
    } else {
        m_retryTimer->start();
    }
}

void KitkatViewer::processBuffer()
{
    forever {
        if (!m_bannerDone) {
            return;
        }
        if (m_buf.size() < 4) {
            return;
        }
        const qint32 len = qFromLittleEndian<qint32>((const uchar *)m_buf.constData());
        if (len == ROTATION_CHUNK) {
            if (m_buf.size() < 4 + 4) {
                return;
            }
            // rotation events are ignored: the fork sends frames already composed
            m_buf.remove(0, 4 + 4);
            continue;
        }
        if (len <= 0 || len > (64 * 1024 * 1024)) {
            logKitkat(QString("bogus chunk length %1").arg(len));
            m_buf.clear();
            return;
        }
        if (m_buf.size() < 4 + len) {
            return;
        }
        QImage image;
        if (!image.loadFromData((const uchar *)m_buf.constData() + 4, len, "JPEG")) {
            logKitkat(QString("JPEG decode failed (%1 bytes), Qt jpeg plugin missing?").arg(len));
        } else {
            if (m_frameCount == 0) {
                logKitkat(QString("first frame decoded %1x%2").arg(image.width()).arg(image.height()));
            }
            ++m_frameCount;
            m_image = image;
            update();
        }
        m_buf.remove(0, 4 + len);
    }
}

void KitkatViewer::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (m_image.isNull()) {
        return;
    }

    const QSize scaled = m_image.size().scaled(size(), Qt::KeepAspectRatio);
    m_drawRect = QRect(QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2), scaled);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(m_drawRect, m_image);
}

QPoint KitkatViewer::toDevicePos(const QPoint &widgetPos) const
{
    if (m_drawRect.isEmpty()) {
        return QPoint();
    }
    int x = (widgetPos.x() - m_drawRect.x()) * m_deviceSize.width() / m_drawRect.width();
    int y = (widgetPos.y() - m_drawRect.y()) * m_deviceSize.height() / m_drawRect.height();
    return QPoint(qBound(0, x, m_deviceSize.width() - 1), qBound(0, y, m_deviceSize.height() - 1));
}

void KitkatViewer::toggleFullScreen()
{
    if (m_fullscreen) {
        showNormal();
        m_fullscreen = false;
    } else {
        showFullScreen();
        m_fullscreen = true;
    }
}

void KitkatViewer::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    menu.addAction(tr("Back (Esc)"), this, [this]() { sendKeycode(4); });
    menu.addAction(tr("Home"), this, [this]() { sendKeycode(3); });
    menu.addAction(tr("Menu"), this, [this]() { sendKeycode(82); });
    menu.addSeparator();
    menu.addAction(m_screenOff ? tr("Wake Screen") : tr("Turn Screen Off (keep mirroring)"), this, [this]() {
        sendPowerMode(m_screenOff ? POWER_MODE_NORMAL : POWER_MODE_OFF);
        m_screenOff = !m_screenOff;
    });
    menu.addSeparator();
    menu.addAction(tr("Volume +"), this, [this]() { sendKeycode(24); });
    menu.addAction(tr("Volume -"), this, [this]() { sendKeycode(25); });
    menu.addAction(tr("Power"), this, [this]() { sendKeycode(26); });
    menu.addSeparator();
    menu.addAction(m_fullscreen ? tr("Exit Fullscreen") : tr("Fullscreen"),
                   this, &KitkatViewer::toggleFullScreen);
    QAction *topAct = menu.addAction(tr("Stay on Top"), this, [this]() {
        m_stayOnTop = !m_stayOnTop;
        setWindowFlag(Qt::WindowStaysOnTopHint, m_stayOnTop);
        show();
    });
    topAct->setCheckable(true);
    topAct->setChecked(m_stayOnTop);
    menu.exec(event->globalPos());
}

void KitkatViewer::sendTouch(int action, const QPoint &widgetPos, quint16 pressure)
{
    if (m_ctrlSocket->state() != QAbstractSocket::ConnectedState || !m_bannerDone) {
        return;
    }
    const QPoint dev = toDevicePos(widgetPos);

    QByteArray msg;
    msg.append(char(MSG_INJECT_TOUCH_EVENT));
    msg.append(char(action));
    qint64 pointerIdBE = qToBigEndian<qint64>(POINTER_ID_MOUSE);
    msg.append((const char *)&pointerIdBE, 8);
    qint32 xBE = qToBigEndian<qint32>(dev.x());
    msg.append((const char *)&xBE, 4);
    qint32 yBE = qToBigEndian<qint32>(dev.y());
    msg.append((const char *)&yBE, 4);
    quint16 wBE = qToBigEndian<quint16>(m_deviceSize.width());
    msg.append((const char *)&wBE, 2);
    quint16 hBE = qToBigEndian<quint16>(m_deviceSize.height());
    msg.append((const char *)&hBE, 2);
    quint16 pBE = qToBigEndian<quint16>(pressure);
    msg.append((const char *)&pBE, 2);
    qint32 buttonsBE = qToBigEndian<qint32>(0);
    msg.append((const char *)&buttonsBE, 4);

    m_ctrlSocket->write(msg);
}

void KitkatViewer::sendScroll(const QPoint &widgetPos, int vScroll)
{
    if (m_ctrlSocket->state() != QAbstractSocket::ConnectedState || !m_bannerDone) {
        return;
    }
    const QPoint dev = toDevicePos(widgetPos);

    QByteArray msg;
    msg.append(char(MSG_INJECT_SCROLL_EVENT));
    qint32 xBE = qToBigEndian<qint32>(dev.x());
    msg.append((const char *)&xBE, 4);
    qint32 yBE = qToBigEndian<qint32>(dev.y());
    msg.append((const char *)&yBE, 4);
    quint16 wBE = qToBigEndian<quint16>(m_deviceSize.width());
    msg.append((const char *)&wBE, 2);
    quint16 hBE = qToBigEndian<quint16>(m_deviceSize.height());
    msg.append((const char *)&hBE, 2);
    qint32 hBEs = qToBigEndian<qint32>(0);
    msg.append((const char *)&hBEs, 4);
    qint32 vBE = qToBigEndian<qint32>(vScroll);
    msg.append((const char *)&vBE, 4);

    m_ctrlSocket->write(msg);
}

void KitkatViewer::sendKeycode(int keyCode)
{
    if (m_ctrlSocket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    QByteArray down;
    down.append(char(MSG_INJECT_KEYCODE));
    down.append(char(AMOTION_ACTION_DOWN)); // KeyEvent ACTION_DOWN shares value 0
    qint32 keyDownBE = qToBigEndian<qint32>(keyCode);
    down.append((const char *)&keyDownBE, 4);
    qint32 zeroDown = qToBigEndian<qint32>(0); // metaState
    down.append((const char *)&zeroDown, 4);
    m_ctrlSocket->write(down);

    QByteArray up;
    up.append(char(MSG_INJECT_KEYCODE));
    up.append(char(AMOTION_ACTION_UP)); // KeyEvent ACTION_UP shares value 1
    qint32 keyUpBE = qToBigEndian<qint32>(keyCode);
    up.append((const char *)&keyUpBE, 4);
    qint32 zeroUp = qToBigEndian<qint32>(0);
    up.append((const char *)&zeroUp, 4);
    m_ctrlSocket->write(up);
}

void KitkatViewer::sendPowerMode(int mode)
{
    if (m_ctrlSocket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    QByteArray msg;
    msg.append(char(9)); // MSG_SET_SCREEN_POWER_MODE
    msg.append(char(mode));
    m_ctrlSocket->write(msg);
}

void KitkatViewer::mousePressEvent(QMouseEvent *event)
{
    // with no title bar the top strip acts as the drag handle
    if (m_frameless && event->button() == Qt::LeftButton && event->pos().y() <= 28) {
        if (windowHandle()) {
            windowHandle()->startSystemMove();
        }
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_touchActive = true;
        sendTouch(AMOTION_ACTION_DOWN, event->pos(), PRESSURE_PRESSED);
    }
}

void KitkatViewer::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_touchActive) {
        m_touchActive = false;
        sendTouch(AMOTION_ACTION_UP, event->pos(), 0);
    }
}

void KitkatViewer::mouseMoveEvent(QMouseEvent *event)
{
    if (m_touchActive) {
        sendTouch(AMOTION_ACTION_MOVE, event->pos(), PRESSURE_PRESSED);
    }
}

void KitkatViewer::wheelEvent(QWheelEvent *event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps != 0) {
        sendScroll(event->position().toPoint(), steps > 0 ? 1 : -1);
    }
}

void KitkatViewer::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        if (m_fullscreen) {
            toggleFullScreen();
        } else {
            sendKeycode(4); // KEYCODE_BACK
        }
        return;
    case Qt::Key_F11:
        toggleFullScreen();
        return;
    case Qt::Key_Home:
        sendKeycode(3); // KEYCODE_HOME
        return;
    case Qt::Key_F5:
        sendKeycode(26); // KEYCODE_POWER (toggle screen)
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

void KitkatViewer::cleanup(bool removingForward)
{
    m_shuttingDown = true;
    if (m_retryTimer) {
        m_retryTimer->stop();
    }
    if (m_restartTimer) {
        m_restartTimer->stop();
    }
    if (m_videoSocket) {
        m_videoSocket->abort();
    }
    if (m_ctrlSocket) {
        m_ctrlSocket->abort();
    }
    if (m_serverProc && m_serverProc->state() != QProcess::NotRunning) {
        m_serverProc->kill();
        m_serverProc->waitForFinished(3000);
    }
    if (removingForward && m_forwardPort != 0) {
        runAdb(QStringList() << "-s" << m_serial << "forward"
                             << "--remove" << QString("tcp:%1").arg(m_forwardPort), 5000);
        m_forwardPort = 0;
    }
    runAdb(QStringList() << "-s" << m_serial << "shell"
                         << "pkill -f com.genymobile.scrcpy.Server", 5000);
}

void KitkatViewer::closeEvent(QCloseEvent *event)
{
    cleanup();
    QWidget::closeEvent(event);
}
