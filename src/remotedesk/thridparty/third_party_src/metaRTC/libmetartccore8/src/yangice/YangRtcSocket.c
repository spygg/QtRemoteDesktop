//
// Copyright (c) 2019-2026 yanggaofeng
//

#include <yangice/YangRtcSocket.h>

#include <yangutil/sys/YangLog.h>
#include <yangutil/sys/YangEndian.h>
#include <yangutil/yangrtptype.h>
#include <yangrtc/YangRtcSession.h>

#include <errno.h>
#include <stdlib.h>

int32_t yang_rtcsock_sendData(YangRtcSocketSession *session, char *data, int32_t nb) {
	int32_t err = Yang_Ok;
	if (session == NULL || session->fd == -1 || session->notRemoteInit || data==NULL)
		return ERROR_RTC_SOCKET;

	yang_thread_mutex_lock(&session->sendLock);
	err=yang_socket_sendto(session->fd, data, nb, &session->remote_addr,0) > 0 ? Yang_Ok : ERROR_RTC_SOCKET;
	yang_thread_mutex_unlock(&session->sendLock);
	// [DIAG] RTPOUT dump（默认关闭）：设置环境变量 QRD_DIAG_RTPOUT=1 时逐包打印
	// RTP 头（pt/ssrc/seq/ts/len/M/dst）。诊断用，平时关闭避免控制台刷屏。
	static int s_rtpoutDiag = -1;
	if (s_rtpoutDiag < 0) {
		s_rtpoutDiag = getenv("QRD_DIAG_RTPOUT") != NULL ? 1 : 0;
	}
	if (s_rtpoutDiag && nb >= 12 && (data[0] & 0xC0) == 0x80) {
		char dbgip[64] = {0};
		yang_addr_getIPStr(&session->remote_addr, dbgip, 64);
		fprintf(stderr, "RTPOUT pt=%d ssrc=%u seq=%u ts=%u len=%d M=%d dst=%s:%d\n",
			(unsigned char)data[1] & 0x7F,
			((unsigned char)data[8]<<24)|((unsigned char)data[9]<<16)|((unsigned char)data[10]<<8)|(unsigned char)data[11],
			((unsigned char)data[2]<<8)|(unsigned char)data[3],
			((unsigned char)data[4]<<24)|((unsigned char)data[5]<<16)|((unsigned char)data[6]<<8)|(unsigned char)data[7],
			nb,
			((unsigned char)data[1] >> 7) & 1,
			dbgip, yang_addr_getPort(&session->remote_addr));
		fflush(stderr);
	}

	return err;
}
int32_t yang_rtcsock_sendData2(YangRtcSocketSession *session, YangIpAddress* remote_addr,char *data, int32_t nb) {
	int32_t err = Yang_Ok;

	if (session == NULL || session->fd == -1 || data==NULL)
		return ERROR_RTC_SOCKET;

	yang_thread_mutex_lock(&session->sendLock);
	err=yang_socket_sendto(session->fd, data, nb, remote_addr,0) > 0 ? Yang_Ok : ERROR_RTC_SOCKET;
	if (err != Yang_Ok) {
		char dbgip[64] = {0};
		yang_addr_getIPStr(remote_addr, dbgip, 64);
		yang_error("sendData2 sendto fail fd=%d ip=%s port=%d errno=%d", session->fd, dbgip, yang_addr_getPort(remote_addr), errno);
	}
	yang_thread_mutex_unlock(&session->sendLock);

	return err;
}

