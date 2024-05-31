#include <string.h>

#if !defined(ENABLE_OVERLAY)
	#include "ARMCM0.h"
#endif
#ifdef ENABLE_FMRADIO
	#include "app/fm.h"
#endif
#if defined(ENABLE_MESSENGER) || defined(ENABLE_MESSENGER_UART)
	#include "app/messenger.h"
  	#include "external/printf/printf.h"
#endif
#include "app/uart.h"
#include "board.h"
#include "bsp/dp32g030/dma.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/aes.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/crc.h"
#include "driver/eeprom.h"
#include "driver/gpio.h"
#include "driver/systick.h"
#include "driver/system.h"
#include "driver/uart.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#include "version.h"

#define DMA_INDEX(x, y) (((x) + (y)) % sizeof(UART_DMA_Buffer))

typedef struct {
	uint8_t FF;
	uint16_t Command;
	uint8_t Size;
} Header_FF;

static union
{
	uint8_t Buffer[256];
	struct
	{
		Header_FF Header;
		uint8_t Data[252];
	};
} UART_Command;

static uint16_t gUART_WriteIndex;

bool UART_IsCommandAvailable(void)
{
	uint16_t Size;
	uint16_t DmaLength = DMA_CH0->ST & 0xFFFU;

	while (1)
	{
		if (gUART_WriteIndex == DmaLength) return false;

		while (gUART_WriteIndex != DmaLength && UART_DMA_Buffer[gUART_WriteIndex] != 0xFFU)
			gUART_WriteIndex = DMA_INDEX(gUART_WriteIndex, 1);

		if (gUART_WriteIndex == DmaLength)
			return false;

		if ((UART_DMA_Buffer[DMA_INDEX(gUART_WriteIndex, 2)] == 0x09) && (UART_DMA_Buffer[DMA_INDEX(gUART_WriteIndex, 1)] == 0x00))
			break;

		gUART_WriteIndex = DMA_INDEX(gUART_WriteIndex, 1);
	}

	Size = UART_DMA_Buffer[DMA_INDEX(gUART_WriteIndex, 3)];
	if (Size == 0) return false;

	memset(UART_Command.Buffer, 0, sizeof(UART_Command.Buffer));
	memcpy(UART_Command.Buffer, UART_DMA_Buffer, Size);
	return true;
}

