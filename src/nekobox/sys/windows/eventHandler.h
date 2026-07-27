




#pragma once
#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <functional>

class PowerOffTaskkillFilter : public QAbstractNativeEventFilter
{
public:
    PowerOffTaskkillFilter(const std::function<void(int)>& f,
                           const std::function<void()> &suspend,
                           const std::function<void()> &resume)
    {
        cleanUpFunc = f;
        suspendFunc = suspend;
        resumeFunc = resume;
    };

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
private:
    std::function<void(int)> cleanUpFunc;
    std::function<void()> suspendFunc;
    std::function<void()> resumeFunc;
};
