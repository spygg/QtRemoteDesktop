//
// Copyright (c) 2019-2026 yanggaofeng
//
#include <yangutil/sys/YangSRtp.h>
#include <yangutil/sys/YangLog.h>


#if Yang_Enable_Dtls
#if Yang_OS_WIN
#define bzero(a, b) yang_memset(a, 0, b)
#endif

int32_t yang_destroy_srtp(YangSRtp* srtp){
    if (srtp->recvCtx) {
        srtp_dealloc(srtp->recvCtx);
    }

    if (srtp->sendCtx) {
        srtp_dealloc(srtp->sendCtx);
    }
    yang_thread_mutex_destroy(&srtp->rtpLock);
    yang_thread_mutex_destroy(&srtp->rtcpLock);
	return Yang_Ok;
}
int32_t yang_create_srtp(YangSRtp* srtp,char* recv_key,int precvkeylen, char* send_key,int psendkeylen)
{
    int32_t err = Yang_Ok;
    uint8_t *skey;
    uint8_t *rkey ;
    srtp_policy_t policy;
    srtp_err_status_t r0 = srtp_err_status_ok;

    yang_memset(&policy, 0,sizeof(policy));
    // TODO: Maybe we can use SRTP-GCM in future.
    // @see https://bugs.chromium.org/p/chromium/issues/detail?id=713701
    // @see https://groups.google.com/forum/#!topic/discuss-webrtc/PvCbWSetVAQ
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);

    policy.ssrc.value = 0;
    // TODO: adjust window_size
    policy.window_size = 8192;
    policy.allow_repeat_tx = 1;
    policy.next = NULL;


    //init send context
    policy.ssrc.type = ssrc_any_outbound;
    skey = (uint8_t *)yang_calloc(psendkeylen,1);
    yang_memcpy(skey, send_key, psendkeylen);
    policy.key = skey;

    if ((r0 = srtp_create(&srtp->sendCtx, &policy)) != srtp_err_status_ok) {
        return yang_error_wrap(ERROR_RTC_SRTP_INIT, "srtp create send r0=%u", r0);
    }

    // init recv context
    policy.ssrc.type = ssrc_any_inbound;
    rkey = (uint8_t *)yang_calloc(precvkeylen,1);
    yang_memcpy(rkey, recv_key, precvkeylen);
    policy.key = rkey;


    if ((r0 = srtp_create(&srtp->recvCtx, &policy)) != srtp_err_status_ok) {
        return yang_error_wrap(ERROR_RTC_SRTP_INIT, "srtp create recv r0=%u", r0);
    }

    yang_free(skey);
    yang_free(rkey);
    yang_thread_mutex_init(&srtp->rtpLock,NULL);
    yang_thread_mutex_init(&srtp->rtcpLock,NULL);
    return err;
}