void SEND_PACKET(uint8_t msgFSKBuffer[], uint8_t size)
{
	uint16_t fsk_reg59;

	// REG_51
	//
	// <15>  TxCTCSS/CDCSS   0 = disable 1 = Enable
	//
	// turn off CTCSS/CDCSS during FFSK
	const uint16_t css_val = BK4819_ReadRegister(BK4819_REG_51);
	BK4819_WriteRegister(BK4819_REG_51, 0);

	// set the FM deviation level
	const uint16_t dev_val = BK4819_ReadRegister(0x40U);
	{
		uint16_t deviation = 850;
		switch (gEeprom.VfoInfo[gEeprom.TX_VFO].CHANNEL_BANDWIDTH)
		{
			case BK4819_FILTER_BW_WIDE:     deviation = 1050; break;
			case BK4819_FILTER_BW_NARROW:   deviation =  850; break;
			case BK4819_FILTER_BW_NARROWER: deviation =  750; break;
		}
		//BK4819_WriteRegister(0x40, (3u << 12) | (deviation & 0xfff));
		BK4819_WriteRegister(0x40U, (dev_val & 0xf000) | (deviation & 0xfff));
	}

	// REG_2B   0
	//
	// <15> 1 Enable CTCSS/CDCSS DC cancellation after FM Demodulation   1 = enable 0 = disable
	// <14> 1 Enable AF DC cancellation after FM Demodulation            1 = enable 0 = disable
	// <10> 0 AF RX HPF 300Hz filter     0 = enable 1 = disable
	// <9>  0 AF RX LPF 3kHz filter      0 = enable 1 = disable
	// <8>  0 AF RX de-emphasis filter   0 = enable 1 = disable
	// <2>  0 AF TX HPF 300Hz filter     0 = enable 1 = disable
	// <1>  0 AF TX LPF filter           0 = enable 1 = disable
	// <0>  0 AF TX pre-emphasis filter  0 = enable 1 = disable
	//
	// disable the 300Hz HPF and FM pre-emphasis filter
	//
	const uint16_t filt_val = BK4819_ReadRegister(BK4819_REG_2B);
	BK4819_WriteRegister(BK4819_REG_2B, (1u << 2) | (1u << 0));

	// *******************************************
	// setup the FFSK modem as best we can

	// Uses 1200/1800 Hz FSK tone frequencies 1200 bits/s
	//
	BK4819_WriteRegister(BK4819_REG_58, // 0x37C3);   // 001 101 11 11 00 001 1
		(1u << 13) |		// 1 FSK TX mode selection
							//   0 = FSK 1.2K and FSK 2.4K TX .. no tones, direct FM
							//   1 = FFSK 1200/1800 TX
							//   2 = ???
							//   3 = FFSK 1200/2400 TX
							//   4 = ???
							//   5 = NOAA SAME TX
							//   6 = ???
							//   7 = ???
							//
		(7u << 10) |		// 0 FSK RX mode selection
							//   0 = FSK 1.2K, FSK 2.4K RX and NOAA SAME RX .. no tones, direct FM
							//   1 = ???
							//   2 = ???
							//   3 = ???
							//   4 = FFSK 1200/2400 RX
							//   5 = ???
							//   6 = ???
							//   7 = FFSK 1200/1800 RX
							//
		(0u << 8) |			// 0 FSK RX gain
							//   0 ~ 3
							//
		(0u << 6) |			// 0 ???
							//   0 ~ 3
							//
		(0u << 4) |			// 0 FSK preamble type selection
							//   0 = 0xAA or 0x55 due to the MSB of FSK sync byte 0
							//   1 = ???
							//   2 = 0x55
							//   3 = 0xAA
							//
		(1u << 1) |			// 1 FSK RX bandwidth setting
							//   0 = FSK 1.2K .. no tones, direct FM
							//   1 = FFSK 1200/1800
							//   2 = NOAA SAME RX
							//   3 = ???
							//   4 = FSK 2.4K and FFSK 1200/2400
							//   5 = ???
							//   6 = ???
							//   7 = ???
							//
		(1u << 0));			// 1 FSK enable
							//   0 = disable
							//   1 = enable

	// REG_72
	//
	// <15:0> 0x2854 TONE-2 / FSK frequency control word
	//        = freq(Hz) * 10.32444 for XTAL 13M / 26M or
	//        = freq(Hz) * 10.48576 for XTAL 12.8M / 19.2M / 25.6M / 38.4M
	//
	// tone-2 = 1200Hz
	// 18583,92
	BK4819_WriteRegister(BK4819_REG_72, 0x3065);

	// REG_70
	//
	// <15>   0 TONE-1
	//        1 = enable
	//        0 = disable
	//
	// <14:8> 0 TONE-1 tuning
	//
	// <7>    0 TONE-2
	//        1 = enable
	//        0 = disable
	//
	// <6:0>  0 TONE-2 / FSK tuning
	//        0 ~ 127
	//
	// enable tone-2, set gain
	//
	BK4819_WriteRegister(BK4819_REG_70,   // 0 0000000 1 1100000
		( 0u << 15) |    // 0
		( 0u <<  8) |    // 0
		( 1u <<  7) |    // 1
		(96u <<  0));    // 96

	// REG_59
	//
	// <15>  0 TX FIFO             1 = clear
	// <14>  0 RX FIFO             1 = clear
	// <13>  0 FSK Scramble        1 = Enable
	// <12>  0 FSK RX              1 = Enable
	// <11>  0 FSK TX              1 = Enable
	// <10>  0 FSK data when RX    1 = Invert
	// <9>   0 FSK data when TX    1 = Invert
	// <8>   0 ???
	//
	// <7:4> 0 FSK preamble length selection
	//       0  =  1 byte
	//       1  =  2 bytes
	//       2  =  3 bytes
	//       15 = 16 bytes
	//
	// <3>   0 FSK sync length selection
	//       0 = 2 bytes (FSK Sync Byte 0, 1)
	//       1 = 4 bytes (FSK Sync Byte 0, 1, 2, 3)
	//
	// <2:0> 0 ???
	//
	fsk_reg59 = (0u << 15) |   // 0/1     1 = clear TX FIFO
				(0u << 14) |   // 0/1     1 = clear RX FIFO
				(0u << 13) |   // 0/1     1 = scramble
				(0u << 12) |   // 0/1     1 = enable RX
				(0u << 11) |   // 0/1     1 = enable TX
				(0u << 10) |   // 0/1     1 = invert data when RX
				(0u <<  9) |   // 0/1     1 = invert data when TX
				(0u <<  8) |   // 0/1     ???
				(15u <<  4) |   // 0 ~ 15  preamble length .. bit toggling
				(1u <<  3) |   // 0/1     sync length
				(0u <<  0);    // 0 ~ 7   ???

	// Set packet length (not including pre-amble and sync bytes that we can't seem to disable)
	BK4819_WriteRegister(BK4819_REG_5D, (size << 8));

	// REG_5A
	//
	// <15:8> 0x55 FSK Sync Byte 0 (Sync Byte 0 first, then 1,2,3)
	// <7:0>  0x55 FSK Sync Byte 1
	//
	BK4819_WriteRegister(BK4819_REG_5A, 0x5555);                   // bytes 1 & 2

	// REG_5B
	//
	// <15:8> 0x55 FSK Sync Byte 2 (Sync Byte 0 first, then 1,2,3)
	// <7:0>  0xAA FSK Sync Byte 3
	//
	BK4819_WriteRegister(BK4819_REG_5B, 0x55AA);                   // bytes 2 & 3

	// CRC setting (plus other stuff we don't know what)
	//
	// REG_5C
	//
	// <15:7> ???
	//
	// <6>    1 CRC option enable    0 = disable  1 = enable
	//
	// <5:0>  ???
	//
	// disable CRC
	//
	// NB, this also affects TX pre-amble in some way
	//
	BK4819_WriteRegister(BK4819_REG_5C, 0x5625);   // 010101100 0 100101
//		BK4819_WriteRegister(0x5C, 0xAA30);   // 101010100 0 110000
//		BK4819_WriteRegister(0x5C, 0x0030);   // 000000000 0 110000

	BK4819_WriteRegister(BK4819_REG_59, (1u << 15) | (1u << 14) | fsk_reg59);   // clear FIFO's
	BK4819_WriteRegister(BK4819_REG_59, fsk_reg59);

	SYSTEM_DelayMs(100);

	{	// load the entire packet data into the TX FIFO buffer			
		for (size_t i = 0, j = 0; i < size; i += 2, j++) {
			BK4819_WriteRegister(BK4819_REG_5F, (msgFSKBuffer[i + 1] << 8) | msgFSKBuffer[i]);
		}
	}

	// enable FSK TX
	BK4819_WriteRegister(BK4819_REG_59, (1u << 11) | fsk_reg59);

	{
		// allow up to 310ms for the TX to complete
		// if it takes any longer then somethings gone wrong, we shut the TX down
		unsigned int timeout = 1000 / 5;

		while (timeout-- > 0)
		{
			SYSTEM_DelayMs(5);
			if (BK4819_ReadRegister(BK4819_REG_0C) & (1u << 0))
			{	// we have interrupt flags
				BK4819_WriteRegister(BK4819_REG_02, 0);
				if (BK4819_ReadRegister(BK4819_REG_02) & BK4819_REG_02_FSK_TX_FINISHED)
					timeout = 0;       // TX is complete
			}
		}
	}
	//BK4819_WriteRegister(BK4819_REG_02, 0);

	SYSTEM_DelayMs(100);

	// disable FSK
	BK4819_WriteRegister(BK4819_REG_59, fsk_reg59);

	// restore FM deviation level
	BK4819_WriteRegister(0x40U, dev_val);

	// restore TX/RX filtering
	BK4819_WriteRegister(BK4819_REG_2B, filt_val);

	// restore the CTCSS/CDCSS setting
	BK4819_WriteRegister(BK4819_REG_51, css_val);
}

void UART_HandleCommand(void)
{
	switch (UART_Command.Header.Command)
	{
		case 0x900: // SEND FSK DATA
			SEND_PACKET(UART_Command.Data, UART_Command.Header.Size);
			break;
	}
}

void UART_SendUiElement(uint8_t type, uint32_t value1, uint32_t value2, uint32_t value3, uint32_t Length, const void* data)
{
	const uint8_t id = 0xB5;
	UART_Send(&id, 1);
	UART_Send(&type, 1);
	UART_Send(&value1, 1);
	UART_Send(&value2, 1);
	UART_Send(&value3, 1);
	UART_Send(&Length, 1);
	UART_Send(data, Length);
}