int32_t yang_rtcsock_tcp_sendData(YangRtcSocketSession *session,char *data, int32_t nb) {
	int32_t nbytes = 0;

	if (session == NULL || session->fd == -1  || session->notRemoteInit || data==NULL)
		return ERROR_RTC_SOCKET;

	yang_thread_mutex_lock(&session->sendLock);
	nbytes=yang_socket_send(session->fd, data, nb);
	yang_thread_mutex_unlock(&session->sendLock);
	if(nbytes==-1) yang_error("rtc tcp send error(%d)",nbytes);
	if(nbytes!=nb) yang_error("rtc tcp send data(%d),should send data(%d)",nbytes,nb);
	return nbytes==nb ? Yang_Ok : ERROR_RTC_SOCKET;
}

 int32_t yang_rtcsock_tcp_read(YangRtcSocketSession *session, char *buffer, int32_t bufferLen,int32_t* nbytes){
	 int32_t readLen=0;
	 while(yangtrue){
		 readLen = yang_socket_recv(session->fd, buffer, bufferLen, 0);

		 if (readLen > 0) {
			 *nbytes=readLen;
			 return Yang_Ok;
		 }else if (readLen == -1) {
			 int32_t sockerr = GetSockError();
			 if (sockerr == EINTR || sockerr == EWOULDBLOCK || sockerr == EAGAIN)	continue;

			 yang_error("%s, recv returned %d. GetSockError(): %d (%s)",
					 __FUNCTION__, readLen, sockerr, strerror(sockerr));

			 return ERROR_RTC_SOCKET;
		 }
	 }
	 return Yang_Ok;
 }

 int32_t yang_rtcsock_udp_read(YangRtcSocketSession *session, char *buffer, int32_t bufferLen,int32_t* nbytes){
	 int32_t readLen=0;
	 if ((readLen = yang_socket_recvfrom(session->fd, buffer, bufferLen, 0)) > 0) {
		 *nbytes=readLen;
		 return Yang_Ok;
	 		}else if (readLen == -1) {
	 			int32_t sockerr = GetSockError();
	 			if (sockerr == EINTR)
	 				return ERROR_RTC_SOCKET;
	 			if (sockerr == EWOULDBLOCK || sockerr == EAGAIN) {
	 				readLen = 0;
	 				return ERROR_RTC_SOCKET;
	 			}
	 			yang_error("%s, recv returned %d. GetSockError(): %d (%s)",
	 					__FUNCTION__, readLen, sockerr, strerror(sockerr));

	 			return ERROR_RTC_SOCKET;
	 		}
	 return Yang_Ok;
}

void* yang_run_rtcudp_thread(void *obj) {
    YangRtcSocketSession *session = (YangRtcSocketSession*) obj;
    YangRtcSession* rtcSession=(YangRtcSession*)session->rtcSession;
	int32_t nbytes = 0;
	char buffer[2048] = { 0 };
	//char ip[64]={0};
	YangIpAddress iceServer={0};
	YangIpAddress src;
	session->income_addr = &src;
	if (!session->binded&&yang_socket_listen(session->fd, &session->local_addr) < 0) {

		yang_error("Udp server bind error");
		return NULL;
	}

	session->isStart = yangtrue;
	session->isLoop = yangtrue;

	if (!session->notRemoteInit&&session->startStunTimer)
		session->startStunTimer(session->user);

	yang_trace("\nrtc udp(%s) server is starting,localPort=%d",session->familyType==Yang_IpFamilyType_IPV4?"IPV4":"IPV6", yang_addr_getPort(&session->local_addr));

	yang_memset(&src, 0, sizeof(YangIpAddress));
	src.familyType=session->familyType;

	while (session->isLoop && !session->stopRequested) {
		yang_memset(buffer, 0, 2048);
		if ((nbytes = yang_socket_recvfrom(session->fd, buffer, 2048,  &src)) > 0) {
			static int udpdbg = 0;
			if (udpdbg < 5) { char sip[64]={0}; yang_addr_getIPStr(&src, sip, 64); yang_error("UDP-RX from %s:%d len=%d t=%02x%02x", sip, yang_addr_getPort(&src), nbytes, (unsigned char)buffer[0], (unsigned char)buffer[1]); udpdbg++; }

			if(buffer[0]==0x01&&buffer[1]==0x01&&session->rtcSession){

				if(session->stunRequesting){
					yang_memset(&iceServer,0,sizeof(YangIpAddress));
					yang_addr_set(&iceServer,rtcSession->ice.session.server.serverIp,rtcSession->ice.session.server.serverPort,session->familyType,session->socketProtocol);
					if(yang_addr_cmp(&src,&iceServer)){
						rtcSession->ice.onStunResponse(&rtcSession->ice.session,buffer,nbytes);
						continue;
					}
				}else{
					if(rtcSession->ice.session.server.serverPort==yang_addr_getPort(&src))
							continue;
				}
			}

			if(session->notRemoteInit){
				if(!rtcSession->ice.isStunRtp(&rtcSession->ice.session,buffer, nbytes))
					continue;
				yang_addr_copy(&src,&session->remote_addr,session->familyType);
				session->notRemoteInit = yangfalse;
				if(session->updateAddress) session->updateAddress(session->user,&src);

			}

			if (session->receive)	session->receive(buffer, nbytes, session->user);
		}
	}

	// 线程退出后不再触碰 socket：关闭 fd 与置 fd=-1 统一由 yang_stop_rtcudp 在
	// join 之后完成。旧实现在这里关闭 fd 并把 fd 写回 -1，而 stop() 只轮询
	// isStart 并不 join，调用方（yang_destroy_rtcContext）紧接着就
	// yang_free(context->sock)；fd 位于 YangRtcSocketSession 偏移 0（也是
	// YangRtcSocket 首成员），这条 -1 写入落在已释放堆块的空闲链表链接字上，
	// 把 LFH 链表写成 0xffffffff，后续任意 malloc 在 ntdll 里按 [edi+4]
	// 访问回绕到地址 0x3 崩溃（0xC0000005 / exit 1067）。
	session->isStart = yangfalse;

	return NULL;
}


