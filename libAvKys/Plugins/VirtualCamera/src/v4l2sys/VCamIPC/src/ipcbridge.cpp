/* Webcamoid, webcam capture application.
 * Copyright (C) 2018  Gonzalo Exequiel Pedone
 *
 * Webcamoid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Webcamoid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Webcamoid. If not, see <http://www.gnu.org/licenses/>.
 *
 * Web-Site: http://webcamoid.github.io/
 */

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileSystemWatcher>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <errno.h>
#include <fcntl.h>
#include <limits>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ipcbridge.h"
#include "VCamUtils/src/image/videoformat.h"
#include "VCamUtils/src/image/videoframe.h"
#include "VCamUtils/src/logger/logger.h"

#define MAX_CAMERAS 64

#define AKVCAM_CID_BASE         (V4L2_CID_USER_BASE | 0xe000)
#define AKVCAM_CID_SCALING      (AKVCAM_CID_BASE + 0)
#define AKVCAM_CID_ASPECT_RATIO (AKVCAM_CID_BASE + 1)
#define AKVCAM_CID_SWAP_RGB     (AKVCAM_CID_BASE + 2)

namespace AkVCam
{
    typedef QList<VideoFormat> FormatsList;
    typedef QMap<uint32_t, PixelFormat> PixFmtFourccMap;
    typedef std::function<bool (const std::string &deviceId)> CanHandleFunc;
    typedef std::function<std::string
                          (const std::wstring &description,
                           const std::vector<VideoFormat> &formats)> DeviceCreateFunc;
    typedef std::function<void
                          (const std::string &deviceId)> DeviceDestroyFunc;
    typedef std::function<bool (const std::string &deviceId,
                                const std::wstring &description)> ChangeDescriptionFunc;
    typedef std::function<QString (void)> DestroyAllDevicesFunc;

    enum IoMethod
    {
        IoMethodUnknown = -1,
        IoMethodReadWrite,
        IoMethodMemoryMap,
        IoMethodUserPointer
    };

    enum DeviceType
    {
        DeviceTypeCapture,
        DeviceTypeOutput
    };

    struct CaptureBuffer
    {
        char *start;
        size_t length;
    };

    struct DriverFunctions
    {
        QString driver;
        CanHandleFunc canHandle;
        DeviceCreateFunc deviceCreate;
        DeviceDestroyFunc deviceDestroy;
        ChangeDescriptionFunc changeDescription;
        DestroyAllDevicesFunc destroyAllDevices;
    };

    struct DeviceInfo
    {
        int nr;
        QString path;
        QString description;
        QString driver;
        QString bus;
        FormatsList formats;
    };

    struct DeviceConfig
    {
        bool horizontalMirror;
        bool verticalMirror;
        Scaling scaling;
        AspectRatio aspectRatio;
        bool swapRgb;
    };

    class IpcBridgePrivate
    {
        public:
            IpcBridge *self;
            QStringList m_devices;
            QMap<QString, QString> m_descriptions;
            QMap<QString, FormatsList> m_devicesFormats;
            std::string m_rootMethod;
            std::vector<std::string> m_broadcasting;
            std::map<std::string, std::string> m_options;
            std::vector<std::wstring> m_driverPaths;
            FormatsList m_defaultFormats;
            QMap<QString, DeviceConfig> m_deviceConfigs;
            QFileSystemWatcher *m_fsWatcher;
            QVector<CaptureBuffer> m_buffers;
            QString m_driverPath;
            VideoFormat m_curFormat;
            IoMethod m_ioMethod;
            int m_fd;
            int m_nBuffers;

            IpcBridgePrivate(IpcBridge *self);
            ~IpcBridgePrivate();

            static inline QMap<Scaling, QString> *scalingToString();
            static inline QMap<AspectRatio, QString> *aspectRatioToString();
            const QVector<AkVCam::DriverFunctions> *driverFunctions();
            const DriverFunctions *functionsForDriver(const QString &driver);
            QStringList supportedDrivers();
            inline int xioctl(int fd, ulong request, void *arg) const;
            bool sudo(const QString &command,
                      const QStringList &argumments) const;
            bool sudo(const std::string &command,
                      const QStringList &argumments) const;
            QString sysfsControls(const QString &deviceId) const;
            QString sysfsControls(const std::string &deviceId) const;
            bool isSplitDevice(const QString &deviceId) const;
            bool isSplitDevice(const std::string &deviceId) const;
            QStringList connectedDevices(const QString &deviceId) const;
            QStringList connectedDevices(const std::string &deviceId) const;
            inline PixFmtFourccMap *v4l2PixFmtFourccMap() const;
            FormatsList formatFps(int fd,
                                  const struct v4l2_fmtdesc &format,
                                  __u32 width,
                                  __u32 height) const;
            FormatsList formats(int fd) const;
            QList<QStringList> combineMatrix(const QList<QStringList> &matrix) const;
            void combineMatrixP(const QList<QStringList> &matrix,
                                              size_t index,
                                              QStringList &combined,
                                              QList<QStringList> &combinations) const;
            QList<FormatsList> readFormats(QSettings &settings) const;
            QList<DeviceInfo> readDevicesConfigs() const;
            FormatsList formatsFromSettings(const QString &deviceId,
                                            const QList<DeviceInfo> &devicesInfo) const;
            void setFps(int fd, const v4l2_fract &fps);
            bool initReadWrite(quint32 bufferSize);
            bool initMemoryMap();
            bool initUserPointer(quint32 bufferSize);
            void initDefaultFormats();
            bool startOutput();
            void stopOutput();
            void updateDevices();
            void onDirectoryChanged();
            void onFileChanged();
            QStringList listDrivers();
            QString preferredDriver() const;
            QString locateDriverPath() const;
            QString compileDriver(const QString &path);
            QString currentDriver(const std::string &deviceId={});
            QString cleanDescription(const QString &description) const;
            bool isModuleLoaded(const QString &driver) const;
            bool canHandleAkVCam(const std::string &deviceId);
            QList<DeviceInfo> devicesInfo() const;
            std::string deviceCreateAkVCam(const std::wstring &description,
                                           const std::vector<VideoFormat> &formats);
            void deviceDestroyAkVCam(const std::string &deviceId);
            bool changeDescriptionAkVCam(const std::string &deviceId,
                                         const std::wstring &description);
            QString destroyAllDevicesAkVCam();
            bool canHandleV4L2Loopback(const std::string &deviceId);
            std::string deviceCreateV4L2Loopback(const std::wstring &description,
                                                 const std::vector<VideoFormat> &formats);
            void deviceDestroyV4L2Loopback(const std::string &deviceId);
            bool changeDescriptionV4L2Loopback(const std::string &deviceId,
                                               const std::wstring &description);
            QString destroyAllDevicesV4L2Loopback();

    };
}

AkVCam::IpcBridge::IpcBridge()
{
    this->d = new IpcBridgePrivate(this);
    auto methods = this->availableRootMethods();
    this->d->m_rootMethod = methods.front();
    this->d->initDefaultFormats();
    this->d->updateDevices();
}

AkVCam::IpcBridge::~IpcBridge()
{
    delete this->d;
}

void AkVCam::IpcBridge::setOption(const std::string &key, const std::string &value)
{
    if (value.empty())
        this->d->m_options.erase(key);
    else
        this->d->m_options[key] = value;
}

std::vector<std::wstring> AkVCam::IpcBridge::driverPaths() const
{
    return this->d->m_driverPaths;
}

void AkVCam::IpcBridge::setDriverPaths(const std::vector<std::wstring> &driverPaths)
{
    this->d->m_driverPaths = driverPaths;
}

// List of possible graphical sudo methods that can be supported:
//
// https://en.wikipedia.org/wiki/Comparison_of_privilege_authorization_features#Introduction_to_implementations
std::vector<std::string> AkVCam::IpcBridge::availableRootMethods() const
{
    auto paths = QProcessEnvironment::systemEnvironment().value("PATH").split(':');

    static const QStringList sus {
        "pkexec",
        "kdesu",
        "kdesudo",
        "gksu",
        "gksudo",
        "gtksu",
        "ktsuss",
        "beesu"
    };

    std::vector<std::string> methods;

    for (auto &su: sus)
        for (auto &path: paths)
            if (QDir(path).exists(su)) {
                methods.push_back(su.toStdString());

                break;
            }

    return methods;
}

std::string AkVCam::IpcBridge::rootMethod() const
{
    return this->d->m_rootMethod;
}

bool AkVCam::IpcBridge::setRootMethod(const std::string &rootMethod)
{
    auto methods = this->availableRootMethods();
    auto it = std::find(methods.begin(), methods.end(), rootMethod);

    if (it == methods.end())
        return false;

    this->d->m_rootMethod = rootMethod;

    return false;
}

void AkVCam::IpcBridge::connectService(bool asClient)
{
    Q_UNUSED(asClient)
}

void AkVCam::IpcBridge::disconnectService()
{
}

bool AkVCam::IpcBridge::registerPeer(bool asClient)
{
    Q_UNUSED(asClient)

    return true;
}

void AkVCam::IpcBridge::unregisterPeer()
{
}

std::vector<std::string> AkVCam::IpcBridge::listDevices() const
{
    std::vector<std::string> devices;

    for (auto &device: this->d->m_devices)
        devices.push_back(device.toStdString());

    return devices;
}

