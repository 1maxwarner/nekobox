


#include <winsock2.h>
#include <windows.h>
#include <nekobox/sys/windows/eventHandler.h>

#include <QDebug>

bool PowerOffTaskkillFilter::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    if (eventType == "windows_generic_MSG") {
        MSG *msg = static_cast<MSG *>(message);

        if (msg->message == WM_QUERYENDSESSION) {
            qDebug() << "WM_QUERYENDSESSION received";
            *result = TRUE;
            return true;
        } else if (msg->message == WM_ENDSESSION) {
            if (msg->wParam)
            {
                qDebug() << "WM_ENDSESSION received, calling cleanUpFunc";
                cleanUpFunc(0);
                return true;
            }
        } else if (msg->message == WM_POWERBROADCAST) {
            if (msg->wParam == PBT_APMSUSPEND) {
                qDebug() << "PBT_APMSUSPEND received";
                if (suspendFunc)
                    suspendFunc();
            } else if (msg->wParam == PBT_APMRESUMEAUTOMATIC ||
                       msg->wParam == PBT_APMRESUMESUSPEND ||
                       msg->wParam == PBT_APMRESUMECRITICAL) {
                qDebug() << "power resume received";
                if (resumeFunc)
                    resumeFunc();
            }
        }
    }
    return false;
}
