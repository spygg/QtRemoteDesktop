// server/screencapturer/screencapturer_agent.cpp
#include "screencapturer_agent.h"
#include "session_agent_client.h"

#include <QDebug>

bool AgentCapturer::initialize()
{
    client_ = &SessionAgentClient::shared();
    if (!client_->isStreaming()) {
        if (!initLogged_) {
            qInfo() << "AgentCapturer: session agent not streaming yet, "
                       "local capture will be used until it is";
            initLogged_ = true;
        }
        return false;
    }
    qInfo() << "AgentCapturer: using session agent frames ("
            << client_->width() << "x" << client_->height() << ")";
    return true;
}

bool AgentCapturer::captureFrame(QImage& outImage, bool* updated)
{
    if (!client_ || !client_->isStreaming())
        return false;
    return client_->takeFrame(outImage, updated);
}

int AgentCapturer::width() const
{
    return client_ ? client_->width() : 0;
}

int AgentCapturer::height() const
{
    return client_ ? client_->height() : 0;
}

void AgentCapturer::setPaused(bool paused)
{
    if (client_)
        client_->setPaused(paused);
}

void AgentCapturer::requestFullCapture()
{
    // 代理端由 PipeWire 驱动，恒为整帧，无需增量语义。
}

void AgentCapturer::requestForceFrame()
{
    if (client_)
        client_->requestFrame();
}
