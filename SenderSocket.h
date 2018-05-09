#pragma once
// SenderSocket.h
#define MAGIC_PORT 22345 // receiver listens on this port
#define MAX_PKT_SIZE (1500-28) // maximum UDP packet size accepted by receiver

// possible status codes from ss.Open, ss.Send, ss.Close
#define STATUS_OK 0 // no error
#define ALREADY_CONNECTED 1 // second call to ss.Open() without closing connection
#define NOT_CONNECTED 2 // call to ss.Send()/Close() without ss.Open()
#define INVALID_NAME 3 // ss.Open() with targetHost that has no DNS entry
#define FAILED_SEND 4 // sendto() failed in kernel
#define TIMEOUT 5 // timeout after all retx attempts are exhausted
#define FAILED_RECV 6 // recvfrom() failed in kernel
#define FAIL -1

#define FORWARD_PATH 0
#define RETURN_PATH 1

#define MAGIC_PROTOCOL 0x8311AA

#define MAX_SYN_ATTEMPTS 50
#define MAX_ATTEMPTS 50
#define ALPHA 0.125
#define BETA 0.25

#pragma pack(push,1) //sets struct padding/alignment to 1 byte
class LinkProperties {
public:
	// transfer parameters
	float RTT; // propagation RTT (in sec)
	float speed; // bottleneck bandwidth (in bits/sec)
	float pLoss[2]; // probability of loss in each direction
	DWORD bufferSize; // buffer size of emulated routers (in packets)
	LinkProperties() { memset(this, 0, sizeof(*this)); }
};
class Flags {
public:
	DWORD reserved : 5; // must be zero
	DWORD SYN : 1;
	DWORD ACK : 1;
	DWORD FIN : 1;
	DWORD magic : 24;
	Flags() { memset(this, 0, sizeof(*this)); magic = MAGIC_PROTOCOL; }
};
class SenderDataHeader {
public:
	Flags flags;
	DWORD seq; // must begin from 0
};
class SenderSynHeader {
public:
	SenderDataHeader sdh;
	LinkProperties lp;
};
class ReceiverHeader {
public:
	Flags flags;
	DWORD recvWnd; // receiver window for flow control (in pkts)
	DWORD ackSeq; // ack value = next expected sequence
};
class Packet {
public:
	SenderDataHeader sdh;
	char data[MAX_PKT_SIZE-sizeof(SenderDataHeader)];
};
#pragma pack(pop) //restores old packing
class Parameters {
public:
	HANDLE	mutex;
	//HANDLE	finished;
	HANDLE	eventQuit;
	

	//queue<string> Q;
	//std::string inputFile;

	volatile long  B = -1; // sender base
	volatile float MB =0.0; // MB acked by the receiver
	volatile long  N=B+1; // next seq no
	volatile long  T=0; // pkts with timeouts
	volatile long  F=0; // pkts with fast retx
	volatile long  W; // window size= min(sender win, receiver win)
	volatile float  S; // data consumption speed @ receiver
	volatile float  RTT=0; // estimated RTT
	volatile float	devRTT=0;

};

class SenderSocket {
public:
	//send
	long nextSeq=0;

	//worker run
	DWORD timeout;
	long senderBase = -1;
	long W;
	

	//flow control
	long lastReleased = 0;



	SOCKET sock;
	WSADATA wsaData;
	WORD wVersionRequested;
	// structure for connecting to server
	struct sockaddr_in server;
	LinkProperties linkProps;
	clock_t constr_start_time;
	float initialRTO;
	int size;
	bool openCalled;
	bool closeCalled;
	Parameters p;

	HANDLE stats_handle;
	HANDLE worker_handle;
	HANDLE empty, full, eventQuit, Ready;
	WSAEVENT socketReceiveReady;
	WSANETWORKEVENTS NetworkEvents;

	Packet* pending_pkts;
	int* pending_pkts_len;
	int* attempts;
	clock_t* dataLastSentTime;
	clock_t* dataAckedTime;
	int* dupAckCount;
	bool sendDone = false;
	//bool workerDone = false;
	
public:
	SenderSocket();
	~SenderSocket();
	
	int Open(char* targetHost, int receiverPort, int senderWindow, LinkProperties* linkProp);
	int Send(char* buf, int pkt_size);
	int Close(float *estimatedRTT);
	//UINT StatsRun(LPVOID pParam);
	//DWORD WINAPI WorkerRun(LPVOID pParam);
	//DWORD WINAPI workerRun = WorkerRun(&p);
	void InitThreadParams(void);
	int ReceiveACK(void);
	int SendTo(long nextToSend);
	void CreateThreadHandles(void);
	
};