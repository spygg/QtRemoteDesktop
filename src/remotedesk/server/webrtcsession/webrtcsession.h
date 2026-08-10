#ifndef WEBRTCSESSION_H
#define WEBRTCSESSION_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>
#include <memory>

#ifdef USE_WEBRTC

#include <yangrtc/YangPeerConnection8.h>
#include <yangrtc/YangPeerInfo.h>

// 一个 WebRTC 会话 = 一个浏览器客户端的 RTCPeerConnection。
// 服务端作为主叫方创建 offer（SENDONLY H.264 视频轨道），通过现有 WebSocket 通道做信令，
// 连接建立后把 VideoEncoder 输出的 H.264 帧经 metaRTC 以 RTP/SRTP 发送。
class WebRtcSession : public QObject, public YangCallbackReceive,
        public YangCallbackIce, public YangCallbackRtc, public YangCallbackSslAlert {
    Q_OBJECT
public:
    explicit WebRtcSession(QObject* parent = nullptr);
    ~WebRtcSession() override;

    // 创建 metaRTC PeerConnection + H.264 视频轨道，并生成 offer（经 localOffer 信号发出）
    bool create(const QVector<QString>& iceServers);
    void handleAnswer(const QString& sdp);
    void handleIce(const QString& candidate, const QString& mid);
    void close();

    bool isConnected() const { return connected_; }
    void sendFrame(const QByteArray& data, bool keyframe);

    // YangCallback* 纯虚实现：metaRTC 回调
    void receiveAudio(YangFrame*) override;
    void receiveVideo(YangFrame*) override;
    void receiveMsg(YangFrame*) override;
    void onIceStateChange(int32_t uid, YangIceCandidateState iceState) override;
    void onConnectionStateChange(int32_t uid, YangRtcConnectionState connectionState) override;
    void onIceCandidate(int32_t uid, char* sdp) override;
    void onIceGatheringState(int32_t uid, YangIceGatheringState gatherState) override;
    void setMediaConfig(int32_t puid, YangAudioParam* audio, YangVideoParam* video) override;
    void sendRequest(int32_t puid, uint32_t ssrc, YangRequestType req) override;
    void sslCloseAlert(int32_t uid) override;

signals:
    void localOffer(const QString& sdp);
    void localIce(const QString& candidate, const QString& mid);
    void connected();
    void failed();
    void closed();
    void keyframeRequested();          // 浏览器请求关键帧（PLI）

private:
    YangPeerInfo peerInfo_;
    std::unique_ptr<YangPeerConnection8> pc_;
    std::unique_ptr<YangRtcPacer> pacer_;
    uint64_t rtpTimestamp_ = 0;
    bool connected_ = false;
};

#endif // USE_WEBRTC
#endif // WEBRTCSESSION_H