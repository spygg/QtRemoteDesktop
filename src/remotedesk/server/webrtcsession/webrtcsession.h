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
    // fps 用于 RTP 时间戳推进（90kHz 时钟）；旧实现硬编码 30fps，
    // 前端把帧率改成 15/60 时时间戳速率对不上，浏览器抖动缓冲会持续 underrun 或堆积。
    bool create(const QVector<QString>& iceServers, int fps = 30);
    void handleAnswer(const QString& sdp);
    void handleIce(const QString& candidate, const QString& mid);
    void close();

    // 实际推流状态（answer 已设置）。connected_ 仅反映 metaRTC 状态回调，
    // 纯媒体推流场景它恒为 false，不能用于业务判断。
    bool isConnected() const { return remoteSet_; }
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
    void answerReceived();             // answer 已生效（用于连接后强制产 IDR）

private:
    YangPeerInfo peerInfo_;
    std::unique_ptr<YangPeerConnection8> pc_;
    std::unique_ptr<YangRtcPacer> pacer_;
    uint64_t rtpTimestamp_ = 0;
    uint64_t rtpTicksPerFrame_ = 3000;  // 90000 / fps，由 create(fps) 计算
    bool connected_ = false;
    // answer 已设置（DTLS/SRTP 握手可以开始）：sendFrame 的推流门禁用它而非
    // connected_——metaRTC 在纯媒体推流（无 DataChannel）场景下不回调
    // Yang_Conn_State_Connected（实测 connected=0 次而媒体面可用），
    // 若以 connected_ 为门禁则一帧都不会推送（表现为 WebRTC 首帧后冻结）。
    // 会话失败/关闭时由 stopWebRtcSession 将本会话从 webrtcSessions_ 移除，
    // 推流随之自然停止。
    bool remoteSet_ = false;
    // answer 未生效前到达的远端 ICE 候选先排队，answer 生效后补发，
    // 否则 metaRTC addIceCandidate 在 remoteDescription 未设置时失败、候选被丢弃。
    QVector<QPair<QString, QString>> pendingIce_;
    // 向 metaRTC 添加单个远端 ICE 候选（包装成 JSON 对象）
    void addIce(const QString& candidate, const QString& mid);
};

#endif // USE_WEBRTC
#endif // WEBRTCSESSION_H