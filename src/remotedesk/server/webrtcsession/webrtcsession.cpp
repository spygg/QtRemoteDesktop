#include "webrtcsession.h"

#ifdef USE_WEBRTC

#include <yangrtc/YangPushData.h>

#include <QDateTime>
#include <QDebug>
#include <cstring>
#include <mutex>

// 90000Hz / 30fps ≈ 3000 ticks/frame
static const uint64_t kRtpTicksPerFrame = 3000;

WebRtcSession::WebRtcSession(QObject* parent)
    : QObject(parent)
{
}

WebRtcSession::~WebRtcSession()
{
    close();
}

bool WebRtcSession::create(const QVector<QString>& iceServers)
{
    Q_UNUSED(iceServers)

    yang_init_peerInfo(&peerInfo_);
    peerInfo_.uid = 0;
    peerInfo_.familyType = Yang_IpFamilyType_IPV4;
    peerInfo_.direction = YangSendonly;
    peerInfo_.rtc.isControlled = yangfalse;          // 服务端为主叫方，生成 offer
    peerInfo_.rtc.rtcLocalPort = 15000;              // metaRTC 会自增直到空闲端口
    peerInfo_.rtc.rtcSocketProtocol = Yang_Socket_Protocol_Udp;
    peerInfo_.rtc.iceCandidateType = YangIceHost;

    pc_ = std::unique_ptr<YangPeerConnection8>(new YangPeerConnection8(&peerInfo_, this, this, this, this));
    if (!pc_)
        return false;

    if (pc_->addVideoTrack(Yang_VED_H264) != 0) {
        qWarning() << "WebRtcSession: addVideoTrack failed";
        return false;
    }
    pc_->addTransceiver(YangMediaVideo, YangSendonly);

    pacer_ = std::unique_ptr<YangRtcPacer>(new YangRtcPacer());
    pacer_->initVideo(Yang_VED_H264, 1024);

    char* offer = nullptr;
    if (pc_->createOffer(&offer) != 0 || !offer) {
        qWarning() << "WebRtcSession: createOffer failed";
        close();
        return false;
    }
    emit localOffer(QString::fromUtf8(offer));
    delete[] offer;

    // setLocalDescription 启动本地 UDP socket + ICE agent
    pc_->setLocalDescription(nullptr);

    rtpTimestamp_ = 0;
    return true;
}

void WebRtcSession::handleAnswer(const QString& sdp)
{
    if (!pc_)
        return;
    QByteArray ba = sdp.toUtf8();
    char* ans = ba.data();
    if (pc_->setRemoteDescription(ans) != 0)
        emit failed();
}

void WebRtcSession::handleIce(const QString& candidate, const QString& mid)
{
    if (!pc_ || candidate.isEmpty())
        return;
    // metaRTC 的 addIceCandidate 期望 JSON 对象:{"candidate":"candidate:...","sdpMid":"0",...}
    // 前端只会发送裸 candidate 字符串,这里包装成 JSON 让 metaRTC 能解析
    QByteArray ba = QStringLiteral("{\"candidate\":%1,\"sdpMid\":%2}")
            .arg(QString::fromUtf8(QByteArray().append('"').append(candidate.toUtf8()).append('"')))
            .arg(QString::fromUtf8(QByteArray().append('"').append(mid.toUtf8()).append('"')))
            .toUtf8();
    if (pc_->addIceCandidate(ba.data()) != 0) {
        qWarning() << "WebRtcSession: addIceCandidate failed" << candidate.left(60);
    }
}

void WebRtcSession::sendFrame(const QByteArray& data, bool keyframe)
{
    if (!connected_ || !pc_ || !pacer_ || data.isEmpty())
        return;

    // 填充 metaRTC 视频帧。payload 需要可写缓冲（metaRTC 分包器会拷贝）。
    static QByteArray buf;
    buf = data;
    buf.detach();
    uint8_t* payload = reinterpret_cast<uint8_t*>(buf.data());

    YangFrame frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.mediaType = YangFrameTypeVideo;
    frame.frametype = keyframe ? YANG_Frametype_I : YANG_Frametype_P;
    frame.nb = static_cast<int32_t>(buf.size());
    frame.pts = static_cast<int64_t>(rtpTimestamp_);
    frame.dts = static_cast<int64_t>(rtpTimestamp_);
    frame.payload = payload;
    rtpTimestamp_ += kRtpTicksPerFrame;

    YangPushData* pushData = pacer_->getVideoData(&frame);
    if (pushData && pc_->on_video(pushData) != 0) {
        qWarning() << "WebRtcSession: on_video failed";
    }
}

void WebRtcSession::close()
{
    connected_ = false;
    if (pc_) {
        pc_->close();
        pc_.reset();
    }
    pacer_.reset();
}

void WebRtcSession::receiveAudio(YangFrame*) {}
void WebRtcSession::receiveVideo(YangFrame*) {}
void WebRtcSession::receiveMsg(YangFrame*) {}

void WebRtcSession::onIceStateChange(int32_t, YangIceCandidateState)
{
}

void WebRtcSession::onConnectionStateChange(int32_t, YangRtcConnectionState state)
{
    switch (state) {
    case Yang_Conn_State_Connected:
        connected_ = true;
        qInfo() << "WebRtcSession: connected";
        emit connected();
        break;
    case Yang_Conn_State_Failed:
        qWarning() << "WebRtcSession: failed";
        connected_ = false;
        emit failed();
        break;
    case Yang_Conn_State_Disconnected:
        qWarning() << "WebRtcSession: disconnected";
        break;
    case Yang_Conn_State_Closed:
        connected_ = false;
        emit closed();
        break;
    default:
        break;
    }
}

void WebRtcSession::onIceCandidate(int32_t, char* sdp)
{
    // metaRTC 回调给出的是标准 WebRTC JSON：{"candidate":"...","sdpMid":"0",...}
    if (sdp)
        emit localIce(QString::fromUtf8(sdp), QStringLiteral("0"));
}

void WebRtcSession::onIceGatheringState(int32_t, YangIceGatheringState)
{
}

void WebRtcSession::setMediaConfig(int32_t, YangAudioParam*, YangVideoParam*)
{
}

void WebRtcSession::sendRequest(int32_t, uint32_t ssrc, YangRequestType req)
{
    if (req == Yang_Req_Sendkeyframe) {
        emit keyframeRequested();
    }
}

void WebRtcSession::sslCloseAlert(int32_t)
{
    emit failed();
}

#endif // USE_WEBRTC