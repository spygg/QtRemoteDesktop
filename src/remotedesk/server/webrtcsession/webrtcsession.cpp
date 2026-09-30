#include "webrtcsession.h"

#ifdef USE_WEBRTC

#include <yangrtc/YangPushData.h>

#include <QDateTime>
#include <QDebug>
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#include <QRandomGenerator>
#else
#include <QUuid>
#endif
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

bool WebRtcSession::create(const QVector<QString>& iceServers, int fps)
{
    Q_UNUSED(iceServers)

    // RTP 时钟 90kHz：每帧推进 90000/fps 个 tick（旧值固定 3000 = 30fps）
    rtpTicksPerFrame_ = 90000ULL / static_cast<uint64_t>(qMax(1, fps));

    yang_init_peerInfo(&peerInfo_);
    peerInfo_.uid = 0;
    peerInfo_.familyType = Yang_IpFamilyType_IPV4;
    peerInfo_.direction = YangSendonly;
    // 关键：服务端必须工作在 isControlled=true（被控方）。
    // 浏览器主机候选默认 mDNS 化（xxxx.local），metaRTC 无法解析，向其发送的
    // ICE 连通性检查只会打到 255.255.255.255；若 isControlled=false（主叫方），
    // metaRTC 仅在“收到自己发出的 STUN 响应”后才启动 DTLS 握手——永远等不到，
    // 浏览器端 dtls 卡 connecting、videoWidth=0（黑屏根因）。
    // 翻转后被控方：offer 携带 a=setup:passive（见 YangSdp.c），浏览器回答
    // a=setup:active 并主动发起 DTLS；浏览器对本机 15000 端口的 STUN 检查
    // 本来就能到达并得到应答，DTLS ClientHello 走同一路径，握手可完成。
    peerInfo_.rtc.isControlled = yangtrue;
    peerInfo_.rtc.rtcLocalPort = 15000;              // metaRTC 会自增直到空闲端口
    peerInfo_.rtc.rtcSocketProtocol = Yang_Socket_Protocol_Udp;
    peerInfo_.rtc.iceCandidateType = YangIceHost;
    // 关键：metaRTC 仅在 iceTransportPolicy==YangTransportAll 时才收集 host 候选；
    // 默认值不满足，导致服务端从不产生 ICE 候选、与浏览器永远无法配对。
    peerInfo_.rtc.iceTransportPolicy = YangTransportAll;
    peerInfo_.iceMode = YangIceModeFull;

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
    // setLocalDescription 必须先于发 offer，且必须传真实 SDP：
    // metaRTC 在 sdp==NULL 时直接返回，UDP socket 与 ICE agent 都不会启动，
    // 导致服务端不产生任何 ICE 候选，与浏览器永远无法配对。
    if (pc_->setLocalDescription(offer) != 0)
        qWarning() << "WebRtcSession: setLocalDescription failed";
    else
        qInfo() << "WebRtcSession: local UDP + ICE agent started";
    // [DIAG] offer 关键行（SSRC/PT/setup/ice-ufrag），WebRTC RTP 黑屏排查
    {
        QString sdpText = QString::fromUtf8(offer);
        QStringList key;
        const QStringList lines = sdpText.split('\n');
        for (const QString& l : lines) {
            if (l.startsWith(QLatin1String("a=ssrc")) || l.startsWith(QLatin1String("a=rtpmap")) ||
                l.startsWith(QLatin1String("a=setup")) || l.startsWith(QLatin1String("a=ice-ufrag")) ||
                l.startsWith(QLatin1String("a=ice-pwd")) || l.startsWith(QLatin1String("a=sendonly")) ||
                l.startsWith(QLatin1String("m=video")) || l.startsWith(QLatin1String("a=mid")))
                key << l.trimmed();
        }
        qInfo() << "WebRtcSession offer keylines:" << key.join(" | ");
    }
    emit localOffer(QString::fromUtf8(offer));
    delete[] offer;

    // RFC 3550: RTP 时间戳初始值应随机，而非从 0 开始。
    // 固定 0 起点会让多会话/重连后的时间戳序列高度可预测，且个别实现
    // 对启动时即出现的小时间戳（含回绕判定边界）处理不佳。
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    rtpTimestamp_ = QRandomGenerator::global()->bounded(1u, 0x7FFFFFFFu);
#else
    // Qt < 5.10 回退：QUuid v4（Unix 下取自 /dev/urandom），映射到 [1, 0x7FFFFFFE]
    {
        const QUuid u = QUuid::createUuid();
        quint32 v = 0;
        std::memcpy(&v, &u.data4[0], sizeof(v));
        rtpTimestamp_ = (v % 0x7FFFFFFEu) + 1u;
    }
#endif
    return true;
}

