#include "webrtcsession.h"

#ifdef USE_WEBRTC

#include <yangrtc/YangPushData.h>

#include <QDebug>
#include <cstring>
#include <mutex>

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
    Q_UNUSED(fps)  // 时间戳改为单调微秒时钟后不再依赖标称帧率

    // RTP 时间戳基准：单调微秒时钟（见 webrtcsession.h 成员注释）。
    // 只用 delta，起点无意义；90kHz/fps 的帧步进计数器已废弃（单位错误，
    // 会被 metaRTC 按 µs 解释导致 RTP 时钟失真）。
    rtpClock_.start();

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

    // RFC 3550 的"时间戳随机起点"由 metaRTC 侧保证：YangTimestamp 以首帧 pts
    // 为基准做差（base），线上 RTP ts = delta*9/100，与本地时钟绝对值无关。
    // （旧的 rtpTimestamp_ 随机计数器已废弃，单位错误见 webrtcsession.h 注释。）
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

    // [PERF] 逐帧诊断日志已移除：旧实现每 30 帧打一条 sendFrame、每个关键帧打一条
    // KEYFRAME（实测一天 5.8 万条 KEYFRAME + 3.2 万条 sendFrame，占日志 85%），
    // 在 sendFrame 热路径上做字符串拼接+落盘，属自伤。
    Q_UNUSED(keyframe);
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

    // 帧内所有 NALU（SPS/PPS/IDR slices）共享同一 RTP 时间戳，由 metaRTC 的
    // YangTimestamp 以 (pts-base)*9/100 换算成 90kHz ticks——pts 单位必须是微秒。
    //
    // [ROOT-CAUSE 2026-10-10] 这里**必须用 elapsed()**，绝不能用 msecsSinceReference()。
    // QElapsedTimer 只在 start()/restart() 时把"当前绝对时刻"快照进 t1：
    //   - msecsSinceReference() 返回那个**快照**，start() 后永不变化 → ptsUs 恒定 →
    //     metaRTC 的 yang_setVideoData() 因 `ts <= preTimestamp` 提前 return，
    //     curVideotimestamp 永远停在 0 → **每帧 RTP 时间戳都是 0**；
    //   - Chrome 的 VideoFrameCompositor 视"与当前帧时间戳相同"的帧为重复帧直接丢弃
    //     → 远端画面冻住，只在时间戳偶发被改写时才"补"一批（表现为每 6~10s 跳一次，
    //     端到端延迟在 0.1s~5.9s 之间锯齿；MSE 路径不受影响，因为其时间戳是前端自建的）。
    //   - elapsed() 返回"自 start() 以来流逝的毫秒数"，才是这里想要的单调递增时钟。
    // 1ms 量化对本场景（帧距 55~66ms）足够。加 1 防止首帧恰为 0
    //（YangTimestamp 以 preTimestamp==0 判基准帧）。
    const qint64 ptsUs = qMax<qint64>(1, rtpClock_.elapsed() * 1000);

    // 单 NALU 发送 helper（frametype 显式指定；marker=1 表示帧内最后一个 NALU）
    auto sendNalu = [&](const Nalu& nu, int frametype, int marker) -> bool {
        YangFrame frame;
        std::memset(&frame, 0, sizeof(frame));
        frame.mediaType = YangFrameTypeVideo;
        frame.frametype = frametype;
        frame.nb = nu.len;
        frame.pts = ptsUs;
        frame.dts = ptsUs;
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
    // 时间戳不再按帧步进：pts 取自单调微秒时钟（本函数开头），RTP 时钟与
    // 真实时间一致；32 位回绕由 metaRTC 的 (pts-base)*9/100 换算自然处理。
    Q_UNUSED(sent);
}

void WebRtcSession::close()
{
    // 防重入：pc_->close() 可能同步回调 failed()/closed() → 上层
    // stopWebRtcSession → 再次 close()。若不设标志，内层会把 pc_ reset 掉，
    // 外层栈帧继续使用已释放对象（UAF）。
    if (closing_)
        return;
    closing_ = true;

    connected_ = false;
    remoteSet_ = false;
    // 先把 unique_ptr move 到局部变量再 close：这样 close() 期间发生的重入
    // 看到的是 pc_ == nullptr（走上面的重入分支或空指针保护），不会碰到正被销毁的对象；
    // 真正的析构推迟到本函数返回时，仍在外层栈帧之外完成。
    std::unique_ptr<YangPeerConnection8> pc = std::move(pc_);
    if (pc)
        pc->close();
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