void* yang_run_rtctcp_thread(void *obj) {

    YangRtcSocketSession *session = (YangRtcSocketSession*) obj;
    int32_t readLen = 0;
    char buffer[4096] = { 0 };
	session->isStart = yangtrue;

	if (yang_socket_connect(session->fd, &session->remote_addr) == -1) {
		yang_socket_close(session->fd);
		// 连接失败：fd 已关闭，这里必须同时清 fd 与 isStart，否则 stop()/join
		// 会等一个永远不会结束的线程（旧实现里 isStart 一直是 true）。
		session->fd = -1;
		session->isStart = yangfalse;
		yang_error_wrap(ERROR_SOCKET, "rtc tcp connect socket error(%d)",GetSockError());
		return NULL;
	}

	if (!session->notRemoteInit&&session->startStunTimer)	session->startStunTimer(session->user);

	yang_trace("\n rtc tcp socket(%s) is starting",session->familyType==Yang_IpFamilyType_IPV4?"IPV4":"IPV6");
	session->isLoop = yangtrue;

	while (session->isLoop && !session->stopRequested) {
		yang_memset(buffer, 0, 4096);
        if ((readLen = yang_socket_recv(session->fd, buffer, 4096, 0)) > 0) {
        		if (session->receive)	session->receive(buffer, readLen, session->user);

		}else if (readLen == -1) {
			int32_t sockerr = GetSockError();
			if (sockerr == EINTR)
				continue;
			if (sockerr == EWOULDBLOCK || sockerr == EAGAIN) {
				readLen = 0;
				continue;
			}
			yang_error("%s, recv returned %d. GetSockError(): %d (%s)",
					__FUNCTION__, readLen, sockerr, strerror(sockerr));

			break;
		}
	}

	// 同 UDP：fd 的关闭与 fd=-1 统一交给 yang_stop_rtcudp（join 之后）。
	session->isStart = yangfalse;

	return NULL;
}

