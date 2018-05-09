#pragma once
class Checksum {
public:
	Checksum();
	DWORD CRC32(unsigned char *buf, size_t len);
	DWORD crc_table[256];
	//32btye
	//unsigned char
};