void WebRtcSession::handleAnswer(const QString& sdp)
{
    if (!pc_)
        return;
    QByteArray ba = sdp.toUtf8();
    char* ans = ba.data();
    qInfo() << "WebRtcSession: answer received, len =" << sdp.size();
    if (pc_->setRemoteDescription(ans) != 0) {
        qWarning() << "WebRtcSession: setRemoteDescription(answer) failed, len =" << sdp.size();
        emit failed();
        return;
    }
    qInfo() << "WebRtcSession: answer set OK";
    remoteSet_ = true; // answer 生效，允许开始推送媒体帧
    // [IDR-ON-CONNECT] 通知 RDPServer：answer 生效后强制编码器重建产 IDR，
    // metaRTC 纯媒体推流不回调 connected，且软编 hasIdr 后不再主动产 IDR，
    // 新接入客户端必须拿到关键帧才能出图。
    emit answerReceived();
    // 补发 answer 前排队等待的远端 ICE 候选（metaRTC 需 remoteDescription 设置后才能接受候选）
    for (const auto& ic : pendingIce_) {
        addIce(ic.first, ic.second);
    }
    pendingIce_.clear();
}

// 向 metaRTC 添加单个远端 ICE 候选（包装成 JSON 对象）
void WebRtcSession::addIce(const QString& candidate, const QString& mid)
{
    if (!pc_ || candidate.isEmpty())
        return;
    QByteArray ba = QStringLiteral("{\"candidate\":%1,\"sdpMid\":%2}")
            .arg(QString::fromUtf8(QByteArray().append('"').append(candidate.toUtf8()).append('"')))
            .arg(QString::fromUtf8(QByteArray().append('"').append(mid.toUtf8()).append('"')))
            .toUtf8();
    if (pc_->addIceCandidate(ba.data()) != 0)
        qWarning() << "WebRtcSession: addIceCandidate failed" << candidate.left(60);
}

void WebRtcSession::handleIce(const QString& candidate, const QString& mid)
{
    if (!pc_ || candidate.isEmpty())
        return;
    // answer 未生效（remoteDescription 未设置）时 metaRTC 无法接受远端候选，
    // 前端 ICE 候选通常早于 answer 到达，先排队等 answer 后统一补发。
    if (!remoteSet_) {
        pendingIce_.append({ candidate, mid });
        qInfo() << "WebRtcSession: remote ICE queued (pre-answer)" << candidate.left(60);
        return;
    }
    qInfo() << "WebRtcSession: remote ICE added" << candidate.left(60);
    addIce(candidate, mid);
}