void* yang_run_rtctcp_thread_turn(void *obj) {

    YangRtcSocketSession *session = (YangRtcSocketSession*) obj;
	int32_t readLen = 0;
	int32_t msgLen=0;
	int32_t msgReadLen=0;
	int32_t msgReadBufferLen=0;
	yangbool recvData=yangfalse;
	int32_t headerLen=session->headerLen;
    char readBuffer[kRtpPacketSize] = { 0 };
    char* p=readBuffer;
    int32_t blank=0;

	session->isStart = yangtrue;

	if (yang_socket_connect(session->fd, &session->remote_addr) == -1) {
		yang_socket_close(session->fd);
		// 同 yang_run_rtctcp_thread：失败路径也要清 fd/isStart，stop() 才能 join 成功。
		session->fd = -1;
		session->isStart = yangfalse;
		yang_error_wrap(ERROR_SOCKET, "rtc tcp connect socket error(%d)",GetSockError());
		return NULL;
	}

	if (!session->notRemoteInit&&session->startStunTimer)	session->startStunTimer(session->user);

	yang_trace("\n rtc tcp socket(%s) is starting",session->familyType==Yang_IpFamilyType_IPV4?"IPV4":"IPV6");
	session->isLoop = yangtrue;


	while (session->isLoop && !session->stopRequested) {
		if(!recvData){
			if(msgLen==0)
				msgReadLen=headerLen;
			else
				msgReadLen=msgLen;
		}
		if ((readLen = yang_socket_recv(session->fd, p+msgReadBufferLen, msgReadLen, 0))> 0) {
			if(readLen!=msgReadLen){
				msgReadLen-=readLen;
				recvData=yangtrue;
				msgReadBufferLen+=readLen;
				continue;
			}
			msgReadBufferLen=0;
			recvData=yangfalse;
			if (msgLen == 0) {
				if(yang_get_be32((uint8_t*) readBuffer + 4) == 0x2112A442){
					msgLen = yang_get_be16((uint8_t*)readBuffer+2)+12;
					blank=0;
				}else{
					msgLen = (int32_t)yang_get_be16((uint8_t*)readBuffer+2);
					msgLen-=4;
					blank=4-msgLen%4;
					if(blank<4) msgLen+=blank;
				}


				if (msgLen > kRtpPacketSize) {
					yang_error("rtc tcp error pkt size=%d", msgLen);
					msgLen = 0;
					msgReadLen = 0;
					p=readBuffer;
					continue;
				}
				p=readBuffer+headerLen;
				continue;
			}

			if (session->receive)
				session->receive(readBuffer, msgLen+headerLen-blank, session->user);
			p=readBuffer;
			msgLen = 0;
		}else if (readLen == -1) {
			int32_t sockerr = GetSockError();
			if (sockerr == EINTR)
				continue;
			if (sockerr == EWOULDBLOCK || sockerr == EAGAIN) {
				readLen = 0;
				continue;
			}
			yang_error("%s, recv returned %d. GetSockError(): %d (%s)",
					__FUNCTION__, readLen, sockerr, strerror(sockerr));

			break;
		}
	}

	// 同 UDP：fd 的关闭与 fd=-1 统一交给 yang_stop_rtcudp（join 之后）。
	session->isStart = yangfalse;

	return NULL;
}

void yang_start_rtcudp(YangRtcSocketSession *sock) {
	if (sock == NULL || sock->isStart)
		return;

	// 每次启动都复位停止请求；否则上一个会话 stop 后本会话线程会立即退出。
	sock->stopRequested = yangfalse;

	if(sock->socketProtocol==Yang_Socket_Protocol_Udp){
		if (yang_thread_create(&sock->threadId, 0, yang_run_rtcudp_thread, sock)) {
			yang_error("YangThread::start could not start thread");
		}
	}else if(sock->socketProtocol==Yang_Socket_Protocol_Tcp){

		if (yang_thread_create(&sock->threadId, 0,yang_run_rtctcp_thread_turn, sock))

		{
				yang_error("YangThread::start could not start thread");
		}


	}
}


void yang_stop_rtcudp(YangRtcSocketSession *sock) {
	if (sock == NULL)	return;

	/*
	 * 拆除顺序必须是：先置停止请求 → join 等待线程真正退出 → 最后关 fd。
	 *
	 * 旧实现只轮询退出标志就返回，存在确定的 use-after-free 竞态：
	 *
	 *   接收线程：while (isLoop) { recvfrom ... }
	 *             session->isStart = yangfalse;   // ← stop 一看到这里就返回
	 *             yang_socket_close(session->fd); // ← 线程还在跑
	 *             session->fd = -1;               // ← 线程还在写
	 *   stop 线程：sock->isLoop = yangfalse;
	 *             while (sock->isStart) usleep(1ms);   // 不 join
	 *
	 * 调用链 yang_destroy_rtcConnection → yang_destroy_rtcContext 在 stop 返回后
	 * 立刻 yang_destroy_rtcsocket()（关 fd）→ yang_free(context->sock)。此时
	 * socket 块已经释放，接收线程最后那条 `fd = -1` 才执行：fd 位于
	 * YangRtcSocketSession 偏移 0（也是 YangRtcSocket 的首成员），-1 正好写进
	 * 已释放块的第一个字，也就是 LFH 空闲链表的链接字。
	 *
	 * 竞态条件：只要“线程发布 isStart=false”与“线程写 fd=-1”之间，主线程完成
	 * stop 轮询、关 fd、释放 socket 块，链表就被写成 0xffffffff；之后任意一次
	 * malloc 从该 size 桶取块，ntdll 用该值当节点指针，`mov edi,[edi+4]` 在
	 * edi=0xffffffff 时 32 位回绕读到地址 0x3 → 0xC0000005（服务退出码 1067）。
	 * 这与现场 dmp（故障点 ntdll LFH、ecx/edi=0xffffffff、ExceptionInformation[1]=0x3）
	 * 逐项吻合。旧实现还有第二处竞态：线程 closesocket(fd) 与 destroy 里的关 fd
	 * 构成 double-close，fd 值可能已被复用，误关其它句柄。
	 *
	 * stopRequested 必须最先置位：否则“线程刚 create 还没跑起来”时 stop 只把
	 * isLoop 置 false，线程入口随后又把它置回 true，join 会永久阻塞。
	 * 本函数会被调用两次（yang_destroy_rtcContext + yang_destroy_rtcsocket），
	 * threadId 为空时是无副作用的空操作。
	 */
	sock->stopRequested = yangtrue;
	sock->isLoop = yangfalse;

	if (sock->threadId) {
		yang_thread_join(sock->threadId, NULL);
		sock->threadId = NULL;
		sock->isStart = yangfalse;
	}

	if (sock->fd > 0) {
		yang_socket_close(sock->fd);
		sock->fd = -1;
	}
}

