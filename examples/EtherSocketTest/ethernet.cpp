

#include "ethernet.h"
#include <stdarg.h>
#include <avr/eeprom.h>




uint8_t selectPin;
static byte Enc28j60Bank;
static int gNextPacketPtr;

bool EtherSocket::broadcast_enabled = false;


MACAddress EtherSocket::localMAC;
IPAddress EtherSocket::localIP;


ARPEntry arpTable[ARP_TABLE_LENGTH];
EthBuffer EtherSocket::chunk;

static unsigned long tickTimer = 1000;

void initSPI() {
	pinMode(SS, OUTPUT);
	digitalWrite(SS, HIGH);
	pinMode(MOSI, OUTPUT);
	pinMode(SCK, OUTPUT);
	pinMode(MISO, INPUT);

	digitalWrite(MOSI, HIGH);
	digitalWrite(MOSI, LOW);
	digitalWrite(SCK, LOW);

	SPCR = bit(SPE) | bit(MSTR); // 8 MHz @ 16
	bitSet(SPSR, SPI2X);
}

static void enableChip() {
	cli();
	digitalWrite(selectPin, LOW);
}

static void disableChip() {
	digitalWrite(selectPin, HIGH);
	sei();
}

static void xferSPI(byte data) {
	SPDR = data;
	while (!(SPSR&(1 << SPIF)))
		;
}

static byte readOp(byte op, byte address) {
	enableChip();
	xferSPI(op | (address & ADDR_MASK));
	xferSPI(0x00);
	if (address & 0x80)
		xferSPI(0x00);
	byte result = SPDR;
	disableChip();
	return result;
}

static void writeOp(byte op, byte address, byte data) {
	enableChip();
	xferSPI(op | (address & ADDR_MASK));
	xferSPI(data);
	disableChip();
}

static void readBuf(uint16_t len, byte* data) {
	enableChip();
	xferSPI(ENC28J60_READ_BUF_MEM);
	while (len--) {
		xferSPI(0x00);
		*data++ = SPDR;
	}
	disableChip();
}

static void writeBuf(uint16_t len, const byte* data) {
	enableChip();
	xferSPI(ENC28J60_WRITE_BUF_MEM);
	while (len--)
		xferSPI(*data++);
	disableChip();
}





static void SetBank(byte address) {
	if ((address & BANK_MASK) != Enc28j60Bank) {
		writeOp(ENC28J60_BIT_FIELD_CLR, ECON1, ECON1_BSEL1 | ECON1_BSEL0);
		Enc28j60Bank = address & BANK_MASK;
		writeOp(ENC28J60_BIT_FIELD_SET, ECON1, Enc28j60Bank >> 5);
	}
}

static byte readRegByte(byte address) {
	SetBank(address);
	return readOp(ENC28J60_READ_CTRL_REG, address);
}

static uint16_t readReg(byte address) {
	return readRegByte(address) + (readRegByte(address + 1) << 8);
}


static void writeRegByte(byte address, byte data) {
	SetBank(address);
	writeOp(ENC28J60_WRITE_CTRL_REG, address, data);
}

static void writeReg(byte address, uint16_t data) {
	writeRegByte(address, data);
	writeRegByte(address + 1, data >> 8);
}

void writeSlot(uint8_t slot, uint16_t offset, const byte * data, uint16_t len)
{
	uint16_t txStart = SLOT_ADDR(slot);
	writeReg(EWRPT, txStart+offset);
	writeOp(ENC28J60_WRITE_BUF_MEM, 0, 0x00);
	writeBuf(len, data);


	uint8_t buf[50];

	writeReg(ERDPT, txStart);

}

