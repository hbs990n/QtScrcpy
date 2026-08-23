#include "kitkatviewer.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTcpServer>
#include <QTimer>
#include <QtEndian>

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

static const qint64 POINTER_ID_MOUSE = -1;               // generic pointer id accepted by the fork
static const quint16 PRESSURE_PRESSED = 0xFFFF;
static const char *REMOTE_JAR = "/data/local/tmp/scrcpy-server-kitkat.jar";
static const char *SERVER_CLASS = "com.genymobile.scrcpy.Server";

KitkatViewer::KitkatViewer(const QString &serial, const QString &serverJarPath,
                           const QString &adbPath, QWidget *parent)
    : QWidget(parent)
    , m_serial(serial)
    , m_serverJarPath(serverJarPath)
    , m_adbPath(adbPath)
{
    setWindowTitle(QString("QtScrcpy-%1 [kitkat]").arg(m_serial));
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

    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    m_retryTimer->setInterval(1000);
    connect(m_retryTimer, &QTimer::timeout, this, &KitkatViewer::beginSession);
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

bool KitkatViewer::runAdb(const QStringList &args, int timeoutMs)
{
    QProcess adb;
    adb.start(m_adbPath, args);
    if (!adb.waitForStarted(3000)) {
        qWarning("kitkat viewer: failed to start adb: %s", m_adbPath.toUtf8().constData());
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
        qWarning("kitkat viewer: missing native libs in %s", libDir.toUtf8().constData());
    }

    const QStringList serverArgs = QStringList() << "-s" << m_serial << "shell"
            << QString("CLASSPATH=%1 app_process / %2 -r 12 -Q 70 -P 480")
                   .arg(REMOTE_JAR).arg(SERVER_CLASS);
    if (m_serverProc->state() != QProcess::NotRunning) {
        m_serverProc->kill();
        m_serverProc->waitForFinished(2000);
    }
    m_serverProc->start(m_adbPath, serverArgs);

    m_bannerDone = false;
    m_buf.clear();
    m_retriesLeft = 30;

    // connect the video socket first: the server pairs its accepts in order,
    // so the control socket must only be dialed after video is established
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
    m_bannerDone = false;
    m_buf.clear();
    scheduleRetry(tr("video stream closed"));
}

void KitkatViewer::scheduleRetry(const QString &reason)
{
    if (m_shuttingDown || m_retryTimer->isActive()) {
        return;
    }
    if (--m_retriesLeft <= 0) {
        qWarning("kitkat viewer: giving up: %s", reason.toUtf8().constData());
        return;
    }
    m_videoSocket->abort();
    m_ctrlSocket->abort();
    m_retryTimer->start();
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
            qWarning("kitkat viewer: bogus chunk length %d, resync aborted", len);
            m_buf.clear();
            return;
        }
        if (m_buf.size() < 4 + len) {
            return;
        }
        QImage image;
        if (!image.loadFromData((const uchar *)m_buf.constData() + 4, len, "JPEG")) {
            qWarning("kitkat viewer: bad JPEG frame (%d bytes)", len);
        } else {
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

static QPoint toDeviceCoords(const QRect &drawRect, const QSize &deviceSize, const QPoint &widgetPos)
{
    if (drawRect.isEmpty()) {
        return QPoint();
    }
    int x = (widgetPos.x() - drawRect.x()) * deviceSize.width() / drawRect.width();
    int y = (widgetPos.y() - drawRect.y()) * deviceSize.height() / drawRect.height();
    return QPoint(qBound(0, x, deviceSize.width() - 1), qBound(0, y, deviceSize.height() - 1));
}

void KitkatViewer::sendTouch(int action, const QPoint &widgetPos, quint16 pressure)
{
    if (m_ctrlSocket->state() != QAbstractSocket::ConnectedState || !m_bannerDone) {
        return;
    }
    const QPoint dev = toDeviceCoords(m_drawRect, m_deviceSize, widgetPos);

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
    const QPoint dev = toDeviceCoords(m_drawRect, m_deviceSize, widgetPos);

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

void KitkatViewer::mousePressEvent(QMouseEvent *event)
{
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
        sendScroll(event->pos(), steps > 0 ? 1 : -1);
    }
}

void KitkatViewer::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        sendKeycode(4); // KEYCODE_BACK
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