std::wstring AkVCam::IpcBridge::description(const std::string &deviceId) const
{
    return this->d->m_descriptions.value(QString::fromStdString(deviceId)).toStdWString();
}

std::vector<AkVCam::PixelFormat> AkVCam::IpcBridge::supportedOutputPixelFormats() const
{
    return {
        PixelFormatRGB24,
        PixelFormatRGB16,
        PixelFormatRGB15,
        PixelFormatBGR32,
        PixelFormatBGR24,
        PixelFormatUYVY,
        PixelFormatYUY2
    };
}

std::vector<AkVCam::VideoFormat> AkVCam::IpcBridge::formats(const std::string &deviceId) const
{
    auto device = QString::fromStdString(deviceId);

    if (!this->d->m_devicesFormats.contains(device))
        return {};

    std::vector<VideoFormat> formats;

    for (auto &format: this->d->m_devicesFormats[device])
        formats.push_back(format);

    return formats;
}

std::string AkVCam::IpcBridge::broadcaster(const std::string &deviceId) const
{
    auto sysfsControls = this->d->sysfsControls(deviceId);

    if (sysfsControls.isEmpty())
        return {};

    sysfsControls += "/broadcasters";

    if (!QFileInfo::exists(sysfsControls))
        return {};

    QFile broadcasters(sysfsControls);

    if (broadcasters.open(QIODevice::ReadOnly | QIODevice::Text))
        for (auto &device: broadcasters.readAll().split('\n')) {
            auto dev = device.trimmed();

            if (!dev.isEmpty())
                return dev.toStdString();
        }

    return {};
}