void packetSend(uint8_t slot, uint16_t len)
{
	// see http://forum.mysensors.org/topic/536/
	// while (readOp(ENC28J60_READ_CTRL_REG, ECON1) & ECON1_TXRTS)
	if (readRegByte(EIR) & EIR_TXERIF) {
		writeOp(ENC28J60_BIT_FIELD_SET, ECON1, ECON1_TXRST);
		writeOp(ENC28J60_BIT_FIELD_CLR, ECON1, ECON1_TXRST);
		writeOp(ENC28J60_BIT_FIELD_CLR, EIR, EIR_TXERIF);
	}

	uint16_t txStart = SLOT_ADDR(slot);
	writeReg(ETXST, txStart);
	writeReg(ETXND, txStart + len);

	writeOp(ENC28J60_BIT_FIELD_SET, ECON1, ECON1_TXRTS);

}

void EtherSocket::enableBroadcast(bool temporary) {
	writeRegByte(ERXFCON, readRegByte(ERXFCON) | ERXFCON_BCEN);
	if (!temporary)
		broadcast_enabled = true;
}

static void writePhy(byte address, uint16_t data) {
	writeRegByte(MIREGADR, address);
	writeReg(MIWR, data);
	while (readRegByte(MISTAT) & MISTAT_BUSY)
		;
}

static uint16_t readPhyByte(byte address) {
	writeRegByte(MIREGADR, address);
	writeRegByte(MICMD, MICMD_MIIRD);
	while (readRegByte(MISTAT) & MISTAT_BUSY)
		;
	writeRegByte(MICMD, 0x00);
	return readRegByte(MIRD + 1);
}

bool EtherSocket::isLinkUp() {
	return (readPhyByte(PHSTAT2) >> 2) & 1;
}

uint8_t EtherSocket::begin(uint8_t cspin)
{
	memset(arpTable, -2, ARP_TABLE_LENGTH * sizeof(ARPEntry));
	
	if (bitRead(SPCR, SPE) == 0)
		initSPI();
	selectPin = cspin;
	pinMode(selectPin, OUTPUT);
	disableChip();

	writeOp(ENC28J60_SOFT_RESET, 0, ENC28J60_SOFT_RESET);
	delay(2); // errata B7/2
	while (!readOp(ENC28J60_READ_CTRL_REG, ESTAT) & ESTAT_CLKRDY)
		;

	gNextPacketPtr = RXSTART_INIT;
	writeReg(ERXST, RXSTART_INIT);
	writeReg(ERXRDPT, RXSTART_INIT);
	writeReg(ERXND, RXSTOP_INIT);
	writeReg(ETXST, TXSTART_INIT);
	writeReg(ETXND, TXSTOP_INIT);
	enableBroadcast(); // change to add ERXFCON_BCEN recommended by epam
	writeReg(EPMM0, 0x303f);
	writeReg(EPMCS, 0xf7f9);
	writeRegByte(MACON1, MACON1_MARXEN | MACON1_TXPAUS | MACON1_RXPAUS);
	writeRegByte(MACON2, 0x00);
	writeOp(ENC28J60_BIT_FIELD_SET, MACON3,
		MACON3_PADCFG0 | MACON3_TXCRCEN | MACON3_FRMLNEN);
	writeReg(MAIPG, 0x0C12);
	writeRegByte(MABBIPG, 0x12);
	writeReg(MAMXFL, MAX_FRAMELEN);
	writeRegByte(MAADR5, localMAC.b[0]);
	writeRegByte(MAADR4, localMAC.b[1]);
	writeRegByte(MAADR3, localMAC.b[2]);
	writeRegByte(MAADR2, localMAC.b[3]);
	writeRegByte(MAADR1, localMAC.b[4]);
	writeRegByte(MAADR0, localMAC.b[5]);
	writePhy(PHCON2, PHCON2_HDLDIS);
	SetBank(ECON1);
	writeOp(ENC28J60_BIT_FIELD_SET, EIE, EIE_INTIE | EIE_PKTIE);
	writeOp(ENC28J60_BIT_FIELD_SET, ECON1, ECON1_RXEN);

	byte rev = readRegByte(EREVID);
	// microchip forgot to step the number on the silcon when they
	// released the revision B7. 6 is now rev B7. We still have
	// to see what they do when they release B8. At the moment
	// there is no B8 out yet
	if (rev > 5) ++rev;
	return rev;



}