void WebRtcSession::sendFrame(const QByteArray& data, bool keyframe)
{
    // 门禁用 remoteSet_（answer 已设置）而非 connected_：metaRTC 纯媒体推流
    // （无 DataChannel）场景不回调 Connected，用 connected_ 会导致一帧都不推
    // （WebRTC 首帧后冻结的根因）。失败/关闭由 stopWebRtcSession 移除会话兜底。
    if (!remoteSet_ || !pc_ || !pacer_ || data.isEmpty())
        return;

    // [DIAG] 确认服务端是否真的把帧推给 metaRTC on_video（WebRTC RTP 黑屏排查）
    {
        static int64_t s_cnt = 0;
        if (++s_cnt % 30 == 1)
            qInfo() << "WebRtcSession sendFrame #" << s_cnt << " key=" << keyframe << " sz=" << data.size();
    }

    // metaRTC 的 H.264 发送器(yang_push_h264_video)接收“单个 NALU”(无起始码)：
    // 小 NALU 直接 memcpy 进 RTP Raw 载荷；大 NALU(>kRtpMaxPayloadSize)走 FU-A。
    // 但 pushVideo 的上层（yang_pushVideo_getData）对 I 帧要求完整 Annex-B 帧，
    // 详见下方发送逻辑说明。FFmpeg 软/硬编码器输出是 Annex-B(00 00 00 01 起始码
    // 包裹 SPS/PPS/IDR 多 NALU)，先拆分 NALU 再按下方策略合成发送。
    // [P2] 去掉每帧无条件 detach 深拷贝（30fps × 8KB-1MB = 纯浪费的带宽/延迟）：
    // QByteArray COW 下 buf(data) 只浅共享；只有真正写 buf（下方 wbase 重写 NALU）
    // 时才会惰性 detach。纯读路径（单 NALU 直发）零拷贝。
    QByteArray buf(data);
    const uint8_t* base = reinterpret_cast<const uint8_t*>(buf.constData());
    const int n = buf.size();

    // 解析本帧所有 NALU：[offset,len](不含起始码) 与 NAL type。
    // 支持 3/4 字节起始码；NALU 区间到下一个起始码(或末尾)为止。
    struct Nalu { int off; int len; uint8_t type; };
    QVector<Nalu> nalus;
    int i = 0;
    while (i + 2 < n) {
        bool sc = false; int scLen = 0;
        if (base[i] == 0x00 && base[i + 1] == 0x00) {
            if (base[i + 2] == 0x01) { sc = true; scLen = 3; }
            else if (i + 3 < n && base[i + 2] == 0x00 && base[i + 3] == 0x01) { sc = true; scLen = 4; }
        }
        if (!sc) { ++i; continue; }
        int start = i + scLen;
        int j = start;
        // [B21] 必须扫到缓冲末尾：旧条件 j+2<n 使末尾 NALU 恒丢最后 2 字节
        //（起始码探测本身需要 j+2/j+3 越界保护，见下）。
        while (j < n) {
            if (j + 2 < n && base[j] == 0x00 && base[j + 1] == 0x00 &&
                (base[j + 2] == 0x01 || (j + 3 < n && base[j + 2] == 0x00 && base[j + 3] == 0x01)))
                break;
            ++j;
        }
        if (j > start) {
            uint8_t t = base[start] & 0x1F;
            nalus.append({start, j - start, t});
        }
        i = j;
        if (i >= n) break;
    }

    if (nalus.isEmpty()) {
        qWarning() << "WebRtcSession: no NALU parsed, drop frame sz=" << n;
        return;
    }

    // metaRTC pushVideo 的 I 帧处理要求“完整 Annex-B 帧”（内部扫描 IDR 起始码、
    // 解析 SPS/PPS 并通过 STAP-A 发出、再定位到首 IDR 交给 on_video）。
    // 若把整帧(含多 IDR slice)整体喂入，FU-A 会把起始码当 NAL 头(type=0)分片；
    // 若把单个 IDR NALU 标为 I 帧喂入，扫描不到 SPS/PPS 返回 NULL 直接丢弃
    // （此前 KEYFRAME sent=2 只剩 SPS/PPS，浏览器收不到 IDR → 黑屏）。
    // 正确做法：合成 [SC][SPS][SC][PPS][SC][IDR1] 作为 I 帧喂入，
    // 同帧其余 IDR slice 按 P 帧（frametype=P 绕过 keyframe 分支）逐 NALU 发出，
    // 与首 IDR 共享同一 RTP 时间戳，浏览器按时间戳拼帧解码。
    uint8_t* wbase = reinterpret_cast<uint8_t*>(buf.data());
    int sent = 0;

    // 单 NALU 发送 helper（frametype 显式指定；marker=1 表示帧内最后一个 NALU）
    auto sendNalu = [&](const Nalu& nu, int frametype, int marker) -> bool {
        YangFrame frame;
        std::memset(&frame, 0, sizeof(frame));
        frame.mediaType = YangFrameTypeVideo;
        frame.frametype = frametype;
        frame.nb = nu.len;
        frame.pts = static_cast<int64_t>(rtpTimestamp_);
        frame.dts = static_cast<int64_t>(rtpTimestamp_);
        frame.payload = wbase + nu.off;
        frame.marker = marker;
        YangPushData* pushData = pacer_->getVideoData(&frame);
        if (pushData) {
            int rc = pc_->on_video(pushData);
            if (rc != 0) {
                static int s_err = 0;
                if (++s_err % 30 == 1)
                    qWarning() << "WebRtcSession: on_video failed rc=" << rc;
                return false;
            }
            return true;
        }
        return false;
    };

    // 分类 NALU：SPS(7)/PPS(8)/IDR(5)/其他
    int spsIdx = -1, ppsIdx = -1;
    QVector<int> idrIdx;
    for (int k = 0; k < nalus.size(); ++k) {
        uint8_t t = nalus[k].type;
        if (t == 7) spsIdx = k;
        else if (t == 8) ppsIdx = k;
        else if (t == 5) idrIdx.append(k);
    }

    if (!idrIdx.isEmpty() && spsIdx >= 0 && ppsIdx >= 0) {
        // [IDR-FIX] 关键帧不再合成“含起始码的完整 Annex-B 帧”喂入：
        // metaRTC 的 on_video(yang_push_h264_video) 按“单个无起始码 NALU”打包，
        // 若把 [SC4][SPS][SC4][PPS][SC4][IDR] 整帧喂入，single_nalu2 会把起始码
        // 当 NAL 头(type 0)打进 RTP payload，Chrome H264 depacketizer 解析失败→黑屏。
        // 正确做法：SPS/PPS/IDR slice 各作为独立 NALU 发送（与 P 帧同路径），
        // 同一时间戳，帧内最后一个 NALU marker=1；SPS/PPS 由解码器带内配置。
        if (sendNalu(nalus[spsIdx], YANG_Frametype_P, 0)) ++sent;
        if (sendNalu(nalus[ppsIdx], YANG_Frametype_P, 0)) ++sent;
        // 所有 IDR slice 独立发送，最后一个 slice marker=1（帧尾）
        for (int k = 0; k < idrIdx.size(); ++k) {
            if (sendNalu(nalus[idrIdx[k]], YANG_Frametype_P, (k == idrIdx.size() - 1) ? 1 : 0)) ++sent;
        }
    } else {
        // P 帧/无完整参数集的帧：只发视频 slice（type 1/5），跳过 SEI(6)/AUD(9) 等，
        // 帧内最后一个 slice 的 RTP marker=1（Chrome 按 marker 提交完整帧）
        QVector<int> sliceIdx;
        for (int k = 0; k < nalus.size(); ++k) {
            uint8_t t2 = nalus[k].type;
            if (t2 == 1 || t2 == 5) sliceIdx.append(k);
        }
        for (int k = 0; k < sliceIdx.size(); ++k) {
            if (sendNalu(nalus[sliceIdx[k]], YANG_Frametype_P, (k == sliceIdx.size() - 1) ? 1 : 0)) ++sent;
        }
    }
    // 每帧推进一次时间戳（按实际帧率），并保持 32 位回绕：
    // RTP 时间戳只有 32 位，若用 64 位累加不截断，交给 metaRTC 时高位被丢弃，
    // 跨回绕点会出现时间戳突变（播放端可能误判为乱序/丢帧）。
    rtpTimestamp_ = (rtpTimestamp_ + rtpTicksPerFrame_) & 0xFFFFFFFFULL;

    // [DIAG] WebRTC RTP 黑屏排查：确认关键帧已拆成 SPS/PPS/IDR 多 NALU 正确发出
    {
        static int64_t s_cnt = 0;
        if (++s_cnt % 30 == 1)
            qInfo() << "WebRtcSession sendFrame #" << s_cnt << " key=" << keyframe
                    << " sz=" << n << " nalus=" << nalus.size() << " sent=" << sent;
        // 关键帧必打详细日志：列出每个 NALU 类型，确认含 SPS(7)/PPS(8)/IDR(5)
        if (keyframe) {
            QString types;
            for (const Nalu& nu : nalus)
                types += QString::number(nu.type) + " ";
            qInfo() << "WebRtcSession KEYFRAME nalus=" << nalus.size()
                    << " types=[" << types.trimmed() << "] sent=" << sent;
        }
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

void WebRtcSession::onIceStateChange(int32_t uid, YangIceCandidateState iceState)
{
    qInfo() << "WebRtcSession: ICE state changed to" << static_cast<int>(iceState);
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

void WebRtcSession::onIceCandidate(int32_t uid, char* sdp)
{
    qInfo() << "WebRtcSession: local ICE candidate" << (sdp ? QString::fromUtf8(sdp).left(80) : "(null)");
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