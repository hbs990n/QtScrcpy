#ifndef KITKATVIEWER_H
#define KITKATVIEWER_H

#include <QImage>
#include <QPoint>
#include <QProcess>
#include <QTcpSocket>
#include <QWidget>

class QLabel;
class QContextMenuEvent;
class QTimer;

// Built-in viewer for Android 4.x devices driven by the kitkat-compatible
// scrcpy-server fork (minicap-style protocol, JPEG frames):
//   - the server listens on TCP port 6612 on the device and expects TWO
//     connections through "adb forward": video first, then control;
//   - the video stream is a 24 byte little-endian banner followed by chunks
//     of [u32 LE length][payload]; length == 4 marks a rotation event,
//     anything else is one complete JPEG frame;
//   - control messages are big-endian scrcpy 1.x style packets.
class KitkatViewer : public QWidget
{
    Q_OBJECT
signals:
    void logMessage(const QString &message);

public:
    // scale/quality/fps map onto the kitkat server flags -P/-Q/-r; the JPEG
    // encoder has no bitrate concept, callers translate bitrate to quality
    explicit KitkatViewer(const QString &serial, const QString &serverJarPath,
                          const QString &adbPath, int scale = 480, int quality = 70,
                          int fps = 12, bool landscape = false, bool stayOnTop = false,
                          bool frameless = false, QWidget *parent = nullptr);
    ~KitkatViewer() override;

    void start();
    void setFpsVisible(bool visible);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    bool runAdb(const QStringList &args, int timeoutMs = 10000);
    void cleanup(bool removingForward = true);
    void beginSession();
    void attemptConnect();
    void onVideoConnected();
    void onCtrlConnected();
    void onVideoDisconnected();
    void onReadyRead();
    void processBuffer();
    void scheduleRetry(const QString &reason, bool fullRestart);
    void applyBanner();
    void logKitkat(const QString &message);
    void toggleFullScreen();
    void onFpsTick();
    QPoint toDevicePos(const QPoint &widgetPos) const;
    void sendTouch(int action, const QPoint &widgetPos, quint16 pressure);
    void sendScroll(const QPoint &widgetPos, int vScroll);
    void sendKeycode(int keyCode);
    void sendPowerMode(int mode);

    QString m_serial;
    QString m_serverJarPath;
    QString m_adbPath;
    quint16 m_forwardPort = 0;
    int m_scale = 480;
    int m_quality = 70;
    int m_fps = 12;
    bool m_landscape = false;
    bool m_stayOnTop = false;
    bool m_frameless = false;
    bool m_fullscreen = false;
    QLabel *m_fpsLabel = nullptr;
    QTimer *m_fpsTimer = nullptr;
    int m_lastFpsSample = 0;

    QTcpSocket *m_videoSocket = nullptr;
    QTcpSocket *m_ctrlSocket = nullptr;
    QProcess *m_serverProc = nullptr;
    QTimer *m_retryTimer = nullptr;
    QTimer *m_restartTimer = nullptr;
    int m_retriesLeft = 0;
    bool m_shuttingDown = false;
    bool m_touchActive = false;
    bool m_sessionEstablished = false;
    bool m_screenOff = false;
    int m_frameCount = 0;

    QByteArray m_buf;
    bool m_bannerDone = false;
    QImage m_image;
    QSize m_deviceSize;
    QRect m_drawRect;
};

#endif // KITKATVIEWER_H