uint16_t EtherSocket::packetReceiveChunk()
{
	uint16_t len = 0;
	if (readRegByte(EPKTCNT) > 0)
	{
		writeReg(ERDPT, gNextPacketPtr);

		struct
		{
			uint16_t nextPacket;
			uint16_t byteCount;
			uint16_t status;
		} header;

		readBuf(sizeof header, (byte*)&header);

		gNextPacketPtr = header.nextPacket;
		len = header.byteCount - 4; //remove the CRC count

		if ((header.status & 0x80) == 0)
			len = 0;

		uint16_t chunkLength;

		bool isHeader = true;
		while (len > 0)
		{
			chunkLength = min(sizeof(chunk), len);
			readBuf(chunkLength, chunk.raw);
			processChunk(isHeader, chunkLength);
			len -= chunkLength;
			isHeader = false;

		}

		if (gNextPacketPtr - 1 > RXSTOP_INIT)
			writeReg(ERXRDPT, RXSTOP_INIT);
		else
			writeReg(ERXRDPT, gNextPacketPtr - 1);
		writeOp(ENC28J60_BIT_FIELD_SET, ECON2, ECON2_PKTDEC);
	}
	return len;

}

void EtherSocket::loop()
{
	packetReceiveChunk();

	if ((long)(millis() - tickTimer) >= 0)
	{
		tick();
		tickTimer += 1000;
	}

}

void EtherSocket::tick()
{
	for (ARPEntry* entry = arpTable + (ARP_TABLE_LENGTH - 1); entry >= arpTable; entry--)
	{
		if (entry->status_TTL > 0)
			entry->status_TTL--;
	}
}


void EtherSocket::processChunk(bool isHeader, uint16_t len)
{
	if (isHeader)
	{
		if (chunk.etherType.getValue() == ETHTYPE_ARP && chunk.arp.OPER.l == ARP_OPCODE_REPLY_L)
		{
			Serial.println("ARP Reply received=");
			Serial.println(chunk.arp.senderMAC.b[1]);
			processARPReply();
			
		}
	}

}





MACAddress* EtherSocket::whoHas(IPAddress& ip)
{
	for (ARPEntry* entry = arpTable + (ARP_TABLE_LENGTH-1);entry >= arpTable;entry--)
	{
		if (ip.u == entry->ip.u && entry->status_TTL > 0)
		{
			entry->status_TTL = MAX_ARP_TTL;
			return &entry->mac;
		}
	}

	makeWhoHasARPRequest(ip);

	return NULL;


}



void EtherSocket::makeWhoHasARPRequest(IPAddress& ip)
{
	memset(&chunk.dstMAC, 0xFF, sizeof(MACAddress));
	chunk.srcMAC = chunk.arp.senderMAC = localMAC;
	chunk.etherType.setValue(ETHTYPE_ARP);
	chunk.arp.HTYPE.setValue(0x0001);
	chunk.arp.PTYPE.setValue(0x0800);
	chunk.arp.HLEN = 0x06;
	chunk.arp.PLEN = 0x04;
	chunk.arp.OPER.setValue(0x0001);
	memset(&chunk.arp.targetMAC, 0x00, sizeof(MACAddress));
	chunk.arp.targetIP = ip;
	chunk.arp.senderIP = localIP;

	writeSlot(0,0, chunk.raw, 6 + 6 + 2 + 28);
	packetSend(0, 6 + 6 + 2 + 28);


}

void EtherSocket::processARPReply()
{
	int16_t lowest = MAX_ARP_TTL;
	ARPEntry * selectedEntry;
	for (ARPEntry* entry = arpTable + (ARP_TABLE_LENGTH - 1); entry >= arpTable; entry--)
	{
		if (entry->status_TTL < lowest)
		{
			lowest = entry->status_TTL;
			selectedEntry = entry;
		}
	}

	selectedEntry->status_TTL = MAX_ARP_TTL;
	selectedEntry->ip = chunk.arp.senderIP;
	selectedEntry->mac = chunk.arp.senderMAC;

}