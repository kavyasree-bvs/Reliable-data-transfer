#include "stdafx.h"
#include "SenderSocket.h"

UINT StatsRun(LPVOID pParam);
UINT WorkerRun(LPVOID pParam);
#define PRINT 0
#define STATS 1
#define DBUG 0
bool workerDone = false;
float mod(float a)
{
	if (a < 0)
		return -a;
	return a;
};

UINT StatsRun(LPVOID pParam)
{
	static long prev_base_no;
	Parameters *p = (Parameters*)pParam; // shared parameters
	static int i = 0;
	long prev_c = 0;
	long prev_bytes = 0;
	while (WaitForSingleObject(p->eventQuit, 2000) == WAIT_TIMEOUT)// ||  WaitForSingleObject(p->eventQuit, 2000) == WAIT_OBJECT_0)
	{
		//if (workerDone == true)
			//return 0;
		i = i + 2;
		// print
		WaitForSingleObject(p->mutex, INFINITE);
		p->S = ((p->B - prev_base_no) * 8 * (MAX_PKT_SIZE - sizeof(SenderDataHeader)) / 2000000.0);
		printf("[%3d] B %8d ( %.1f MB) N %8d T %2d F %2d W %5d S %.3f Mbps RTT %.3f\n",
			i,
			p->B,
			p->MB,
			p->N,
			p->T,
			p->F,
			p->W,
			p->S,
			p->RTT);
		prev_base_no = p->B;
#if 0
		if (p->noOfActiveThreads == 0)
		{
			ReleaseMutex(p->mutex);
			break;
		}
#endif
		ReleaseMutex(p->mutex);
	}
	return 0;
}

SenderSocket::SenderSocket()
{
	//printf("size of dqord %d\n", sizeof(DWORD));
	constr_start_time = clock();
	
	//Initialize WinSock; once per program run
	wVersionRequested = MAKEWORD(2, 2);
	if (WSAStartup(wVersionRequested, &wsaData) != 0) {
		printf("WSAStartup error %d\n", WSAGetLastError());
		exit(0);
	}

	initialRTO = 1.0;
	openCalled = false;
	closeCalled = false;

#if STATS
	stats_handle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)StatsRun, &p, 0, NULL);		// start threadA (instance #1) 
	if (stats_handle == NULL)
	{
		printf("stats CreateThread error: %d\n", GetLastError());
		return;
	}
	//printf("stats thread created\n");
#endif
	InitThreadParams();
}

SenderSocket::~SenderSocket()
{
	//do not forget to close handles
#if STATS
	WaitForSingleObject(stats_handle, INFINITE);
	CloseHandle(stats_handle);

#endif
	WaitForSingleObject(worker_handle, INFINITE);
	CloseHandle(worker_handle);
	WSACleanup();

	delete pending_pkts;
	delete pending_pkts_len;
	delete attempts;
}

void SenderSocket::CreateThreadHandles(void)
{
	//count of empty buffers
	empty = CreateSemaphore(NULL, 0, W, NULL);
	if (empty == NULL)
	{
		printf("empty CreateSemaphore error: %d\n", GetLastError());
		return;
	}
	//count of full buffers
	full = CreateSemaphore(NULL, 0, W, NULL);
	if (full == NULL)
	{
		printf("p.full CreateSemaphore error: %d\n", GetLastError());
		return;
	}
	socketReceiveReady = WSACreateEvent();
	if (socketReceiveReady == NULL)
	{
		printf("socketReceiveReady CreateEvent error: %d\n", GetLastError());
		return;
	}

	// thread handles are stored here; they can be used to check status of threads, or kill them
	// start stats thread

	worker_handle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)WorkerRun, this, 0, NULL);		// start threadA (instance #1) 
	if (worker_handle == NULL)
	{
		printf("worker CreateThread error: %d\n", GetLastError());
		return;
	}
	//printf("worker thread created\n");
}

void SenderSocket::InitThreadParams(void)
{
#if 1
	// initialize shared data structures & parameters sent to threads
	// create a mutex for accessing critical sections (including printf); initial state = not locked
	p.mutex = CreateMutex(NULL, 0, NULL);
	if (p.mutex == NULL)
	{
		printf("CreateMutex error: %d\n", GetLastError());
		return;
	}

	// create a semaphore that counts the number of active threads; initial value = 0, max = 2
	//p.finished = CreateSemaphore(NULL, 0, threads, NULL);
	// create a quit event; manual reset, initial state = not signaled
	p.eventQuit = CreateEvent(NULL, true, false, NULL);
	if (p.eventQuit == NULL)
	{
		printf("eventQuit CreateEvent error: %d\n", GetLastError());
		return;
	}
#endif
}