void yang_rtcsock_update_remoteAddr(YangRtcSocketSession *sock,char* remoteIp,int32_t port){

	yang_trace("\nremoteIp=%s,port=%d",remoteIp,port);
	yang_addr_set(&sock->remote_addr,remoteIp,port,sock->familyType,sock->socketProtocol);

	sock->notRemoteInit=(port==0?yangtrue:yangfalse);

}

int32_t yang_rtcsock_connect(YangRtcSocketSession* sock){
	return yang_socket_connect(sock->fd, &sock->remote_addr);
}

int32_t yang_rtcsock_listen(YangRtcSocketSession* sock){
	if(sock==NULL)
		return ERROR_RTC_SOCKET;

	if (yang_socket_listen(sock->fd, &sock->local_addr) < 0) {
		char ip[64]={0};
		yang_addr_getIPStr(&sock->local_addr,ip,sizeof(ip));
		yang_error("Udp server (%s:%hu) bind error",ip,yang_addr_getPort(&sock->local_addr));
		return ERROR_RTC_SOCKET;
	}
	sock->binded=yangtrue;
	return Yang_Ok;
}

int32_t yang_create_rtcsocket(YangRtcSocket *sock,YangIpFamilyType familyType, YangSocketProtocol protocol,int32_t plocalPort) {
	YangRtcSocketSession* session = NULL;
	if (sock == NULL)
		return ERROR_RTC_SOCKET;
	session=&sock->session;

	session->fd = -1;
	session->socketProtocol=protocol;
	session->familyType=familyType;

	session->headerLen=0;

	session->binded=yangfalse;
	session->income_addr = NULL;
	yang_addr_setAnyAddr(&session->local_addr,plocalPort,familyType,protocol);

	session->fd = yang_socket_create(familyType,protocol);
	if(session->fd==-1){
		return yang_error_wrap(ERROR_RTC_SOCKET,"create rtc socket fail,familtype=%s,protocol=%s",
				familyType==Yang_IpFamilyType_IPV4?"IPV4":"IPV6",protocol==Yang_Socket_Protocol_Udp?"UDP":"TCP");
	}

	session->notRemoteInit=yangtrue;

	yang_thread_mutex_init(&session->sendLock,NULL);


	if(protocol == Yang_Socket_Protocol_Udp){
		sock->write=yang_rtcsock_sendData;
		sock->write2 = yang_rtcsock_sendData2;
		sock->read=yang_rtcsock_udp_read;
	}else{
		sock->write=yang_rtcsock_tcp_sendData;
		sock->read=yang_rtcsock_tcp_read;
	}
	sock->start=yang_start_rtcudp;
	sock->stop=yang_stop_rtcudp;
	sock->updateRemoteAddress=yang_rtcsock_update_remoteAddr;
	sock->connect=yang_rtcsock_connect;
	sock->listen=yang_rtcsock_listen;


	return Yang_Ok;

}

void yang_destroy_rtcsocket(YangRtcSocket *sock) {
    if (sock == NULL)
    	return;

    yang_stop_rtcudp(&sock->session);
    if (sock->session.fd > 0) {
    	yang_socket_close(sock->session.fd);
        sock->session.fd = -1;
    }
    yang_free(sock->session.buffer);
    yang_thread_mutex_destroy(&sock->session.sendLock);

}