int32_t yang_enc_rtp(YangSRtp* srtp,void* packet, int* nb_cipher)
{
    int32_t err = Yang_Ok;
    srtp_err_status_t r0 = srtp_err_status_ok;

    // If DTLS/SRTP is not ready, fail.
    if (!srtp->sendCtx) {
        return yang_error_wrap(ERROR_RTC_SRTP_PROTECT, "srtp not init");
    }

    // [DIAG] plaintext RTP mode (QTRD_NO_SRTP=1): 跳过 srtp_protect，用于判定 Chrome demux vs SRTP
    {
        static int s_noSrtp = -1;
        if (s_noSrtp < 0) {
            s_noSrtp = (getenv("QTRD_NO_SRTP") != NULL) ? 1 : 0;
            if (s_noSrtp) fprintf(stderr, "SRTP-DISABLED plaintext RTP mode\n");
        }
        if (s_noSrtp) return Yang_Ok;
    }

    yang_thread_mutex_lock(&srtp->rtpLock);
    {
        int in_len = *nb_cipher;
        // [DIAG] WebRTC 黑屏排查：srtp_protect 前 dump 明文 RTP 载荷（每进程前 12 包）
        {
            static int s_rtpdump = 0;
            if (*nb_cipher >= 12 && s_rtpdump < 12 && (((unsigned char*)packet)[0] & 0x80)) {
                s_rtpdump++;
                unsigned char* rp = (unsigned char*)packet;
                fprintf(stderr, "RTPRAW len=%d M=%d pt=%d seq=%u ts=%u ssrc=%u nal=%d | %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                    *nb_cipher, (rp[1] >> 7) & 1, rp[1] & 0x7F,
                    ((unsigned)rp[2]<<8)|(unsigned)rp[3],
                    ((unsigned)rp[4]<<24)|((unsigned)rp[5]<<16)|((unsigned)rp[6]<<8)|(unsigned)rp[7],
                    ((unsigned)rp[8]<<24)|((unsigned)rp[9]<<16)|((unsigned)rp[10]<<8)|(unsigned)rp[11],
                    (*nb_cipher >= 13) ? (rp[12] & 0x1F) : -1,
                    rp[12], rp[13], rp[14], rp[15], rp[16], rp[17], rp[18], rp[19], rp[20], rp[21], rp[22], rp[23]);
                fflush(stderr);
            }
        }
        if ((r0 = srtp_protect(srtp->sendCtx, packet, nb_cipher)) != srtp_err_status_ok) {
            unsigned char* rp=(unsigned char*)packet;
            fprintf(stderr, "SRTP-PROTECT-FAIL r0=%d in=%d nb=%d v=%02x pt=%02x seq=%u ts=%u ssrc=%u cc=%u x=%u p=%u\n",
                r0, in_len, *nb_cipher, rp[0], rp[1],
                ((unsigned)rp[2]<<8)|(unsigned)rp[3],
                ((unsigned)rp[4]<<24)|((unsigned)rp[5]<<16)|((unsigned)rp[6]<<8)|(unsigned)rp[7],
                ((unsigned)rp[8]<<24)|((unsigned)rp[9]<<16)|((unsigned)rp[10]<<8)|(unsigned)rp[11],
                rp[0]&0x0F, (rp[0]>>4)&1, (rp[1]>>5)&1);
            fflush(stderr);
            return yang_error_wrap(ERROR_RTC_SRTP_PROTECT, "rtp protect r0=%u", r0);
        }
    }
    yang_thread_mutex_unlock(&srtp->rtpLock);
    return err;
}

int32_t yang_enc_rtcp(YangSRtp* srtp,void* packet, int* nb_cipher)
{
    int32_t err = Yang_Ok;
    srtp_err_status_t r0 = srtp_err_status_ok;

    // If DTLS/SRTP is not ready, fail.
    if (!srtp->sendCtx) {
        return yang_error_wrap(ERROR_RTC_SRTP_PROTECT, "not ready");
    }


    yang_thread_mutex_lock(&srtp->rtcpLock);
    if ((r0 = srtp_protect_rtcp(srtp->sendCtx, packet, nb_cipher)) != srtp_err_status_ok) {
        return yang_error_wrap(ERROR_RTC_SRTP_PROTECT, "rtcp protect r0=%u", r0);
    }
    yang_thread_mutex_unlock(&srtp->rtcpLock);
    return err;
}

int32_t yang_dec_rtp(YangSRtp* srtp,void* packet, int* nb_plaintext)
{
	srtp_err_status_t r0 = srtp_err_status_ok;

    // If DTLS/SRTP is not ready, fail.
    if (!srtp->recvCtx) {
        return yang_error_wrap(ERROR_RTC_SRTP_UNPROTECT, "not ready");
    }


    if ((r0 = srtp_unprotect(srtp->recvCtx, packet, nb_plaintext)) != srtp_err_status_ok) {
        if(r0==srtp_err_status_replay_fail) return r0;
        return yang_error_wrap(ERROR_RTC_SRTP_UNPROTECT, "rtp unprotect r0=%u", r0);
    }

    return Yang_Ok;
}

int32_t yang_dec_rtcp(YangSRtp* srtp,void* packet, int* nb_plaintext)
{
	srtp_err_status_t r0 = srtp_err_status_ok;

    // If DTLS/SRTP is not ready, fail.
    if (!srtp->recvCtx) {
        return yang_error_wrap(ERROR_RTC_SRTP_UNPROTECT, "not ready");
    }


    if ((r0 = srtp_unprotect_rtcp(srtp->recvCtx, packet, nb_plaintext)) != srtp_err_status_ok) {
    	if (r0 == srtp_err_status_replay_fail)	return r0;
    	return yang_error_wrap(ERROR_RTC_SRTP_UNPROTECT, "rtcp unprotect r0=%u", r0);
    }

    return Yang_Ok;
}
#endif