int SenderSocket::Open(char * targetHost, int receiverPort, int senderWindow, LinkProperties* linkProp)
{	
	if (openCalled == true && closeCalled == false)
		return ALREADY_CONNECTED;

	openCalled = true;

	WaitForSingleObject(p.mutex, INFINITE);					// lock mutex
	p.W = senderWindow;
	ReleaseMutex(p.mutex);									// release critical section

	W = senderWindow;
	pending_pkts = new Packet[W];
	pending_pkts_len = new int[W];
	attempts = new int[W];
	dataLastSentTime= new clock_t[W];
	dataAckedTime= new clock_t[W];
	dupAckCount = new int[W];

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	//handle errors
	if (sock == INVALID_SOCKET)
	{
		printf("socket() generated error %d\n", WSAGetLastError());
		return FAIL;
	}

	struct sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = INADDR_ANY;
	local.sin_port = htons(0);
	if (bind(sock, (struct sockaddr*)&local, sizeof(local)) == SOCKET_ERROR)
	{
		printf("socket() binding error %d\n", WSAGetLastError());
		return FAIL;
	}

	// structure used in DNS lookups
	struct hostent *remote;
	
	server.sin_family = AF_INET;
	server.sin_port = htons(receiverPort);
	DWORD IP_add = inet_addr(targetHost);
	if (IP_add == INADDR_NONE)
	{
		// if not a valid IP, then do a DNS lookup
		//DNS lookup performed through a system call
		if ((remote = gethostbyname(targetHost)) == NULL)
		{
			//printf("Invalid string: neither FQDN, nor IP address\n");
			//printf("failed with %d\n", WSAGetLastError());
			clock_t temp = clock();
			double diff_temp = ((double)(temp - constr_start_time)) / CLOCKS_PER_SEC;//in s
			printf("[ %.3f]\t--> target %s is invalid\n", diff_temp, targetHost);
			return INVALID_NAME;
		}
		else // take the first IP address and copy into sin_addr
			memcpy((char *)&(server.sin_addr), remote->h_addr, remote->h_length);
	}
	else
	{
		// if a valid IP, directly drop its binary version into sin_addr
		server.sin_addr.S_un.S_addr = IP_add;
	}
	
	clock_t start, end;
	double cpu_time_used;
	char * res;
	linkProps = *linkProp;
	linkProps.bufferSize = senderWindow;

	SenderSynHeader ssh;
	ssh.lp = linkProps;
	ssh.sdh.flags.SYN = 1;ssh.sdh.seq = 0;
	//RTO of SYN pkts 
	initialRTO = max(1,2*ssh.lp.RTT);
	//printf("syn init rto %f\n", initialRTO);
	int count = 1;
	while (count <= MAX_SYN_ATTEMPTS)
	{
		clock_t last_syn = clock();
		cpu_time_used = ((double)(last_syn - constr_start_time)) / CLOCKS_PER_SEC;//in s

#if PRINT
		printf("[ %.3f]\t--> SYN %d (attempt %d of %d, RTO %.3f) to %s\n", cpu_time_used, ssh.sdh.seq, count, MAX_SYN_ATTEMPTS, initialRTO, inet_ntoa(server.sin_addr));
#endif
		if (sendto(sock, (char*)&ssh, sizeof(SenderSynHeader), 0, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR)
		{
			clock_t temp = clock();
			cpu_time_used = ((double)(temp - constr_start_time)) / CLOCKS_PER_SEC;//in s
			printf("[ %.3f]\t--> failed sendto with %d\n", cpu_time_used, WSAGetLastError());
			//exit(0);
			return FAILED_SEND;
		}

		ReceiverHeader rh;
		struct sockaddr_in response;
		size = sizeof(response);

		fd_set fd;
		FD_ZERO(&fd);
		FD_SET(sock, &fd);
		timeval tp;
		if (initialRTO<1.0)
		{
			tp.tv_sec = 0;
			tp.tv_usec = (long)(initialRTO * 1000 * 1000);
			//printf("syn %ld %ld \n", 0, (long)(initialRTO * 1000 * 1000));
		}
		else
		{
			tp.tv_sec = (long)initialRTO;
			tp.tv_usec = (initialRTO - (long)initialRTO) * 1000 * 1000;
			//printf("syn %ld %ld \n", (long)initialRTO, (initialRTO - (long)initialRTO) * 1000 * 1000);
		}
		
		int available = select(0, &fd, NULL, NULL, &tp);
		//int available = select(0, NULL,&fd, NULL, &tp);
		int iResult = 0;
		if (available > 0)
		{
			iResult = recvfrom(sock, (char*)&rh, sizeof(ReceiverHeader), 0, (struct sockaddr*)&response, &size);
			if (iResult == SOCKET_ERROR)
			{
				//error processing
				clock_t temp = clock();
				cpu_time_used = ((double)(temp - constr_start_time)) / CLOCKS_PER_SEC;//in s
				printf("[ %.3f]\t<-- failed recvfrom with %d\n", cpu_time_used, WSAGetLastError());
				return FAILED_RECV;
			}
			clock_t syn_ack = clock();
			cpu_time_used = ((double)(syn_ack - constr_start_time)) / CLOCKS_PER_SEC;//in s
			initialRTO = 3 * ((double)(syn_ack - last_syn)) / CLOCKS_PER_SEC;
#if PRINT
			printf("[ %.3f]\t<-- SYN-ACK %d window %d; setting initial RTO to %.3f\n", cpu_time_used, rh.ackSeq, rh.recvWnd, initialRTO);
#endif
			WaitForSingleObject(p.mutex, INFINITE);					// lock mutex
			//printf("SYN sender base b4 updating is %d rcved ack no is %d\n", senderBase, rh.ackSeq);
			double sampledRTT = (double)(syn_ack - last_syn) / CLOCKS_PER_SEC;
			p.RTT = (1 - ALPHA)*p.RTT + ALPHA * sampledRTT;
			p.devRTT = (1 - BETA)*p.devRTT + BETA * mod(p.RTT - sampledRTT);
			ReleaseMutex(p.mutex);									// release critical section
			//printf("after synack setting rto to: %f\n", p.RTT);
			senderBase++;

			WaitForSingleObject(p.mutex, INFINITE);
			p.B = senderBase;
			ReleaseMutex(p.mutex);
			//printf("after updating is %d\n", senderBase);

			CreateThreadHandles();
			//SetEvent(Ready);

			//Flow control
			
			lastReleased = min(W, rh.recvWnd);
			//printf("lastReleased %d\n", lastReleased);
			bool ret = ReleaseSemaphore(empty, lastReleased, NULL);
			//printf("\n\nafter syn ack empty ReleaseSemaphore ret value is %d\n\n", ret);
			return STATUS_OK;
		}
		else if (available == 0)
		{
			count++;
		}
		else if (available < 0)
		{
			printf("failed with %d on recv\n", WSAGetLastError());
			return FAIL;
		}
	}
	return TIMEOUT;
}

int SenderSocket::Close(float *estimatedRTT)
{
	sendDone = true;
#if DBUG
	printf("\t\tCLsoe -1 setting event quit\n");
#endif
	WaitForSingleObject(p.mutex, INFINITE);
	SetEvent(p.eventQuit);
	ReleaseMutex(p.mutex);
	
#if DBUG
	printf("CLsoe 0\n");
#endif
	
	WaitForSingleObject(worker_handle, INFINITE);
	CloseHandle(worker_handle);
	
#if STATS
	WaitForSingleObject(stats_handle, INFINITE);
	CloseHandle(stats_handle);
#endif
	
	
	if (openCalled == false)
		return NOT_CONNECTED;
	closeCalled = true;

	SenderSynHeader ssh;
	ssh.lp = linkProps;
	ssh.sdh.flags.FIN = 1;
	//WaitForSingleObject(p.mutex, INFINITE);					// lock mutex
	//ssh.sdh.seq = p.N;
	//ReleaseMutex(p.mutex);									// release critical section
	ssh.sdh.seq = nextSeq;
	WaitForSingleObject(p.mutex, INFINITE);					// lock mutex
	*estimatedRTT = p.RTT;
	ReleaseMutex(p.mutex);									// release critical section
	
	int count = 1;
	
	while (count <= MAX_ATTEMPTS)
	{
		double cpu_time_used;
		clock_t last_fin = clock();
		cpu_time_used = ((double)(last_fin - constr_start_time)) / CLOCKS_PER_SEC;//in s
#if PRINT
		printf("[ %.3f]\t--> FIN %d (attempt %d of %d, RTO %.3f)\n", cpu_time_used, ssh.sdh.seq, count, MAX_ATTEMPTS, initialRTO);
#endif
		if (sendto(sock, (char*)&ssh, sizeof(SenderSynHeader), 0, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR)
		{
			clock_t temp = clock();
			cpu_time_used = ((double)(temp - constr_start_time)) / CLOCKS_PER_SEC;//in s
			printf("[ %.3f]\t--> FIN %d (attempt %d of %d, RTO %.3f)\n", cpu_time_used, ssh.sdh.seq, count, MAX_ATTEMPTS, initialRTO);
			printf("[ %.3f]\t--> failed sendto with %d\n", cpu_time_used, WSAGetLastError());
			return FAILED_SEND;
		}

		
		ReceiverHeader rh;
		struct sockaddr_in response;
		int size = sizeof(response);

		fd_set fd;
		FD_ZERO(&fd);
		FD_SET(sock, &fd);
		timeval tp;
		if (initialRTO<1.0)
		{
			tp.tv_sec = 0;
			tp.tv_usec = (long)(initialRTO * 1000 * 1000);
			//printf("FIN %ld %ld \n", 0, (long)(initialRTO * 1000 * 1000));
		}
		else
		{
			tp.tv_sec = (long)initialRTO;
			tp.tv_usec = (initialRTO - (long)initialRTO) * 1000 * 1000;
			//printf("FIN %ld %ld \n", (long)initialRTO, (initialRTO - (long)initialRTO) * 1000 * 1000);
		}
		int available = select(0, &fd, NULL, NULL, &tp);
		int iResult = 0;
		if (available > 0)
		{
			iResult = recvfrom(sock, (char*)&rh, sizeof(ReceiverHeader), 0, (struct sockaddr*)&response, &size);
			if (iResult == SOCKET_ERROR)
			{
				//error processing
				clock_t temp = clock();
				cpu_time_used = ((double)(temp - constr_start_time)) / CLOCKS_PER_SEC;//in s
				printf("[ %.3f]\t<-- failed recvfrom with %d\n", cpu_time_used, WSAGetLastError());
				return FAILED_RECV;
			}
			clock_t fin_ack = clock();
			cpu_time_used = ((double)(fin_ack - constr_start_time)) / CLOCKS_PER_SEC;//in s
			printf("[ %.3f]\t<-- FIN-ACK %d window %x\n", cpu_time_used, rh.ackSeq, rh.recvWnd);
			return STATUS_OK;
			
		}
		else if (available == 0)
		{
			count++;
		}
		else if (available < 0)
		{
			printf("failed with %d on recv\n", WSAGetLastError());
			return FAIL;
		}
	}
	return TIMEOUT;
}

int SenderSocket::Send(char * buf, int pkt_size)
{
#if DBUG
	printf("Send 1\n");
#endif
	if (openCalled == false)
		return NOT_CONNECTED;

	//HANDLE arr[] = { eventQuit, empty };
	
#if DBUG
	printf("Send 2\n");
#endif
	DWORD ret = WaitForSingleObject(empty,INFINITE);
#if DBUG
	printf("Send 3 ret from Send %x\n", ret);
#endif
	//DWORD ret = WaitForMultipleObjects(2, arr, false, INFINITE);
	// no need for mutex as no shared variables are modified
	Packet packet;
	int slot = nextSeq % W;
	packet.sdh.seq = nextSeq;
	memcpy(packet.data, buf, pkt_size);

	pending_pkts[slot] = packet;
	pending_pkts_len[slot] = pkt_size;
	attempts[slot] = 0;
	dupAckCount[slot] = 0;

	nextSeq++;
	bool b = ReleaseSemaphore(full, 1, NULL);
#if DBUG
	printf("Send 4 full ReleaseSemaphore bool b %d W %d slot %d nextseq %d\n", b,W, slot, nextSeq);
#endif
	return 0;
}



UINT WorkerRun(LPVOID pParam)
{
	SenderSocket *ssObj = (SenderSocket*)pParam;
	int nextToSend = ssObj->senderBase;
	
	int kernelBuffer = 20e6; // 20 meg
	if (setsockopt(ssObj->sock, SOL_SOCKET, SO_RCVBUF, (char *)&kernelBuffer, sizeof(int)) == SOCKET_ERROR)
		printf("failed setsockopt with %d\n", WSAGetLastError());
	kernelBuffer = 20e6; // 20 meg
	if (setsockopt(ssObj->sock, SOL_SOCKET, SO_SNDBUF, (char *)&kernelBuffer, sizeof(int)) == SOCKET_ERROR)
		printf("failed setsockopt with %d\n", WSAGetLastError());
	/**/
	WaitForSingleObject(ssObj->p.mutex, INFINITE);					// lock mutex
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	ReleaseMutex(ssObj->p.mutex);
	
//	timeout = 1000 * (ssObj->p.RTT + 4 * max(ssObj->p.devRTT, 0.010));

	if (WSAEventSelect(ssObj->sock, ssObj->socketReceiveReady, FD_READ) == SOCKET_ERROR)
	{
		printf("WSAEventSelect() failed with error %d\n", WSAGetLastError());
		return 1;
	}
	//else
		//printf("WSAEventSelect() is pretty fine!\n");
	HANDLE events[] = { ssObj->full, ssObj->socketReceiveReady,  ssObj->p.eventQuit };

	DWORD timeout;
	while (true)
	{
		//if (pending packets)
		//timeout = timerExpire - cur_time;
		//else
		//timeout=INFINITE

		WaitForSingleObject(ssObj->p.mutex, INFINITE);					// lock mutex
		//timeout = ssObj->p.RTT*1000;
		timeout = 1000*(ssObj->p.RTT + 4 * max(ssObj->p.devRTT, 0.010));
		ReleaseMutex(ssObj->p.mutex);
		
		int ret = WaitForMultipleObjects(3, events, false, timeout);
		/*
		if (ret == WAIT_OBJECT_0 + 2)
		{
			printf("quit event called here\n");
			timeout = INFINITE;
		}*/
		//printf("timeout is %d\n", timeout);
		switch (ret)
		{
			case WAIT_TIMEOUT:
			{
#if DBG
				printf("WAIT_TIMEOUT senderBase %d last pkt to be sent SeqNo %d \n", ssObj->senderBase , ssObj->nextSeq);
#endif
				if (ssObj->sendDone == true)
				{
					if (ssObj->senderBase == ssObj->nextSeq)
					{
						workerDone = true;
						return 0;
					}
				}
				//ssObj->attempts[ssObj->senderBase]++;
#if DBG
				printf("retx WAIT_TIMEOUT 1\n");
#endif
				if (ssObj->attempts[ssObj->senderBase% ssObj->W] < MAX_ATTEMPTS)
				{
					int retv = ssObj->SendTo(ssObj->senderBase);
					if (retv != 1)
						return ret;
					WaitForSingleObject(ssObj->p.mutex, INFINITE);					// lock mutex
					ssObj->p.T = (ssObj->p.T) + 1;
					ReleaseMutex(ssObj->p.mutex);									// release critical section
					
				}
				else
					return TIMEOUT;
#if DBG
				printf("retx WAIT_TIMEOUT 2\n");
#endif
				

				break;
			}
			case WAIT_OBJECT_0 +1:
			{
	#if DBUG
				printf("wait for socket to be read\n");
	#endif
				WSAEnumNetworkEvents(ssObj->sock, ssObj->socketReceiveReady, &ssObj->NetworkEvents);
				ssObj->ReceiveACK();
				break;
			}
			case WAIT_OBJECT_0:
			{
				//printf("full event\n");

				int retv = ssObj->SendTo(nextToSend);
				nextToSend++;
				//printf("WorkerRun 2 full\n");
				if (retv != 1)
					return ret;
				break;
			}
			/*
			case WAIT_OBJECT_0 + 2:
			{
				printf("event quit signaled\n");
				//return 0;
				break;
			}*/
			default: //handle failed wait;
			{
				//printf("wait ssObj->senderBase %d last pkt to be sent SeqNo %d \n", ssObj->senderBase , ssObj->nextSeq);
				if (ssObj->sendDone == true)
				{
					if (ssObj->senderBase == ssObj->nextSeq)
					{
						workerDone = true;
						return 0;
					}
				}
			}
		}
		/*
		if (first packet of window || just did a retx(timeout / 3 - dup ACK)
		|| senderBase moved forward)
		recompute timerExpire;
		*/
		
	}
	return 0;
}

int SenderSocket::SendTo(long nextToSend)
{
#if DBUG
	printf("\nSendTo 0 started nextToSend %d\n", nextToSend);
#endif
	
	clock_t last_syn = clock();
	long index = nextToSend % W;
	dataLastSentTime[index] = last_syn;
	double cpu_time_used = ((double)(last_syn - constr_start_time)) / CLOCKS_PER_SEC;//in s
	int pkt_size = sizeof(SenderDataHeader) + pending_pkts_len[index];
	
#if DBUG
	printf("SendTo 1 pktsize %d\n", pkt_size);
#endif
	
	Packet pkt = pending_pkts[index];

	if (sendto(sock, (char*)&pkt, pkt_size, 0, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR)
	{
		clock_t temp = clock();
		cpu_time_used = ((double)(temp - constr_start_time)) / CLOCKS_PER_SEC;//in s
		printf("[ %.3f]\t--> failed sendto with %d\n", cpu_time_used, WSAGetLastError());
		exit(0);
		return FAILED_SEND;
	}
	attempts[index]++;
#if DBG
	printf("\n[ %.3f]\t--> data %d (attempt %d of %d, RTO %.3f) to %s\n", cpu_time_used, pkt.sdh.seq, attempts[index], MAX_ATTEMPTS, initialRTO, inet_ntoa(server.sin_addr));
#endif
#if DBUG
	printf("SendTo 2 done\n");
#endif
	return 1;
}

int SenderSocket::ReceiveACK(void)
{
#if DBUG
	printf("ReceiveACK 0\n");
#endif
	
	//get ACK with sequence y, receiver window R
	ReceiverHeader rh;
	struct sockaddr_in response;
	size = sizeof(response);
	int iResult = 0;
	double cpu_time_used;

	iResult = recvfrom(sock, (char*)&rh, sizeof(ReceiverHeader), 0, (struct sockaddr*)&response, &size);
	if (iResult == SOCKET_ERROR)
	{
		//error processing
		clock_t temp = clock();
		cpu_time_used = ((double)(temp - constr_start_time)) / CLOCKS_PER_SEC;//in s
		printf("[ %.3f]\t<-- failed recvfrom with %d\n", cpu_time_used, WSAGetLastError());
		return FAILED_RECV;
	}
	clock_t syn_ack = clock();
	cpu_time_used = ((double)(syn_ack - constr_start_time)) / CLOCKS_PER_SEC;//in s

#if PRINT
	printf("[ %.3f]\t<-- data-ACK %d window %d; setting initial RTO to 1\n", cpu_time_used, rh.ackSeq, rh.recvWnd);
#endif
	
	DWORD y = rh.ackSeq;
	clock_t last_syn = dataLastSentTime[(y - 1) % W];
	WaitForSingleObject(p.mutex, INFINITE);					// lock mutex
	//int pkt_size =pending_pkts_len[y-1 % W];
	p.B = senderBase;
	p.N = y;
	ReleaseMutex(p.mutex);									// release critical section
	
#if DBUG
	printf("\n\n\t\tb4 y %d senderBase %d nextSeq %d rh.recvWnd %d\n", y, senderBase, nextSeq, rh.recvWnd);
#endif															
		
	if (y > senderBase)
	{
		

		senderBase = y;
		int effectiveWin = min(W, rh.recvWnd);
		WaitForSingleObject(p.mutex, INFINITE);					// lock mutex
		p.W = effectiveWin;
		p.B = senderBase;
		p.MB = (p.MB * 1000000 + MAX_PKT_SIZE) / 1000000;
		ReleaseMutex(p.mutex);									// release critical section
		// how much we can advance the semaphore
		int newReleased = senderBase + effectiveWin - lastReleased;
		ReleaseSemaphore(empty, newReleased, NULL);
		lastReleased += newReleased;
	}
	else
	{
		//increment no of dup acks received for y 
		dupAckCount[y%W]++;
#if DBG
		printf("dup ack count of %d is %d\n", y,dupAckCount[y%W]);
#endif
		if (dupAckCount[y%W] == 3)
		{
			int ret = SendTo(y);
#if DBG
			printf("\n\t\t\tfast retx\n");
#endif
			WaitForSingleObject(p.mutex, INFINITE);					// lock mutex
			p.F = (p.F) + 1;
			ReleaseMutex(p.mutex);									// release critical section
			//dupAckCount[y%W]=0;
		}
	}
#if DBUG
	//printf("\n\n\t\ta4 y %d senderBase %d nextSeq %d\n", y, senderBase, nextSeq);
#endif
	WaitForSingleObject(p.mutex, INFINITE);
	double sampledRTT = (double)(syn_ack - last_syn) / CLOCKS_PER_SEC;
	p.RTT = (1 - ALPHA)*p.RTT + ALPHA * sampledRTT;
	p.devRTT = (1 - BETA)*p.devRTT + BETA * mod(p.RTT - sampledRTT);
	//initialRTO = p.RTT + 4 * max(p.devRTT, 0.010);
	ReleaseMutex(p.mutex);									// release critical section
	
	return STATUS_OK;
}
