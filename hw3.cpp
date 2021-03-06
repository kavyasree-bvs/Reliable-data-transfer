// hw3.cpp : Defines the entry point for the console application.
//
/*
hw 3 part 3: Reliable data transfer
Name: Venkata Satya Kavya Sree Bondapalli
UIN: 725006670
Course: CSCE 612 Networks and Distributed Processing
Semester: Spring 2018
Instructor: Dmitri Loguinov
*/

#include "stdafx.h"
#include "SenderSocket.h"
#include "checksum.h"

void main(int argc, char** argv)
{
	if (argc != 8)
	{
		printf("incorrect number of arguments\n");
		return;
	}

	// parse command-line parameters
	char* targetHost = argv[1];
	
	int power = atoi(argv[2]); // command-line specified integer
	int senderWindow = atoi(argv[3]); // command-line specified integer

	LinkProperties lp;
	lp.RTT = atof(argv[4]);
	lp.speed = 1e6 * atof(argv[7]); // convert to megabits
	lp.pLoss[FORWARD_PATH] = atof(argv[5]);
	lp.pLoss[RETURN_PATH] = atof(argv[6]);
	int bottleneckSpeed = atoi(argv[7]);

	printf("Main:\tsender W = %d, RTT = %.3f sec, loss %g / %g, link %d Mbps\n", senderWindow, lp.RTT, lp.pLoss[FORWARD_PATH], lp.pLoss[RETURN_PATH], bottleneckSpeed);
	printf("Main:\tinitializing DWORD array with 2^%d elements... ", power);

	clock_t start, end;
	double cpu_time_used;
	start = clock();
	UINT64 dwordBufSize = (UINT64)1 << power;
	DWORD *dwordBuf = new DWORD[dwordBufSize]; // user-requested buffer
	for (UINT64 i = 0; i < dwordBufSize; i++) // required initialization
		dwordBuf[i] = i;
	end = clock();
	cpu_time_used = 1000 * ((double)(end - start)) / CLOCKS_PER_SEC;//in ms
	printf("done in %d ms\n", cpu_time_used);

	start = clock();
	SenderSocket ss; // instance of your class
	int status;
	if ((status = ss.Open(targetHost, MAGIC_PORT, senderWindow, &lp)) != STATUS_OK)
	{
		printf("Main:\tconnect failed with status %d\n", status);
		delete dwordBuf;
		return;
		// error handling: print status and quit
	}
	
	end = clock();
	cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;//in s
	printf("Main:\tconnected to %s in %.3f sec, pkt %d bytes\n", targetHost, cpu_time_used, MAX_PKT_SIZE);

	char *charBuf = (char*)dwordBuf; // this buffer goes into socket
	UINT64 byteBufferSize = dwordBufSize << 2; // convert to bytes
	UINT64 off = 0; // current position in buffer
	start = clock();
	while (off < byteBufferSize)
	{
		// decide the size of next chunk
		int bytes = min(byteBufferSize - off, MAX_PKT_SIZE - sizeof(SenderDataHeader));
		// send chunk into socket 
		//printf("bytes = %d off = %d\n", bytes,off);

		if ((status = ss.Send(charBuf + off, bytes)) != STATUS_OK)
		{
			printf("Main:\tSend failed with status %d\n", status);
			delete dwordBuf;
			return;
			// error handing: print status and quit
		}
		off += bytes;
	}
	end = clock();
	cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;//in s
	
	float elapsedTime,estimatedRTT;
	//if ((status = ss.Close(&elapsedTime)) != STATUS_OK)
	if ((status = ss.Close(&estimatedRTT)) != STATUS_OK)
	{
		printf("Main:\tClose failed with status %d\n", status);
		
		delete dwordBuf;
		return;
		// error handing: print status and quit
	}
	
	// send the buffer, close the connection 
	Checksum cs;
	DWORD check = cs.CRC32((unsigned char*)charBuf, byteBufferSize);
	//printf("estimatedRTT %f\n", estimatedRTT);
	//printf("%d %ld\n", byteBufferSize, check);
	printf("Main:\ttransfer finished in %.3f sec, %.2f Kbps, checksum %x\n", cpu_time_used,8* byteBufferSize/(1000* cpu_time_used), check);
	printf("Main:\testRTT %.3f, ideal rate %.2f Kbps\n", estimatedRTT, senderWindow*(MAX_PKT_SIZE-sizeof(SenderDataHeader))*8 / (1000 * estimatedRTT));
	delete dwordBuf;
    return;
}