bool AkVCam::IpcBridge::isHorizontalMirrored(const std::string &deviceId)
{
    auto outputs = this->d->connectedDevices(deviceId);

    for (auto &output: outputs) {
        /* Check if the device has V4L2 controls defined and we can get the
         * property.
         */
        int fd = open(output.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

        if (fd >= 0) {
            v4l2_control control;
            memset(&control, 0, sizeof(v4l2_control));
            control.id = V4L2_CID_HFLIP;

            if (this->d->xioctl(fd, VIDIOC_G_CTRL, &control) >= 0) {
                close(fd);

                return control.value;
            }

            close(fd);
        }

        // Else, check if the device has the attribute.
        auto sysfsControls = this->d->sysfsControls(output);

        if (!sysfsControls.isEmpty()) {
            sysfsControls += "/hflip";

            if (QFileInfo::exists(sysfsControls)) {
                QFile hflip(sysfsControls);

                if (hflip.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    auto values = hflip.readAll().split('\n');

                    if (!values.isEmpty())
                        return values.first().trimmed() != "0";
                }
            }
        }

        /* All previous checks failed, check if we have stored it as a device
         * config.
         */
        if (this->d->m_deviceConfigs.contains(output))
            return this->d->m_deviceConfigs[output].horizontalMirror;
    }

    return false;
}

bool AkVCam::IpcBridge::isVerticalMirrored(const std::string &deviceId)
{
    auto outputs = this->d->connectedDevices(deviceId);

    for (auto &output: outputs) {
        /* Check if the device has V4L2 controls defined and we can get the
         * property.
         */
        int fd = open(output.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

        if (fd >= 0) {
            v4l2_control control;
            memset(&control, 0, sizeof(v4l2_control));
            control.id = V4L2_CID_VFLIP;

            if (this->d->xioctl(fd, VIDIOC_G_CTRL, &control) >= 0) {
                close(fd);

                return control.value;
            }

            close(fd);
        }

        // Else, check if the device has the attribute.
        auto sysfsControls = this->d->sysfsControls(output);

        if (!sysfsControls.isEmpty()) {
            sysfsControls += "/vflip";

            if (QFileInfo::exists(sysfsControls)) {
                QFile vflip(sysfsControls);

                if (vflip.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    auto values = vflip.readAll().split('\n');

                    if (!values.isEmpty())
                        return values.first().trimmed() != "0";
                }
            }
        }

        /* All previous checks failed, check if we have stored it as a device
         * config.
         */
        if (this->d->m_deviceConfigs.contains(output))
            return this->d->m_deviceConfigs[output].verticalMirror;
    }

    return false;
}

AkVCam::Scaling AkVCam::IpcBridge::scalingMode(const std::string &deviceId)
{
    auto outputs = this->d->connectedDevices(deviceId);

    for (auto &output: outputs) {
        /* Check if the device has V4L2 controls defined and we can get the
         * property.
         */
        int fd = open(output.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

        if (fd >= 0) {
            v4l2_control control;
            memset(&control, 0, sizeof(v4l2_control));
            control.id = AKVCAM_CID_SCALING;

            if (this->d->xioctl(fd, VIDIOC_G_CTRL, &control) >= 0) {
                close(fd);

                return Scaling(control.value);
            }

            close(fd);
        }

        // Else, check if the device has the attribute.
        auto sysfsControls = this->d->sysfsControls(output);

        if (!sysfsControls.isEmpty()) {
            sysfsControls += "/scaling";

            if (QFileInfo::exists(sysfsControls)) {
                QFile scalingMode(sysfsControls);

                if (scalingMode.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    auto values = scalingMode.readAll().split('\n');

                    if (!values.isEmpty()) {
                        auto scaling = values.first().trimmed();
                        auto scalingToString = this->d->scalingToString();

                        if (scalingToString->values().contains(scaling))
                            return scalingToString->key(scaling);
                    }
                }
            }
        }

        /* All previous checks failed, check if we have stored it as a device
         * config.
         */
        if (this->d->m_deviceConfigs.contains(output))
            return this->d->m_deviceConfigs[output].scaling;
    }

    return ScalingFast;
}

AkVCam::AspectRatio AkVCam::IpcBridge::aspectRatioMode(const std::string &deviceId)
{
    auto outputs = this->d->connectedDevices(deviceId);

    for (auto &output: outputs) {
        /* Check if the device has V4L2 controls defined and we can get the
         * property.
         */
        int fd = open(output.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

        if (fd >= 0) {
            v4l2_control control;
            memset(&control, 0, sizeof(v4l2_control));
            control.id = AKVCAM_CID_ASPECT_RATIO;

            if (this->d->xioctl(fd, VIDIOC_G_CTRL, &control) >= 0) {
                close(fd);

                return AspectRatio(control.value);
            }

            close(fd);
        }

        // Else, check if the device has the attribute.
        auto sysfsControls = this->d->sysfsControls(output);

        if (!sysfsControls.isEmpty()) {
            sysfsControls += "/aspect_ratio";

            if (QFileInfo::exists(sysfsControls)) {
                QFile aspectRatioMode(sysfsControls);

                if (aspectRatioMode.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    auto values = aspectRatioMode.readAll().split('\n');

                    if (!values.isEmpty()) {
                        auto aspectRatio = values.first().trimmed();
                        auto aspectRatioToString = this->d->aspectRatioToString();

                        if (aspectRatioToString->values().contains(aspectRatio))
                            return aspectRatioToString->key(aspectRatio);
                    }
                }
            }
        }

        /* All previous checks failed, check if we have stored it as a device
         * config.
         */
        if (this->d->m_deviceConfigs.contains(output))
            return this->d->m_deviceConfigs[output].aspectRatio;
    }

    return AspectRatioIgnore;
}

bool AkVCam::IpcBridge::swapRgb(const std::string &deviceId)
{
    auto outputs = this->d->connectedDevices(deviceId);

    for (auto &output: outputs) {
        /* Check if the device has V4L2 controls defined and we can get the
         * property.
         */
        int fd = open(output.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

        if (fd >= 0) {
            v4l2_control control;
            memset(&control, 0, sizeof(v4l2_control));
            control.id = AKVCAM_CID_SWAP_RGB;

            if (this->d->xioctl(fd, VIDIOC_G_CTRL, &control) >= 0) {
                close(fd);

                return control.value;
            }

            close(fd);
        }

        // Else, check if the device has the attribute.
        auto sysfsControls = this->d->sysfsControls(output);

        if (!sysfsControls.isEmpty()) {
            sysfsControls += "/swap_rgb";

            if (QFileInfo::exists(sysfsControls)) {
                QFile swapRgb(sysfsControls);

                if (swapRgb.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    auto values = swapRgb.readAll().split('\n');

                    if (!values.isEmpty())
                        return values.first().trimmed() != "0";
                }
            }
        }

        /* All previous checks failed, check if we have stored it as a device
         * config.
         */
        if (this->d->m_deviceConfigs.contains(output))
            return this->d->m_deviceConfigs[output].swapRgb;
    }

    return false;
}

std::vector<std::string> AkVCam::IpcBridge::listeners(const std::string &deviceId)
{
    auto outputs = this->d->connectedDevices(deviceId);
    QSet<QString> listenersSet;

    for (auto &output: outputs) {
        auto sysfsControls = this->d->sysfsControls(output);

        if (sysfsControls.isEmpty())
            continue;

        sysfsControls += "/listeners";

        if (!QFileInfo::exists(sysfsControls))
            continue;

        QFile listeners(sysfsControls);

        if (listeners.open(QIODevice::ReadOnly | QIODevice::Text))
            for (auto &device: listeners.readAll().split('\n')) {
                auto dev = device.trimmed();

                if (!dev.isEmpty())
                    listenersSet << dev;
            }
    }

    std::vector<std::string> listeners;

    for (auto &listener: listenersSet)
        listeners.push_back(listener.toStdString());

    return listeners;
}

std::string AkVCam::IpcBridge::deviceCreate(const std::wstring &description,
                                            const std::vector<VideoFormat> &formats)
{
    auto driver = this->d->currentDriver();

    if (driver.isEmpty())
        return {};

    auto functions = this->d->functionsForDriver(driver);

    if (!functions)
        return {};

    return functions->deviceCreate(description, formats);
}

void AkVCam::IpcBridge::deviceDestroy(const std::string &deviceId)
{
    auto driver = this->d->currentDriver(deviceId);

    if (driver.isEmpty())
        return;

    auto functions = this->d->functionsForDriver(driver);

    if (!functions)
        return;

    return functions->deviceDestroy(deviceId);
}

bool AkVCam::IpcBridge::changeDescription(const std::string &deviceId,
                                          const std::wstring &description)
{
    auto driver = this->d->currentDriver(deviceId);

    if (driver.isEmpty())
        return false;

    auto functions = this->d->functionsForDriver(driver);

    if (!functions)
        return false;

    return functions->changeDescription(deviceId, description);
}

bool AkVCam::IpcBridge::destroyAllDevices()
{
    QTemporaryDir tempDir;
    QFile cmds(tempDir.path() + "/akvcam_exec.sh");

    if (cmds.open(QIODevice::WriteOnly | QIODevice::Text)) {
        cmds.setPermissions(QFileDevice::ReadOwner
                            | QFileDevice::WriteOwner
                            | QFileDevice::ExeOwner
                            | QFileDevice::ReadUser
                            | QFileDevice::WriteUser
                            | QFileDevice::ExeUser);

        for (auto &function: *this->d->driverFunctions())
            cmds.write(function.destroyAllDevices().toUtf8() + "\n");

        cmds.close();

        return this->d->sudo(this->d->m_rootMethod, {"sh", cmds.fileName()});
    }

    return false;
}

bool AkVCam::IpcBridge::deviceStart(const std::string &deviceId,
                                    const VideoFormat &format)
{
    std::string device;
    auto isSplit = this->d->isSplitDevice(deviceId);

    if (isSplit) {
        auto outputs = this->d->connectedDevices(deviceId);

        if (outputs.isEmpty())
            return false;

        device = outputs.first().toStdString();
    } else {
        device = deviceId;
    }

    // Frames read must be blocking so we does not waste CPU time.
    this->d->m_fd = open(device.c_str(),
                         O_RDWR | O_NONBLOCK,
                         0);

    if (this->d->m_fd < 0)
        return false;

    v4l2_capability capabilities;
    memset(&capabilities, 0, sizeof(v4l2_capability));

    if (this->d->xioctl(this->d->m_fd, VIDIOC_QUERYCAP, &capabilities) < 0) {
        qDebug() << "VirtualCamera:  Can't query capabilities.";
        close(this->d->m_fd);
        this->d->m_fd = -1;

        return false;
    }

    auto fmtToFourcc = this->d->v4l2PixFmtFourccMap();
    struct v4l2_format fmt;
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    this->d->xioctl(this->d->m_fd, VIDIOC_G_FMT, &fmt);

    if (isSplit) {
        fmt.fmt.pix.pixelformat = fmtToFourcc->key(PixelFormat(format.fourcc()));
        fmt.fmt.pix.width = __u32(format.width());
        fmt.fmt.pix.height = __u32(format.height());
    } else {
        auto outputFormats = this->formats(deviceId);

        if (outputFormats.empty()) {
            qDebug() << "VirtualCamera: Can't find a similar format:"
                     << VideoFormat::stringFromFourcc(format.fourcc()).c_str();
            close(this->d->m_fd);
            this->d->m_fd = -1;

            return false;
        }

        auto nearestFormat = format.nearest(outputFormats);
        fmt.fmt.pix.pixelformat = fmtToFourcc->key(PixelFormat(nearestFormat.fourcc()));
        fmt.fmt.pix.width = __u32(nearestFormat.width());
        fmt.fmt.pix.height = __u32(nearestFormat.height());
    }

    if (this->d->xioctl(this->d->m_fd, VIDIOC_S_FMT, &fmt) < 0) {
        qDebug() << "VirtualCamera:  Can't set format:"
                 << VideoFormat::stringFromFourcc(format.fourcc()).c_str();
        close(this->d->m_fd);
        this->d->m_fd = -1;

        return false;
    }

    v4l2_fract fps = {__u32(format.minimumFrameRate().num()),
                      __u32(format.minimumFrameRate().den())};
    this->d->setFps(this->d->m_fd, fps);
    this->d->m_curFormat =
            VideoFormat(fmtToFourcc->value(fmt.fmt.pix.pixelformat),
                        int(fmt.fmt.pix.width),
                        int(fmt.fmt.pix.height));

    if (this->d->m_ioMethod == IoMethodReadWrite
        && capabilities.capabilities & V4L2_CAP_READWRITE
        && this->d->initReadWrite(fmt.fmt.pix.sizeimage)) {
    } else if (this->d->m_ioMethod == IoMethodMemoryMap
             && capabilities.capabilities & V4L2_CAP_STREAMING
             && this->d->initMemoryMap()) {
    } else if (this->d->m_ioMethod == IoMethodUserPointer
             && capabilities.capabilities & V4L2_CAP_STREAMING
             && this->d->initUserPointer(fmt.fmt.pix.sizeimage)) {
    } else
        this->d->m_ioMethod = IoMethodUnknown;

    if (this->d->m_ioMethod != IoMethodUnknown)
        return this->d->startOutput();

    if (capabilities.capabilities & V4L2_CAP_STREAMING) {
        if (this->d->initMemoryMap())
            this->d->m_ioMethod = IoMethodMemoryMap;
        else if (this->d->initUserPointer(fmt.fmt.pix.sizeimage))
            this->d->m_ioMethod = IoMethodUserPointer;
    }

    if (this->d->m_ioMethod == IoMethodUnknown) {
        if (capabilities.capabilities & V4L2_CAP_READWRITE
            && this->d->initReadWrite(fmt.fmt.pix.sizeimage))
            this->d->m_ioMethod = IoMethodReadWrite;
        else
            return false;
    }

    return this->d->startOutput();
}

void AkVCam::IpcBridge::deviceStop(const std::string &deviceId)
{
    Q_UNUSED(deviceId)
    this->d->stopOutput();

    if (!this->d->m_buffers.isEmpty()) {
        if (this->d->m_ioMethod == IoMethodReadWrite)
            delete [] this->d->m_buffers[0].start;
        else if (this->d->m_ioMethod == IoMethodMemoryMap)
            for (qint32 i = 0; i < this->d->m_buffers.size(); i++)
                munmap(this->d->m_buffers[i].start,
                       this->d->m_buffers[i].length);
        else if (this->d->m_ioMethod == IoMethodUserPointer)
            for (qint32 i = 0; i < this->d->m_buffers.size(); i++)
                delete [] this->d->m_buffers[i].start;
    }

    close(this->d->m_fd);
    this->d->m_fd = -1;
    this->d->m_curFormat.clear();
    this->d->m_buffers.clear();
}

bool AkVCam::IpcBridge::write(const std::string &deviceId,
                              const VideoFrame &frame)
{
    Q_UNUSED(deviceId)

    if (this->d->m_buffers.isEmpty())
        return false;

    if (this->d->m_fd < 0)
        return false;

    auto scaled = frame.scaled(this->d->m_curFormat.width(),
                               this->d->m_curFormat.height())
                        .convert(this->d->m_curFormat.fourcc());

    if (!scaled.format().isValid())
        return false;

    if (this->d->m_ioMethod == IoMethodReadWrite) {
        memcpy(this->d->m_buffers[0].start,
               scaled.data().get(),
               qMin(this->d->m_buffers[0].length,
                    scaled.dataSize()));

        return ::write(this->d->m_fd,
                       this->d->m_buffers[0].start,
                       this->d->m_buffers[0].length) >= 0;
    } else if (this->d->m_ioMethod == IoMethodMemoryMap
               || this->d->m_ioMethod == IoMethodUserPointer) {
        v4l2_buffer buffer;
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buffer.memory = (this->d->m_ioMethod == IoMethodMemoryMap)?
                            V4L2_MEMORY_MMAP:
                            V4L2_MEMORY_USERPTR;

        if (this->d->xioctl(this->d->m_fd, VIDIOC_DQBUF, &buffer) < 0)
            return false;

        if (buffer.index >= quint32(this->d->m_buffers.size()))
            return false;

        memcpy(this->d->m_buffers[int(buffer.index)].start,
               scaled.data().get(),
               qMin(size_t(buffer.bytesused),
                    scaled.dataSize()));

        return this->d->xioctl(this->d->m_fd, VIDIOC_QBUF, &buffer) >= 0;
    }

    return false;
}

void AkVCam::IpcBridge::setMirroring(const std::string &deviceId,
                                     bool horizontalMirrored,
                                     bool verticalMirrored)
{
    auto outputs = this->d->connectedDevices(deviceId);

    for (auto &output: outputs) {
        /* Check if the device has V4L2 controls defined and we can set the
         * property.
         */
        int fd = open(output.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

        if (fd >= 0) {
            v4l2_control hflipControl;
            memset(&hflipControl, 0, sizeof(v4l2_control));
            hflipControl.id = V4L2_CID_HFLIP;
            hflipControl.value = horizontalMirrored;

            v4l2_control vflipControl;
            memset(&vflipControl, 0, sizeof(v4l2_control));
            vflipControl.id = V4L2_CID_VFLIP;
            vflipControl.value = verticalMirrored;

            if (this->d->xioctl(fd, VIDIOC_S_CTRL, &hflipControl) >= 0
                && this->d->xioctl(fd, VIDIOC_S_CTRL, &vflipControl) >= 0) {
                close(fd);

                return;
            }

            close(fd);
        }

        // Else, check if the device has the attribute.
        auto sysfsControls = this->d->sysfsControls(output);

        if (!sysfsControls.isEmpty()) {
            auto hflipControls = sysfsControls + "/hflip";
            auto vflipControls = sysfsControls + "/vflip";

            if (QFileInfo::exists(hflipControls)
                && QFileInfo::exists(vflipControls)) {
                QTemporaryDir tempDir;
                QFile cmds(tempDir.path() + "/akvcam_exec.sh");

                if (cmds.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    cmds.setPermissions(QFileDevice::ReadOwner
                                        | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner
                                        | QFileDevice::ReadUser
                                        | QFileDevice::WriteUser
                                        | QFileDevice::ExeUser);
                    cmds.write(QString("echo %1 > %2\n")
                               .arg(horizontalMirrored)
                               .arg(hflipControls)
                               .toUtf8());
                    cmds.write(QString("echo %1 > %2\n")
                               .arg(verticalMirrored)
                               .arg(vflipControls)
                               .toUtf8());
                    cmds.close();

                    this->d->sudo(this->d->m_rootMethod,
                                  {"sh", cmds.fileName()});

                    return;
                }
            }
        }

        // All previous checks failed, stored the value in the device config.
        if (!this->d->m_deviceConfigs.contains(output))
            this->d->m_deviceConfigs[output] = {};

        this->d->m_deviceConfigs[output].horizontalMirror = horizontalMirrored;
        this->d->m_deviceConfigs[output].verticalMirror = verticalMirrored;
    }
}

void AkVCam::IpcBridge::setScaling(const std::string &deviceId,
                                   Scaling scaling)
{
    auto outputs = this->d->connectedDevices(deviceId);

    for (auto &output: outputs) {
        /* Check if the device has V4L2 controls defined and we can set the
         * property.
         */
        int fd = open(output.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

        if (fd >= 0) {
            v4l2_control control;
            memset(&control, 0, sizeof(v4l2_control));
            control.id = AKVCAM_CID_SCALING;
            control.value = scaling;

            if (this->d->xioctl(fd, VIDIOC_S_CTRL, &control) >= 0) {
                close(fd);

                return;
            }

            close(fd);
        }

        // Else, check if the device has the attribute.
        auto sysfsControls = this->d->sysfsControls(output);

        if (!sysfsControls.isEmpty()) {
            sysfsControls += "/scaling";

            if (QFileInfo::exists(sysfsControls)) {
                QTemporaryDir tempDir;
                QFile cmds(tempDir.path() + "/akvcam_exec.sh");

                if (cmds.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    cmds.setPermissions(QFileDevice::ReadOwner
                                        | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner
                                        | QFileDevice::ReadUser
                                        | QFileDevice::WriteUser
                                        | QFileDevice::ExeUser);
                    auto scalingToString = this->d->scalingToString();

                    if (scalingToString->contains(scaling)) {
                        cmds.write(QString("echo %1 > %2\n")
                                   .arg(scalingToString->value(scaling))
                                   .arg(sysfsControls)
                                   .toUtf8());
                        cmds.close();

                        this->d->sudo(this->d->m_rootMethod,
                                      {"sh", cmds.fileName()});

                        return;
                    }
                }
            }
        }

        // All previous checks failed, stored the value in the device config.
        if (!this->d->m_deviceConfigs.contains(output))
            this->d->m_deviceConfigs[output] = {};

        this->d->m_deviceConfigs[output].scaling = scaling;
    }
}

void AkVCam::IpcBridge::setAspectRatio(const std::string &deviceId,
                                       AspectRatio aspectRatio)
{
    auto outputs = this->d->connectedDevices(deviceId);

    for (auto &output: outputs) {
        /* Check if the device has V4L2 controls defined and we can set the
         * property.
         */
        int fd = open(output.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

        if (fd >= 0) {
            v4l2_control control;
            memset(&control, 0, sizeof(v4l2_control));
            control.id = AKVCAM_CID_ASPECT_RATIO;
            control.value = aspectRatio;

            if (this->d->xioctl(fd, VIDIOC_S_CTRL, &control) >= 0) {
                close(fd);

                return;
            }

            close(fd);
        }

        // Else, check if the device has the attribute.
        auto sysfsControls = this->d->sysfsControls(output);

        if (!sysfsControls.isEmpty()) {
            sysfsControls += "/aspect_ratio";

            if (QFileInfo::exists(sysfsControls)) {
                QTemporaryDir tempDir;
                QFile cmds(tempDir.path() + "/akvcam_exec.sh");

                if (cmds.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    cmds.setPermissions(QFileDevice::ReadOwner
                                        | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner
                                        | QFileDevice::ReadUser
                                        | QFileDevice::WriteUser
                                        | QFileDevice::ExeUser);
                    auto aspectRatioToString = this->d->aspectRatioToString();

                    if (aspectRatioToString->contains(aspectRatio)) {
                        cmds.write(QString("echo %1 > %2\n")
                                   .arg(aspectRatioToString->value(aspectRatio))
                                   .arg(sysfsControls)
                                   .toUtf8());
                        cmds.close();

                        this->d->sudo(this->d->m_rootMethod,
                                      {"sh", cmds.fileName()});

                        return;
                    }
                }
            }
        }

        // All previous checks failed, stored the value in the device config.
        if (!this->d->m_deviceConfigs.contains(output))
            this->d->m_deviceConfigs[output] = {};

        this->d->m_deviceConfigs[output].aspectRatio = aspectRatio;
    }
}

void AkVCam::IpcBridge::setSwapRgb(const std::string &deviceId, bool swap)
{
    auto outputs = this->d->connectedDevices(deviceId);

    for (auto &output: outputs) {
        /* Check if the device has V4L2 controls defined and we can set the
         * property.
         */
        int fd = open(output.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

        if (fd >= 0) {
            v4l2_control control;
            memset(&control, 0, sizeof(v4l2_control));
            control.id = AKVCAM_CID_SWAP_RGB;
            control.value = swap;

            if (this->d->xioctl(fd, VIDIOC_S_CTRL, &control) >= 0) {
                close(fd);

                return;
            }

            close(fd);
        }

        // Else, check if the device has the attribute.
        auto sysfsControls = this->d->sysfsControls(output);

        if (!sysfsControls.isEmpty()) {
            sysfsControls += "/swap_rgb";

            if (QFileInfo::exists(sysfsControls)) {
                QTemporaryDir tempDir;
                QFile cmds(tempDir.path() + "/akvcam_exec.sh");

                if (cmds.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    cmds.setPermissions(QFileDevice::ReadOwner
                                        | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner
                                        | QFileDevice::ReadUser
                                        | QFileDevice::WriteUser
                                        | QFileDevice::ExeUser);
                    cmds.write(QString("echo %1 > %2\n")
                               .arg(swap)
                               .arg(sysfsControls)
                               .toUtf8());
                    cmds.close();

                    this->d->sudo(this->d->m_rootMethod,
                                  {"sh", cmds.fileName()});

                    return;
                }
            }
        }

        // All previous checks failed, stored the value in the device config.
        if (!this->d->m_deviceConfigs.contains(output))
            this->d->m_deviceConfigs[output] = {};

        this->d->m_deviceConfigs[output].swapRgb = swap;
    }
}

bool AkVCam::IpcBridge::addListener(const std::string &deviceId)
{
    Q_UNUSED(deviceId)

    return true;
}

bool AkVCam::IpcBridge::removeListener(const std::string &deviceId)
{
    Q_UNUSED(deviceId)

    return true;
}

AkVCam::IpcBridgePrivate::IpcBridgePrivate(IpcBridge *self):
    self(self)
{
    this->m_fsWatcher = new QFileSystemWatcher({"/dev"});
    this->m_ioMethod = IoMethodUnknown;
    this->m_fd = -1;
    this->m_nBuffers = 32;

    QObject::connect(this->m_fsWatcher,
                     &QFileSystemWatcher::directoryChanged,
                     [this] () {
        this->onDirectoryChanged();
    });
    QObject::connect(this->m_fsWatcher,
                     &QFileSystemWatcher::fileChanged,
                     [this] () {
        this->onFileChanged();
    });
}

AkVCam::IpcBridgePrivate::~IpcBridgePrivate()
{
    delete this->m_fsWatcher;
}

QMap<AkVCam::Scaling, QString> *AkVCam::IpcBridgePrivate::scalingToString()
{
    static QMap<Scaling, QString> scalingMap = {
        {ScalingFast  ,   "Fast"},
        {ScalingLinear, "Linear"},
    };

    return &scalingMap;
}

QMap<AkVCam::AspectRatio, QString> *AkVCam::IpcBridgePrivate::aspectRatioToString()
{
    static QMap<AspectRatio, QString> aspectRatioMap = {
        {AspectRatioIgnore   ,    "Ignore"},
        {AspectRatioKeep     ,      "Keep"},
        {AspectRatioExpanding, "Expanding"},
    };

    return &aspectRatioMap;
}

const QVector<AkVCam::DriverFunctions> *AkVCam::IpcBridgePrivate::driverFunctions()
{
    using namespace std::placeholders;

    static QVector<DriverFunctions> driverFunctions = {
        {"akvcam"      , std::bind(&IpcBridgePrivate::canHandleAkVCam, this, _1)
                       , std::bind(&IpcBridgePrivate::deviceCreateAkVCam, this, _1, _2)
                       , std::bind(&IpcBridgePrivate::deviceDestroyAkVCam, this, _1)
                       , std::bind(&IpcBridgePrivate::changeDescriptionAkVCam, this, _1, _2)
                       , std::bind(&IpcBridgePrivate::destroyAllDevicesAkVCam, this)},
        {"v4l2loopback", std::bind(&IpcBridgePrivate::canHandleV4L2Loopback, this, _1)
                       , std::bind(&IpcBridgePrivate::deviceCreateV4L2Loopback, this, _1, _2)
                       , std::bind(&IpcBridgePrivate::deviceDestroyV4L2Loopback, this, _1)
                       , std::bind(&IpcBridgePrivate::changeDescriptionV4L2Loopback, this, _1, _2)
                       , std::bind(&IpcBridgePrivate::destroyAllDevicesV4L2Loopback, this)},
    };

    return &driverFunctions;
}

const AkVCam::DriverFunctions *AkVCam::IpcBridgePrivate::functionsForDriver(const QString &driver)
{
    auto functions = driverFunctions();

    for (auto &functions: *functions)
        if (functions.driver == driver)
            return &functions;

    return nullptr;
}

QStringList AkVCam::IpcBridgePrivate::supportedDrivers()
{
    QStringList drivers;
    auto functions = driverFunctions();

    for (auto &functions: *functions)
        drivers << functions.driver;

    return drivers;
}

int AkVCam::IpcBridgePrivate::xioctl(int fd, ulong request, void *arg) const
{
    int r = -1;

    for (;;) {
        r = ioctl(fd, request, arg);

        if (r != -1 || errno != EINTR)
            break;
    }

    return r;
}

bool AkVCam::IpcBridgePrivate::sudo(const QString &command,
                                    const QStringList &argumments) const
{
    QProcess su;

    su.start(QString::fromStdString(this->m_rootMethod),
             QStringList {command} << argumments);
    su.waitForFinished(-1);

    if (su.exitCode()) {
        QByteArray outMsg = su.readAllStandardOutput();

        if (!outMsg.isEmpty())
            qDebug() << outMsg.toStdString().c_str();

        QByteArray errorMsg = su.readAllStandardError();

        if (!errorMsg.isEmpty())
            qDebug() << errorMsg.toStdString().c_str();

        return false;
    }

    return true;
}

bool AkVCam::IpcBridgePrivate::sudo(const std::string &command,
                                    const QStringList &argumments) const
{
    return this->sudo(QString::fromStdString(command), argumments);
}

QString AkVCam::IpcBridgePrivate::sysfsControls(const QString &deviceId) const
{
    auto sysfsPath = deviceId;
    sysfsPath = sysfsPath.replace("/dev/video",
                                  "/sys/devices/virtual/video4linux/video");
    sysfsPath += "/controls";

    return QFileInfo::exists(sysfsPath + "/connected_devices")?
                sysfsPath: QString();
}

QString AkVCam::IpcBridgePrivate::sysfsControls(const std::string &deviceId) const
{
    return this->sysfsControls(QString::fromStdString(deviceId));
}

bool AkVCam::IpcBridgePrivate::isSplitDevice(const QString &deviceId) const
{
    auto sysfsControls = this->sysfsControls(deviceId);

    if (sysfsControls.isEmpty())
        return false;

    sysfsControls += "/connected_devices";

    return QFileInfo::exists(sysfsControls);
}

bool AkVCam::IpcBridgePrivate::isSplitDevice(const std::string &deviceId) const
{
    return this->isSplitDevice(QString::fromStdString(deviceId));
}

QStringList AkVCam::IpcBridgePrivate::connectedDevices(const QString &deviceId) const
{
    auto sysfsControls = this->sysfsControls(deviceId);

    if (sysfsControls.isEmpty())
        return {};

    sysfsControls += "/connected_devices";

    if (!QFileInfo::exists(sysfsControls))
        return {};

    QFile connectedDevices(sysfsControls);
    QStringList devices;

    if (connectedDevices.open(QIODevice::ReadOnly | QIODevice::Text))
        for (auto &device: connectedDevices.readAll().split('\n')) {
            auto dev = device.trimmed();

            if (!dev.isEmpty())
                devices << dev;
        }

    return devices;
}

QStringList AkVCam::IpcBridgePrivate::connectedDevices(const std::string &deviceId) const
{
    return this->connectedDevices(QString::fromStdString(deviceId));
}

inline AkVCam::PixFmtFourccMap *AkVCam::IpcBridgePrivate::v4l2PixFmtFourccMap() const
{
    static PixFmtFourccMap fmtToFourcc = {
        // RGB formats
        {V4L2_PIX_FMT_RGB32 , PixelFormatRGB32},
        {V4L2_PIX_FMT_RGB24 , PixelFormatRGB24},
        {V4L2_PIX_FMT_RGB565, PixelFormatRGB16},
        {V4L2_PIX_FMT_RGB555, PixelFormatRGB15},

        // BGR formats
        {V4L2_PIX_FMT_BGR32 , PixelFormatBGR32},
        {V4L2_PIX_FMT_BGR24 , PixelFormatBGR24},

        // Luminance+Chrominance formats
        {V4L2_PIX_FMT_UYVY  , PixelFormatUYVY },
        {V4L2_PIX_FMT_YUYV  , PixelFormatYUY2 },
    };

    return &fmtToFourcc;
}

AkVCam::FormatsList AkVCam::IpcBridgePrivate::formatFps(int fd,
                                                        const v4l2_fmtdesc &format,
                                                        __u32 width,
                                                        __u32 height) const
{
    FormatsList formats;

#ifdef VIDIOC_ENUM_FRAMEINTERVALS
    struct v4l2_frmivalenum frmival;
    memset(&frmival, 0, sizeof(frmival));
    frmival.pixel_format = format.pixelformat;
    frmival.width = width;
    frmival.height = height;
    auto fmtToFourcc = v4l2PixFmtFourccMap();

    for (frmival.index = 0;
         this->xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) >= 0;
         frmival.index++) {
        if (!frmival.discrete.numerator
            || !frmival.discrete.denominator)
            continue;

        Fraction fps;

        if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE)
            fps = {frmival.discrete.denominator,
                   frmival.discrete.numerator};
        else
            fps = {frmival.stepwise.min.denominator,
                   frmival.stepwise.max.numerator};

        formats << VideoFormat(fmtToFourcc->value(format.pixelformat),
                               int(width),
                               int(height),
                               {fps});
    }
#else
    struct v4l2_streamparm params;
    memset(&params, 0, sizeof(v4l2_streamparm));
    params.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (this->xioctl(fd, VIDIOC_G_PARM, &params) >= 0) {
        auto timeperframe = &params.parm.capture.timeperframe;
        double fps = double(timeperframe->denominator)
                     / timeperframe->numerator;
        formats << VideoFormat(fmtToFourcc->value(format.pixelformat),
                               int(width),
                               int(height),
                               {fps});
    }
#endif

    return formats;
}

AkVCam::FormatsList AkVCam::IpcBridgePrivate::formats(int fd) const
{
    FormatsList formats;

#ifndef VIDIOC_ENUM_FRAMESIZES
    v4l2_format fmt;
    memset(&fmt, 0, sizeof(v4l2_format));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    uint width = 0;
    uint height = 0;

    // Check if it has at least a default format.
    if (this->xioctl(fd, VIDIOC_G_FMT, &fmt) >= 0) {
        width = fmt.fmt.pix.width;
        height = fmt.fmt.pix.height;
    }

    if (width <= 0 || height <= 0)
        return {};
#endif

    // Enumerate all supported formats.
    v4l2_fmtdesc fmtdesc;
    memset(&fmtdesc, 0, sizeof(v4l2_fmtdesc));
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (fmtdesc.index = 0;
         this->xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) >= 0;
         fmtdesc.index++) {
#ifdef VIDIOC_ENUM_FRAMESIZES
        v4l2_frmsizeenum frmsize;
        memset(&frmsize, 0, sizeof(v4l2_frmsizeenum));
        frmsize.pixel_format = fmtdesc.pixelformat;

        // Enumerate frame sizes.
        for (frmsize.index = 0;
             this->xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) >= 0;
             frmsize.index++) {
            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                formats << this->formatFps(fd,
                                           fmtdesc,
                                           frmsize.discrete.width,
                                           frmsize.discrete.height);
            }
        }
#else
        formats << this->formatFps(fd,
                                   fmtdesc,
                                   width,
                                   height);
#endif
    }

    return formats;
}

QList<QStringList> AkVCam::IpcBridgePrivate::combineMatrix(const QList<QStringList> &matrix) const
{
    QStringList combined;
    QList<QStringList> combinations;
    this->combineMatrixP(matrix, 0, combined, combinations);

    return combinations;
}

/* A matrix is a list of lists where each element in the main list is a row,
 * and each element in a row is a column. We combine each element in a row with
 * each element in the next rows.
 */
void AkVCam::IpcBridgePrivate::combineMatrixP(const QList<QStringList> &matrix,
                                              size_t index,
                                              QStringList &combined,
                                              QList<QStringList> &combinations) const
{
    if (index >= size_t(matrix.size())) {
        combinations << combined;

        return;
    }

    for (auto &column: matrix[int(index)]) {
        auto combined_p1 = combined + QStringList {column};
        this->combineMatrixP(matrix, index + 1, combined_p1, combinations);
    }
}

QList<AkVCam::FormatsList> AkVCam::IpcBridgePrivate::readFormats(QSettings &settings) const
{
    QList<FormatsList> formatsMatrix;
    QList<QStringList> strFormatsMatrix;
    settings.beginGroup("Formats");
    auto nFormats = settings.beginReadArray("formats");

    for (int i = 0; i < nFormats; i++) {
        settings.setArrayIndex(i);
        auto pixFormats = settings.value("format").toString().split(',');
        auto widths = settings.value("width").toString().split(',');
        auto heights = settings.value("height").toString().split(',');
        auto frameRates = settings.value("fps").toString().split(',');

        auto trim = [] (const QString &str) {
            return str.trimmed();
        };

        std::transform(pixFormats.begin(),
                       pixFormats.end(),
                       pixFormats.begin(), trim);
        std::transform(widths.begin(),
                       widths.end(),
                       widths.begin(), trim);
        std::transform(heights.begin(),
                       heights.end(),
                       heights.begin(), trim);
        std::transform(frameRates.begin(),
                       frameRates.end(),
                       frameRates.begin(), trim);

        if (pixFormats.empty()
            || widths.empty()
            || heights.empty()
            || frameRates.empty())
            continue;

        strFormatsMatrix << pixFormats;
        strFormatsMatrix << widths;
        strFormatsMatrix << heights;
        strFormatsMatrix << frameRates;
        auto combinedFormats = this->combineMatrix(strFormatsMatrix);
        FormatsList formats;

        for (auto &formatList: combinedFormats) {
            auto pixFormat = VideoFormat::fourccFromString(formatList[0].trimmed().toStdString());
            auto width = formatList[1].trimmed().toUInt();
            auto height = formatList[2].trimmed().toUInt();
            auto fps = Fraction(formatList[3].toStdString());

            VideoFormat format(pixFormat,
                               int(width),
                               int(height),
                               {fps});

            if (format)
                formats << format;
        }

        formatsMatrix << formats;
    }

    settings.endArray();
    settings.endGroup();

    return formatsMatrix;
}

QList<AkVCam::DeviceInfo> AkVCam::IpcBridgePrivate::readDevicesConfigs() const
{
    QSettings settings(QCoreApplication::organizationName(),
                       "VirtualCamera");
    auto availableFormats = this->readFormats(settings);
    QList<DeviceInfo> devices;

    settings.beginGroup("Cameras");
    auto nCameras = settings.beginReadArray("cameras");

    for (int i = 0; i < nCameras; i++) {
        settings.setArrayIndex(i);
        auto description = settings.value("description").toString();
        auto driver = settings.value("driver").toString();
        auto bus = settings.value("bus").toString();
        auto formats = settings.value("formats").toStringList();
        FormatsList formatsList;

        for (auto &format: formats) {
            auto index = format.trimmed().toInt() - 1;

            if (index < 0 || index >= availableFormats.size())
                continue;

            if (!availableFormats[index].empty())
                formatsList << availableFormats[index];
        }

        if (!formatsList.isEmpty())
            devices << DeviceInfo {0,
                                   "",
                                   description,
                                   driver,
                                   bus,
                                   formatsList};
    }

    settings.endArray();
    settings.endGroup();

    return devices;
}

AkVCam::FormatsList AkVCam::IpcBridgePrivate::formatsFromSettings(const QString &deviceId,
                                                                  const QList<DeviceInfo> &devicesInfo) const
{
    int fd = open(deviceId.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

    if (fd < 0)
        return {};

    QString driver;
    QString description;
    QString bus;
    v4l2_capability capability;
    memset(&capability, 0, sizeof(v4l2_capability));

    if (this->xioctl(fd, VIDIOC_QUERYCAP, &capability) >= 0) {
        driver = reinterpret_cast<const char *>(capability.driver);
        description = reinterpret_cast<const char *>(capability.card);
        bus = reinterpret_cast<const char *>(capability.bus_info);
    }

    close(fd);

    for (auto &devInfo: devicesInfo) {
        if (devInfo.driver.isEmpty()
            && devInfo.description.isEmpty()
            && devInfo.bus.isEmpty())
            continue;

        if ((devInfo.driver.isEmpty() || devInfo.driver == driver)
            && (devInfo.description.isEmpty() || devInfo.description == description)
            && (devInfo.bus.isEmpty() || devInfo.bus == bus))
            return devInfo.formats;
    }

    return {};
}

void AkVCam::IpcBridgePrivate::setFps(int fd, const v4l2_fract &fps)
{
    v4l2_streamparm streamparm;
    memset(&streamparm, 0, sizeof(streamparm));
    streamparm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

    if (this->xioctl(fd, VIDIOC_G_PARM, &streamparm) >= 0)
        if (streamparm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) {
            streamparm.parm.capture.timeperframe.numerator = fps.denominator;
            streamparm.parm.capture.timeperframe.denominator = fps.numerator;
            this->xioctl(fd, VIDIOC_S_PARM, &streamparm);
        }
}

bool AkVCam::IpcBridgePrivate::initReadWrite(quint32 bufferSize)
{
    this->m_buffers.resize(1);
    this->m_buffers[0].length = bufferSize;
    this->m_buffers[0].start = new char[bufferSize];

    if (!this->m_buffers[0].start) {
        this->m_buffers.clear();

        return false;
    }

    memset(this->m_buffers[0].start, 0, bufferSize);

    return true;
}

bool AkVCam::IpcBridgePrivate::initMemoryMap()
{
    v4l2_requestbuffers requestBuffers;
    memset(&requestBuffers, 0, sizeof(requestBuffers));
    requestBuffers.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    requestBuffers.memory = V4L2_MEMORY_MMAP;
    requestBuffers.count = __u32(this->m_nBuffers);

    if (this->xioctl(this->m_fd, VIDIOC_REQBUFS, &requestBuffers) < 0)
        return false;

    if (requestBuffers.count < 1)
        return false;

    this->m_buffers.resize(int(requestBuffers.count));
    bool error = false;

    for (int i = 0; i < int(requestBuffers.count); i++) {
        v4l2_buffer buffer;
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = __u32(i);

        if (this->xioctl(this->m_fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            error = true;

            break;
        }

        this->m_buffers[i].length = buffer.length;
        this->m_buffers[i].start =
                reinterpret_cast<char *>(mmap(nullptr,
                                              buffer.length,
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED,
                                              this->m_fd,
                                              buffer.m.offset));

        if (this->m_buffers[i].start == MAP_FAILED) {
            error = true;

            break;
        }
    }

    if (error) {
        for (qint32 i = 0; i < this->m_buffers.size(); i++)
            munmap(this->m_buffers[i].start, this->m_buffers[i].length);

        this->m_buffers.clear();

        return false;
    }

    return true;
}

bool AkVCam::IpcBridgePrivate::initUserPointer(quint32 bufferSize)
{
    v4l2_requestbuffers requestBuffers;
    memset(&requestBuffers, 0, sizeof(requestBuffers));
    requestBuffers.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    requestBuffers.memory = V4L2_MEMORY_USERPTR;
    requestBuffers.count = __u32(this->m_nBuffers);

    if (this->xioctl(this->m_fd, VIDIOC_REQBUFS, &requestBuffers) < 0)
        return false;

    this->m_buffers.resize(int(requestBuffers.count));
    bool error = false;

    for (int i = 0; i < int(requestBuffers.count); i++) {
        this->m_buffers[i].length = bufferSize;
        this->m_buffers[i].start = new char[bufferSize];

        if (!this->m_buffers[i].start) {
            error = true;

            break;
        }

        memset(this->m_buffers[i].start, 0, bufferSize);
    }

    if (error) {
        for (qint32 i = 0; i < this->m_buffers.size(); i++)
            delete [] this->m_buffers[i].start;

        this->m_buffers.clear();

        return false;
    }

    return true;
}

void AkVCam::IpcBridgePrivate::initDefaultFormats()
{
    QVector<PixelFormat> pixelFormats = {
        PixelFormatYUY2,
        PixelFormatUYVY,
        PixelFormatRGB32,
        PixelFormatRGB24,
    };
    QVector<QPair<int , int>> resolutions = {
        { 640,  480},
        { 160,  120},
        { 320,  240},
        { 800,  600},
        {1280,  720},
        {1920, 1080},
    };

    for (auto &format: pixelFormats)
        for (auto &resolution: resolutions)
            this->m_defaultFormats << VideoFormat(format,
                                                  resolution.first,
                                                  resolution.second,
        {{30, 1}});
}

bool AkVCam::IpcBridgePrivate::startOutput()
{
    bool error = false;

    if (this->m_ioMethod == IoMethodMemoryMap) {
        for (int i = 0; i < this->m_buffers.size(); i++) {
            v4l2_buffer buffer;
            memset(&buffer, 0, sizeof(buffer));
            buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = __u32(i);

            if (this->xioctl(this->m_fd, VIDIOC_QBUF, &buffer) < 0)
                error = true;
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

        if (this->xioctl(this->m_fd, VIDIOC_STREAMON, &type) < 0)
            error = true;
    } else if (this->m_ioMethod == IoMethodUserPointer) {
        for (int i = 0; i < this->m_buffers.size(); i++) {
            v4l2_buffer buffer;
            memset(&buffer, 0, sizeof(buffer));
            buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            buffer.memory = V4L2_MEMORY_USERPTR;
            buffer.index = __u32(i);
            buffer.m.userptr = ulong(this->m_buffers[i].start);
            buffer.length = __u32(this->m_buffers[i].length);

            if (this->xioctl(this->m_fd, VIDIOC_QBUF, &buffer) < 0)
                error = true;
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

        if (this->xioctl(this->m_fd, VIDIOC_STREAMON, &type) < 0)
            error = true;
    }

    if (error)
        self->deviceStop({});

    return !error;
}

void AkVCam::IpcBridgePrivate::stopOutput()
{
    if (this->m_ioMethod == IoMethodMemoryMap
        || this->m_ioMethod == IoMethodUserPointer) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        this->xioctl(this->m_fd, VIDIOC_STREAMOFF, &type);
    }
}

void AkVCam::IpcBridgePrivate::updateDevices()
{
    decltype(this->m_devices) devices;
    decltype(this->m_descriptions) descriptions;
    decltype(this->m_devicesFormats) devicesFormats;
    QStringList virtualDevices;

    QDir devicesDir("/dev");

    auto devicesFiles = devicesDir.entryList(QStringList() << "video*",
                                             QDir::System
                                             | QDir::Readable
                                             | QDir::Writable
                                             | QDir::NoSymLinks
                                             | QDir::NoDotAndDotDot
                                             | QDir::CaseSensitive,
                                             QDir::Name);

    for (const QString &devicePath: devicesFiles) {
        auto fileName = devicesDir.absoluteFilePath(devicePath);
        int fd = open(fileName.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

        if (fd < 0)
            continue;

        v4l2_capability capability;
        memset(&capability, 0, sizeof(v4l2_capability));

        if (this->xioctl(fd, VIDIOC_QUERYCAP, &capability) >= 0
            && capability.capabilities & V4L2_CAP_VIDEO_OUTPUT) {
            if (this->isSplitDevice(fileName)) {
                virtualDevices << this->connectedDevices(fileName);
            } else {
                virtualDevices << fileName;
            }
        }

        close(fd);
    }

    auto devicesInfo = this->readDevicesConfigs();

    for (auto &device: virtualDevices) {
        int fd = open(device.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

        if (fd < 0)
            continue;

        auto isSplit = isSplitDevice(device);
        auto formats = isSplit?
                           this->formats(fd):
                           this->formatsFromSettings(device, devicesInfo);

        if (formats.empty() && !isSplit)
            formats = this->m_defaultFormats;

        if (!formats.empty()) {
            v4l2_capability capability;
            memset(&capability, 0, sizeof(v4l2_capability));
            QString description;

            if (this->xioctl(fd, VIDIOC_QUERYCAP, &capability) >= 0)
                description = reinterpret_cast<const char *>(capability.card);

            devices << device;
            descriptions[device] = description;
            devicesFormats[device] = formats;
        }

        close(fd);
    }

    this->m_descriptions = descriptions;
    this->m_devicesFormats = devicesFormats;

    if (this->m_devices != devices) {
        if (!this->m_devices.isEmpty())
            this->m_fsWatcher->removePaths(this->m_devices);

        this->m_devices = devices;

        if (!this->m_devices.isEmpty())
            this->m_fsWatcher->addPaths(this->m_devices);
    }
}

void AkVCam::IpcBridgePrivate::onDirectoryChanged()
{
    this->updateDevices();
}

void AkVCam::IpcBridgePrivate::onFileChanged()
{
}

QStringList AkVCam::IpcBridgePrivate::listDrivers()
{
    auto modules = QString("/lib/modules/%1/modules.dep")
                   .arg(QSysInfo::kernelVersion());
    QFile file(modules);

    if (!file.open(QIODevice::ReadOnly))
        return {};

    QStringList supportedDrivers;

    for (auto &function: *this->driverFunctions())
         supportedDrivers << function.driver;

    QStringList drivers;

    forever {
        QByteArray line = file.readLine();

        if (line.isEmpty())
            break;

        auto driver = QFileInfo(line.left(line.indexOf(':'))).baseName();

        if (supportedDrivers.contains(driver))
            drivers << driver;
    }

    file.close();

    return drivers;
}

QString AkVCam::IpcBridgePrivate::preferredDriver() const
{
    QSettings settings(QCoreApplication::organizationName(),
                       "VirtualCamera");

    // Read 'General' section
    return settings.value("driver", "akvcam").toString();
}

QString AkVCam::IpcBridgePrivate::locateDriverPath() const
{
    QString driverPath;

    for (auto it = this->m_driverPaths.rbegin();
         it != this->m_driverPaths.rend();
         it++) {
        auto path = QString::fromStdWString(*it);

        if (!QFileInfo::exists(path + "/Makefile"))
            continue;

        if (!QFileInfo::exists(path + "/dkms.conf"))
            continue;

        driverPath = path;

        break;
    }

    return driverPath;
}

QString AkVCam::IpcBridgePrivate::compileDriver(const QString &path)
{
    QProcess make;
    make.setWorkingDirectory(path);
    make.start("make");
    make.waitForFinished();

    if (make.exitCode() != 0)
        return {};

    for (auto &driver: this->supportedDrivers())
        if (QFileInfo::exists(path + "/" + driver + ".ko"))
            return driver;

    return {};
}

QString AkVCam::IpcBridgePrivate::currentDriver(const std::string &deviceId)
{
    this->m_driverPath.clear();

    if (!deviceId.empty())
        for (auto &function: *this->driverFunctions())
            if (function.canHandle(deviceId))
                return function.driver;

    auto driverPath = this->locateDriverPath();

    if (!driverPath.isEmpty()) {
        auto driver = this->compileDriver(driverPath);

        if (!driver.isEmpty()) {
            this->m_driverPath = driverPath + "/" + driver + ".ko";

            return driver;
        }
    }

    auto availableDrivers = this->listDrivers();
    auto preferredDriver = this->preferredDriver();

    if (availableDrivers.contains(preferredDriver))
        return preferredDriver;
    else if (!availableDrivers.isEmpty())
        return availableDrivers.front();

    return {};
}

QString AkVCam::IpcBridgePrivate::cleanDescription(const QString &description) const
{
    QString desc;

    for (auto &c: description)
        if (QString("'\"\\,$`").contains(c))
            desc += ' ';
        else
            desc += c;

    desc = desc.simplified();

    if (desc.isEmpty())
        desc = "Virtual Camera";

    return desc;
}

bool AkVCam::IpcBridgePrivate::isModuleLoaded(const QString &driver) const
{
    QProcess lsmod;
    lsmod.start("lsmod");
    lsmod.waitForFinished();

    if (lsmod.exitCode() != 0)
        return false;

    for (auto &line: lsmod.readAllStandardOutput().split('\n'))
        if (line.trimmed().startsWith(driver.toUtf8() + ' '))
            return true;

    return false;
}

bool AkVCam::IpcBridgePrivate::canHandleAkVCam(const std::string &deviceId)
{
    int fd = open(deviceId.c_str(), O_RDWR | O_NONBLOCK, 0);

    if (fd < 0)
        return false;

    QString driver;
    v4l2_capability capability;
    memset(&capability, 0, sizeof(v4l2_capability));

    if (this->xioctl(fd, VIDIOC_QUERYCAP, &capability) >= 0)
        driver = reinterpret_cast<const char *>(capability.driver);

    close(fd);

    return driver == "akvcam";
}

QList<AkVCam::DeviceInfo> AkVCam::IpcBridgePrivate::devicesInfo() const
{
    QList<DeviceInfo> devices;
    QDir devicesDir("/dev");
    auto devicesFiles = devicesDir.entryList(QStringList() << "video*",
                                             QDir::System
                                             | QDir::Readable
                                             | QDir::Writable
                                             | QDir::NoSymLinks
                                             | QDir::NoDotAndDotDot
                                             | QDir::CaseSensitive,
                                             QDir::Name);

    for (const QString &devicePath: devicesFiles) {
        auto fileName = devicesDir.absoluteFilePath(devicePath);
        int fd = open(fileName.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

        if (fd < 0)
            continue;

        v4l2_capability capability;
        memset(&capability, 0, sizeof(v4l2_capability));

        if (this->xioctl(fd, VIDIOC_QUERYCAP, &capability) >= 0) {
            QString driver = reinterpret_cast<const char *>(capability.driver);

            if (capability.capabilities & V4L2_CAP_VIDEO_OUTPUT
                && driver == "v4l2 loopback") {
                devices << DeviceInfo {QString(fileName).remove("/dev/video").toInt(),
                                       fileName,
                                       reinterpret_cast<const char *>(capability.card),
                                       reinterpret_cast<const char *>(capability.driver),
                                       reinterpret_cast<const char *>(capability.bus_info),
                                       {}};
            }
        }

        close(fd);
    }

    return devices;
}

std::string AkVCam::IpcBridgePrivate::deviceCreateAkVCam(const std::wstring &description,
                                                         const std::vector<VideoFormat> &formats)
{
    return {};
}

void AkVCam::IpcBridgePrivate::deviceDestroyAkVCam(const std::string &deviceId)
{
}

bool AkVCam::IpcBridgePrivate::changeDescriptionAkVCam(const std::string &deviceId,
                                                       const std::wstring &description)
{
    return false;
}

QString AkVCam::IpcBridgePrivate::destroyAllDevicesAkVCam()
{
    return {};
}

bool AkVCam::IpcBridgePrivate::canHandleV4L2Loopback(const std::string &deviceId)
{
    int fd = open(deviceId.c_str(), O_RDWR | O_NONBLOCK, 0);

    if (fd < 0)
        return false;

    QString driver;
    v4l2_capability capability;
    memset(&capability, 0, sizeof(v4l2_capability));

    if (this->xioctl(fd, VIDIOC_QUERYCAP, &capability) >= 0)
        driver = reinterpret_cast<const char *>(capability.driver);

    close(fd);

    return driver == "v4l2 loopback";
}

std::string AkVCam::IpcBridgePrivate::deviceCreateV4L2Loopback(const std::wstring &description,
                                                               const std::vector<VideoFormat> &formats)
{
    int deviceNR = -1;
    QString devicePath;

    for (int i = 0; i < MAX_CAMERAS; i++) {
        devicePath = QString("/dev/video%1").arg(i);

        if (!QFileInfo::exists(devicePath)) {
            deviceNR = i;

            break;
        }
    }

    if (deviceNR < 0)
        return {};

    QTemporaryDir tempDir;
    QFile cmds(tempDir.path() + "/akvcam_exec.sh");

    if (!cmds.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};

    cmds.setPermissions(QFileDevice::ReadOwner
                        | QFileDevice::WriteOwner
                        | QFileDevice::ExeOwner
                        | QFileDevice::ReadUser
                        | QFileDevice::WriteUser
                        | QFileDevice::ExeUser);

    auto devices = this->devicesInfo();
    devices << DeviceInfo {deviceNR,
                           devicePath,
                           QString::fromStdWString(description),
                           "",
                           "",
                           {}};

    QString videoNR;
    QString cardLabel;

    for (auto &device: devices) {
        if (!videoNR.isEmpty())
            videoNR += ',';

        videoNR += QString("%1").arg(device.nr);

        if (!cardLabel.isEmpty())
            cardLabel += ',';

        cardLabel += this->cleanDescription(device.description);
    }

    cmds.write("rmmod v4l2loopback 2>/dev/null\n");

    if (this->m_driverPath.isEmpty()) {
        cmds.write("sed -i '/v4l2loopback/d' /etc/modules 2>/dev/null\n");
        cmds.write("sed -i '/v4l2loopback/d' /etc/modules-load.d/*.conf 2>/dev/null\n");
        cmds.write("sed -i '/v4l2loopback/d' /etc/modprobe.d/*.conf 2>/dev/null\n");
        cmds.write("echo v4l2loopback > /etc/modules-load.d/v4l2loopback.conf\n");
        cmds.write(QString("echo options v4l2loopback devices=%1 'card_label=\"%2\"' "
                           "> /etc/modprobe.d/v4l2loopback.conf\n")
                   .arg(devices.size()).arg(cardLabel).toUtf8());
        cmds.write(QString("modprobe v4l2loopback video_nr=%1 card_label=\"%2\"\n")
                   .arg(videoNR).arg(cardLabel).toUtf8());
    } else {
        QFileInfo info(this->m_driverPath);
        auto dir = info.dir().canonicalPath();
        cmds.write(QString("cd '%1'\n").arg(dir).toUtf8());

        if (!this->isModuleLoaded("videodev"))
            cmds.write("modprobe videodev\n");

        cmds.write(QString("insmod v4l2loopback.ko video_nr=%1 card_label=\"%2\"\n")
                   .arg(videoNR).arg(cardLabel).toUtf8());
    }

    cmds.close();

    if (!this->sudo(this->m_rootMethod, {"sh", cmds.fileName()}))
        return {};

    /* udev can take some time to give proper file permissions to the device,
     * so we wait until el character device become fully accesible.
     */
    int fd = -1;

    forever {
        fd = open(devicePath.toStdString().c_str(), O_RDWR | O_NONBLOCK, 0);

        if (fd != -EPERM)
            break;

        QThread::msleep(500);
    }

    if (fd < 0)
        return {};

    close(fd);

    auto devicesInfo = this->readDevicesConfigs();
    QSettings settings(QCoreApplication::organizationName(),
                       "VirtualCamera");
    int i = 0;
    int j = 0;

    for (auto &device: this->devicesInfo()) {
        if (device.path == devicePath)
            device.formats =
                    QVector<VideoFormat>::fromStdVector(formats).toList();
        else
            device.formats =
                    this->formatsFromSettings(device.path, devicesInfo);

        if (device.formats.empty())
            device.formats = this->m_defaultFormats;

        QStringList formatsIndex;

        for (int i = 0; i < device.formats.size(); i++)
            formatsIndex << QString("%1").arg(i + j + 1);

        settings.beginGroup("Cameras");
        settings.beginWriteArray("cameras");
        settings.setArrayIndex(i);
        settings.setValue("driver", device.driver);
        settings.setValue("bus", device.bus);
        settings.setValue("formats", formatsIndex);
        settings.endArray();
        settings.endGroup();

        settings.beginGroup("Formats");
        settings.beginWriteArray("formats");

        for (auto &format: device.formats) {
            settings.setArrayIndex(j);
            settings.setValue("format", VideoFormat::stringFromFourcc(format.fourcc()).c_str());
            settings.setValue("width", format.width());
            settings.setValue("height", format.height());
            settings.setValue("fps", format.minimumFrameRate().toString().c_str());
            j++;
        }

        settings.endArray();
        settings.endGroup();

        i++;
    }

    return devicePath.toStdString();
}

void AkVCam::IpcBridgePrivate::deviceDestroyV4L2Loopback(const std::string &deviceId)
{
    /*
    if ((this->m_rootMethod == "su" || this->m_rootMethod == "sudo")
        && password.isEmpty())
        return false;

    if (!QRegExp("/dev/video[0-9]+").exactMatch(webcam))
        return false;

    QStringList webcams = this->webcams();

    if (webcams.isEmpty()
        || !webcams.contains(webcam))
        return false;

    QStringList webcamDescriptions;
    QStringList webcamIds;

    for (const QString &webcam: webcams) {
        webcamDescriptions << this->description(webcam);
        int id = webcam.indexOf(QRegExp("[0-9]+"));
        webcamIds << webcam.mid(id);
    }

    int id = webcam.indexOf(QRegExp("[0-9]+"));
    bool ok = false;
    id = webcam.mid(id).toInt(&ok);

    if (!ok)
        return false;

    int index = webcamIds.indexOf(QString("%1").arg(id));

    if (index < 0)
        return false;

    webcamDescriptions.removeAt(index);
    webcamIds.removeAt(index);

    if (!this->d->updateCameras(webcamIds, webcamDescriptions, password))
        return false;

    QStringList curWebcams = this->webcams();

    if (curWebcams != webcams)
        emit this->webcamsChanged(curWebcams);

    return true;
    */
}

bool AkVCam::IpcBridgePrivate::changeDescriptionV4L2Loopback(const std::string &deviceId,
                                                             const std::wstring &description)
{
    /*
    if ((this->m_rootMethod == "su" || this->m_rootMethod == "sudo")
        && password.isEmpty())
        return false;

    if (!QRegExp("/dev/video[0-9]+").exactMatch(webcam))
        return false;

    QStringList webcams = this->webcams();

    if (webcams.isEmpty()
        || !webcams.contains(webcam))
        return false;

    QStringList webcamDescriptions;
    QStringList webcamIds;

    for (const QString &webcam: webcams) {
        webcamDescriptions << this->description(webcam);
        int id = webcam.indexOf(QRegExp("[0-9]+"));
        webcamIds << webcam.mid(id);
    }

    int id = webcam.indexOf(QRegExp("[0-9]+"));
    bool ok = false;
    id = webcam.mid(id).toInt(&ok);

    if (!ok)
        return false;

    QString deviceDescription;

    if (description.isEmpty())
        deviceDescription = QString(tr("Virtual Camera %1")).arg(id);
    else
        deviceDescription = this->d->cleanupDescription(description);

    int index = webcamIds.indexOf(QString("%1").arg(id));

    if (index < 0)
        return false;

    webcamDescriptions[index] = deviceDescription;

    if (!this->d->updateCameras(webcamIds, webcamDescriptions, password))
        return false;

    QStringList curWebcams = this->webcams();

    if (curWebcams != webcams)
        emit this->webcamsChanged(curWebcams);

    return true;
    */
    return false;
}

QString AkVCam::IpcBridgePrivate::destroyAllDevicesV4L2Loopback()
{
    return {"rmmod v4l2loopback 2>/dev/null\n"
            "sed -i '/v4l2loopback/d' /etc/modules 2>/dev/null\n"
            "sed -i '/v4l2loopback/d' /etc/modules-load.d/*.conf 2>/dev/null\n"
            "sed -i '/v4l2loopback/d' /etc/modprobe.d/*.conf 2>/dev/null\n"
            "rm -f /etc/modules-load.d/v4l2loopback.conf\n"
            "rm -f /etc/modprobe.d/v4l2loopback.conf\n"};
}