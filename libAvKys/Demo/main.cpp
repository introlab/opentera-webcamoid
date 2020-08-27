#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <akvideocaps.h>
#include <ak.h>
#include <objbase.h>
#include <QDir>
#include "main.h"

int main(int argc, char* argv[])
{
    QCoreApplication app(argc,argv);

    qDebug() << "Starting...";
    //Why do we need to do that?
    //qRegisterMetaType<AkPacket>("AkPacket");
    Ak::registerTypes();
    Ak::setQmlEngine(nullptr);

/*
    HRESULT hr_init = CoInitializeEx(nullptr, COINIT_MULTITHREADED); //CoInitialize(NULL);
    if (FAILED(hr_init))
    {
        return -2;
    }
*/
    //Tell the library where to look for plugins
    //TODO Hardcoded for now...
    QStringList searchPaths;
    searchPaths << QDir::currentPath() + "/AvKysPlugins";
    AkElement::setSearchPaths(searchPaths);


    qDebug() << "MultiSrc submodules: " << AkElement::listSubModules("MultiSrc");
    qDebug() << "VirtualCamera submodules: "<< AkElement::listSubModules("VirtualCamera");

    //This should load the library. Let's see...
    auto MultiSrcPtr = AkElement::create("MultiSrc");

    auto VirtualCameraPtr = AkElement::create("VirtualCamera");
    QString deviceId;
    AkVideoCapsList formats;
/*
    QMetaObject::invokeMethod(VirtualCameraPtr.data(),
                              "createWebcam",
                              Q_RETURN_ARG(QString, deviceId),
                              Q_ARG(QString, "VirtCam"),
                              Q_ARG(AkVideoCapsList, formats));
*/
    if (MultiSrcPtr && VirtualCameraPtr)
    {
        qDebug() << "Linking Src & Dest";
        MultiSrcPtr->link(VirtualCameraPtr);


        //Set Parameters
        //Start streaming something from VLC at this address for testing...
        //MultiSrcPtr->setProperty("media", "rtsp://localhost:8554/live");
        MultiSrcPtr->setProperty("media", "C:/Big_Buck_Bunny_360_10s_30MB.mp4");
        MultiSrcPtr->setProperty("loop", false);          // Loop the video/media if you need it.
        MultiSrcPtr->setProperty("showLog", true);       // Show play log in console, similar to MPlayer and ffplay.

        //First, a virtual camera needs to be created
        VirtualCameraPtr->setProperty("media","/akvcam/video0");
        VirtualCameraPtr->setProperty("swapRgb", true);

        // start the pipeline.
        MultiSrcPtr->setState(AkElement::ElementStatePlaying);
        VirtualCameraPtr->setState(AkElement::ElementStatePlaying);

    }

    qDebug() << "Starting event loop";

    //Start event loop
    return app.exec();

}
