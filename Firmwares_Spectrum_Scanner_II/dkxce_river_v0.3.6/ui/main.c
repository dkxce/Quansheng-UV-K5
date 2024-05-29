/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <string.h>
#include <stdlib.h>  // abs()

#include "app/chFrScanner.h"
#include "app/dtmf.h"
#include "bitmaps.h"
#include "board.h"
#include "driver/bk4819.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include "ui/main.h"
#include "ui/ui.h"

center_line_t center_line = CENTER_LINE_NONE;

const int8_t dBmCorrTable[7] = {
			-15, // band 1
			-25, // band 2
			-20, // band 3
			-4, // band 4
			-7, // band 5
			-6, // band 6
			 -1  // band 7
		};

const char *VfoStateStr[] = {
       [VFO_STATE_NORMAL]="",
       [VFO_STATE_BUSY]="BUSY",
       [VFO_STATE_BAT_LOW]="BAT LOW",
       [VFO_STATE_TX_DISABLE]="TX DISABLE",
       [VFO_STATE_TIMEOUT]="TIMEOUT",
       [VFO_STATE_ALARM]="ALARM",
       [VFO_STATE_VOLTAGE_HIGH]="VOLT HIGH"
};

// ***************************************************************************

#ifndef ENABLE_RSSI_BAR

static void DrawSmallAntennaAndBars(uint8_t *p, unsigned int level)
{
	if(level>6)
		level = 6;

	memcpy(p, BITMAP_Antenna, ARRAY_SIZE(BITMAP_Antenna));

	for(uint8_t i = 1; i <= level; i++) {
		char bar = (0xff << (6-i)) & 0x7F;
		memset(p + 2 + i*3, bar, 2);
	}
}

#endif

#if defined ENABLE_AUDIO_BAR || defined ENABLE_RSSI_BAR

static void DrawLevelBar(uint8_t xpos, uint8_t line, uint8_t level)
{
	const char hollowBar[] = {
		0b01111111,
		0b01000001,
		0b01000001,
		0b01111111
	};

	uint8_t *p_line = gFrameBuffer[line];
	level = MIN(level, 13);

	for(uint8_t i = 0; i < level; i++) {
		if(i < 9) {
			for(uint8_t j = 0; j < 4; j++)
				p_line[xpos + i * 5 + j] = (~(0x7F >> (i+1))) & 0x7F;
		}
		else {
			memcpy(p_line + (xpos + i * 5), &hollowBar, ARRAY_SIZE(hollowBar));
		}
	}
}
#endif

#ifdef ENABLE_AUDIO_BAR

unsigned int sqrt16(unsigned int value)
{	// return square root of 'value'
	unsigned int shift = 16;         // number of bits supplied in 'value' .. 2 ~ 32
	unsigned int bit   = 1u << --shift;
	unsigned int sqrti = 0;
	while (bit)
	{
		const unsigned int temp = ((sqrti << 1) | bit) << shift--;
		if (value >= temp) {
			value -= temp;
			sqrti |= bit;
		}
		bit >>= 1;
	}
	return sqrti;
}

void UI_DisplayAudioBar(void)
{
	if (gSetting_mic_bar)
	{
		if(gLowBattery && !gLowBatteryConfirmed)
			return;

		const unsigned int line      = 3;

		if (gCurrentFunction != FUNCTION_TRANSMIT ||
			gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
			|| gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
			)
		{
			return;  // screen is in use
		}

#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
		if (gAlarmState != ALARM_STATE_OFF)
			return;
#endif
		const unsigned int voice_amp  = BK4819_GetVoiceAmplitudeOut();  // 15:0

		// make non-linear to make more sensitive at low values
		const unsigned int level      = MIN(voice_amp * 8, 65535u);
		const unsigned int sqrt_level = MIN(sqrt16(level), 124u);
		uint8_t bars = 13 * sqrt_level / 124;

		uint8_t *p_line = gFrameBuffer[line];
		memset(p_line, 0, LCD_WIDTH);

		DrawLevelBar(62, line, bars);

		if (gCurrentFunction == FUNCTION_TRANSMIT)
			ST7565_BlitFullScreen();
	}
}
#endif


void DisplayRSSIBar(const bool now)
{
#if defined(ENABLE_RSSI_BAR)

	const int16_t rssi = BK4819_GetRSSI();
	const unsigned int txt_width    = 7 * 8;                 // 8 text chars
	const unsigned int bar_x        = 2 + txt_width + 4;     // X coord of bar graph

	const unsigned int line         = 3;
	uint8_t           *p_line        = gFrameBuffer[line];
	char               str[16];

	const char plus[] = {
		0b00011000,
		0b00011000,
		0b01111110,
		0b01111110,
		0b01111110,
		0b00011000,
		0b00011000,
	};

	if ((gEeprom.KEY_LOCK && gKeypadLocked > 0) || center_line != CENTER_LINE_RSSI)
		return;     // display is in use

	if (gCurrentFunction == FUNCTION_TRANSMIT ||
		gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
		|| gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
		)
		return;     // display is in use

	if (now)
		memset(p_line, 0, LCD_WIDTH);


	sLevelAttributes sLevelAtt;
		
	sLevelAtt = GetSLevelAttributes(rssi, gRxVfo->freq_config_RX.Frequency);
	
	uint8_t overS9Bars = MIN(sLevelAtt.over/10, 4);
	
	if(overS9Bars == 0) {
		sprintf(str, "% 4d S%d", sLevelAtt.dBmRssi, sLevelAtt.sLevel); 
	}
	else {
		sprintf(str, "% 4d  %2d", sLevelAtt.dBmRssi, sLevelAtt.over);
		memcpy(p_line + 2 + 7*5, &plus, ARRAY_SIZE(plus));
	}

	UI_PrintStringSmallNormal(str, 2, 0, line);
	DrawLevelBar(bar_x, line, sLevelAtt.sLevel + overS9Bars);

	if (now)
		ST7565_BlitLine(line);
#else
	int16_t rssi = BK4819_GetRSSI();
	uint8_t Level;

	if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][3]) {
		Level = 6;
	} else if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][2]) {
		Level = 4;
	} else if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][1]) {
		Level = 2;
	} else if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][0]) {
		Level = 1;
	} else {
		Level = 0;
	}

	uint8_t *pLine = (gEeprom.RX_VFO == 0)? gFrameBuffer[2] : gFrameBuffer[6];
	if (now)
		memset(pLine, 0, 23);
	DrawSmallAntennaAndBars(pLine, Level);
	if (now)
		ST7565_BlitFullScreen();
#endif

}

#ifdef ENABLE_AGC_SHOW_DATA
void UI_MAIN_PrintAGC(bool now)
{
	char buf[20];
	memset(gFrameBuffer[3], 0, 128);
	union {
		struct {
			uint16_t _ : 5;
			uint16_t agcSigStrength : 7;
			int16_t gainIdx : 3;
			uint16_t agcEnab : 1;
		};
    	uint16_t __raw;
	} reg7e;
	reg7e.__raw = BK4819_ReadRegister(0x7E);
	uint8_t gainAddr = reg7e.gainIdx < 0 ? 0x14 : 0x10 + reg7e.gainIdx;
	union {
		struct {
			uint16_t pga:3;
			uint16_t mixer:2;
			uint16_t lna:3;
			uint16_t lnaS:2;
		};
		uint16_t __raw;
	} agcGainReg;
	agcGainReg.__raw = BK4819_ReadRegister(gainAddr);
	int8_t lnaShortTab[] = {-28, -24, -19, 0};
	int8_t lnaTab[] = {-24, -19, -14, -9, -6, -4, -2, 0};
	int8_t mixerTab[] = {-8, -6, -3, 0};
	int8_t pgaTab[] = {-33, -27, -21, -15, -9, -6, -3, 0};
	int16_t agcGain = lnaShortTab[agcGainReg.lnaS] + lnaTab[agcGainReg.lna] + mixerTab[agcGainReg.mixer] + pgaTab[agcGainReg.pga];

	sprintf(buf, "%d%2d %2d %2d %3d", reg7e.agcEnab, reg7e.gainIdx, -agcGain, reg7e.agcSigStrength, BK4819_GetRSSI());
	UI_PrintStringSmallNormal(buf, 2, 0, 3);
	if(now)
		ST7565_BlitLine(3);
}
#endif

void UI_MAIN_TimeSlice500ms(void)
{
	if(gScreenToDisplay==DISPLAY_MAIN) {
#ifdef ENABLE_AGC_SHOW_DATA
		UI_MAIN_PrintAGC(true);
		return;
#endif

		if(FUNCTION_IsRx()) {
			DisplayRSSIBar(true);
		}
	}
}

// ***************************************************************************

void UI_DisplayMain(void)
{
	char               String[22];

	center_line = CENTER_LINE_NONE;

	// clear the screen
	UI_DisplayClear();

	if(gLowBattery && !gLowBatteryConfirmed) {
		UI_DisplayPopup("LOW BATTERY");
		ST7565_BlitFullScreen();
		return;
	}

	if (gEeprom.KEY_LOCK && gKeypadLocked > 0)
	{	// tell user how to unlock the keyboard
		UI_PrintString("Long press #", 0, LCD_WIDTH, 1, 8);
		UI_PrintString("to unlock",    0, LCD_WIDTH, 3, 8);
		ST7565_BlitFullScreen();
		return;
	}

	unsigned int activeTxVFO = gRxVfoIsActive ? gEeprom.RX_VFO : gEeprom.TX_VFO;

	for (unsigned int vfo_num = 0; vfo_num < 2; vfo_num++)
	{
		const unsigned int line0 = 0;  // text screen line
		const unsigned int line1 = 4;
		const unsigned int line       = (vfo_num == 0) ? line0 : line1;
		const bool         isMainVFO  = (vfo_num == gEeprom.TX_VFO);
		uint8_t           *p_line0    = gFrameBuffer[line + 0];
#ifndef ENABLE_SMALL_BOLD		
		uint8_t           *p_line1    = gFrameBuffer[line + 1];
#endif		
		enum Vfo_txtr_mode mode       = VFO_MODE_NONE;

		if (activeTxVFO != vfo_num) // this is not active TX VFO
		{
#ifdef ENABLE_SCAN_RANGES
			if(gScanRangeStart) {
				UI_PrintString("ScnRng", 5, 0, line, 8);
				sprintf(String, "%3u.%05u", gScanRangeStart / 100000, gScanRangeStart % 100000);
				UI_PrintStringSmallNormal(String, 56, 0, line);
				sprintf(String, "%3u.%05u", gScanRangeStop / 100000, gScanRangeStop % 100000);
				UI_PrintStringSmallNormal(String, 56, 0, line + 1);
				continue;
			}
#endif


			if (gDTMF_InputMode
#ifdef ENABLE_DTMF_CALLING
				|| gDTMF_CallState != DTMF_CALL_STATE_NONE || gDTMF_IsTx
#endif
			) {
				char *pPrintStr = "";
				// show DTMF stuff
#ifdef ENABLE_DTMF_CALLING
				char Contact[16];
				if (!gDTMF_InputMode) {
					if (gDTMF_CallState == DTMF_CALL_STATE_CALL_OUT) {
						pPrintStr = DTMF_FindContact(gDTMF_String, Contact) ? Contact : gDTMF_String;
					} else if (gDTMF_CallState == DTMF_CALL_STATE_RECEIVED || gDTMF_CallState == DTMF_CALL_STATE_RECEIVED_STAY){
						pPrintStr = DTMF_FindContact(gDTMF_Callee, Contact) ? Contact : gDTMF_Callee;
					}else if (gDTMF_IsTx) {
						pPrintStr = gDTMF_String;
					}
				}

				UI_PrintString(pPrintStr, 2, 0, 2 + (vfo_num * 3), 8);

				pPrintStr = "";
				if (!gDTMF_InputMode) {
					if (gDTMF_CallState == DTMF_CALL_STATE_CALL_OUT) {
						pPrintStr = (gDTMF_State == DTMF_STATE_CALL_OUT_RSP) ? "CALL OUT(RSP)" : "CALL OUT";
					} else if (gDTMF_CallState == DTMF_CALL_STATE_RECEIVED || gDTMF_CallState == DTMF_CALL_STATE_RECEIVED_STAY) {
						sprintf(String, "CALL FRM:%s", (DTMF_FindContact(gDTMF_Caller, Contact)) ? Contact : gDTMF_Caller);
						pPrintStr = String;
					} else if (gDTMF_IsTx) {
						pPrintStr = (gDTMF_State == DTMF_STATE_TX_SUCC) ? "DTMF TX(SUCC)" : "DTMF TX";
					}
				}
				else
#endif
				{
					sprintf(String, ">%s", gDTMF_InputBox);
					pPrintStr = String;
				}

				UI_PrintString(pPrintStr, 2, 0, 0 + (vfo_num * 3), 8);

				center_line = CENTER_LINE_IN_USE;
				continue;
			}

			// highlight the selected/used VFO with a marker

			if (isMainVFO)
				memcpy(p_line0 + 0, BITMAP_VFO_Default, sizeof(BITMAP_VFO_Default));
		}
		else // active TX VFO
		{	// highlight the selected/used VFO with a marker

			if (isMainVFO)
				memcpy(p_line0 + 0, BITMAP_VFO_Default, sizeof(BITMAP_VFO_Default));
			else
				memcpy(p_line0 + 0, BITMAP_VFO_NotDefault, sizeof(BITMAP_VFO_NotDefault));
		}

		if (gCurrentFunction == FUNCTION_TRANSMIT)
		{	// transmitting

#ifdef ENABLE_ALARM
			if (gAlarmState == ALARM_STATE_SITE_ALARM)
				mode = VFO_MODE_RX;
			else
#endif
			{
				if (activeTxVFO == vfo_num)
				{	// show the TX symbol
					mode = VFO_MODE_TX;
					UI_PrintStringSmallBold("TX", 14, 0, line);
				}
			}
		}
		else
		{	// receiving .. show the RX symbol
			mode = VFO_MODE_RX;
			if (FUNCTION_IsRx() && gEeprom.RX_VFO == vfo_num) {
				UI_PrintStringSmallBold("RX", 14, 0, line);
			}
		}

		if (IS_MR_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
		{	// channel mode
			const unsigned int x = 2;
			const bool inputting = gInputBoxIndex != 0 && gEeprom.TX_VFO == vfo_num;
			if (!inputting)
				sprintf(String, "M%u", gEeprom.ScreenChannel[vfo_num] + 1);
			else
				sprintf(String, "M%.3s", INPUTBOX_GetAscii());  // show the input text
			UI_PrintStringSmallNormal(String, x, 0, line + 1);
		}
		else if (IS_FREQ_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
		{	// frequency mode
			// show the frequency band number
			const unsigned int x = 2;
			char * buf = gEeprom.VfoInfo[vfo_num].pRX->Frequency < _1GHz_in_KHz ? "" : "+";
			sprintf(String, "F%u%s", 1 + gEeprom.ScreenChannel[vfo_num] - FREQ_CHANNEL_FIRST, buf);
			UI_PrintStringSmallNormal(String, x, 0, line + 1);
		}
#ifdef ENABLE_NOAA
		else
		{
			if (gInputBoxIndex == 0 || gEeprom.TX_VFO != vfo_num)
			{	// channel number
				sprintf(String, "N%u", 1 + gEeprom.ScreenChannel[vfo_num] - NOAA_CHANNEL_FIRST);
			}
			else
			{	// user entering channel number
				sprintf(String, "N%u%u", '0' + gInputBox[0], '0' + gInputBox[1]);
			}
			UI_PrintStringSmallNormal(String, 7, 0, line + 1);
		}
#endif

		// ************

		enum VfoState_t state = VfoState[vfo_num];

#ifdef ENABLE_ALARM
		if (gCurrentFunction == FUNCTION_TRANSMIT && gAlarmState == ALARM_STATE_SITE_ALARM) {
			if (activeTxVFO == vfo_num)
				state = VFO_STATE_ALARM;
		}
#endif

		uint32_t frequency = gEeprom.VfoInfo[vfo_num].pRX->Frequency;

		if (state != VFO_STATE_NORMAL)
		{
			if (state < ARRAY_SIZE(VfoStateStr))
				UI_PrintString(VfoStateStr[state], 31, 0, line, 8);
		}
		else if (gInputBoxIndex > 0 && IS_FREQ_CHANNEL(gEeprom.ScreenChannel[vfo_num]) && gEeprom.TX_VFO == vfo_num)
		{	// user entering a frequency
			const char * ascii = INPUTBOX_GetAscii();
			bool isGigaF = frequency>=_1GHz_in_KHz;
			sprintf(String, "%.*s.%.3s", 3 + isGigaF, ascii, ascii + 3 + isGigaF);
#ifdef ENABLE_BIG_FREQ
			if(!isGigaF) {
				// show the remaining 2 small frequency digits
				UI_PrintStringSmallNormal(String + 7, 113, 0, line + 1);
				String[7] = 0;
				// show the main large frequency digits
				UI_DisplayFrequency(String, 32, line, false);
			}
			else
#endif
			{
				// show the frequency in the main font
				UI_PrintString(String, 32, 0, line, 8);
			}

			continue;
		}
		else
		{
			if (gCurrentFunction == FUNCTION_TRANSMIT)
			{	// transmitting
				if (activeTxVFO == vfo_num)
					frequency = gEeprom.VfoInfo[vfo_num].pTX->Frequency;
			}

			if (IS_MR_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
			{	// it's a channel

				// show the scan list assigment symbols
				const ChannelAttributes_t att = gMR_ChannelAttributes[gEeprom.ScreenChannel[vfo_num]];
				if (att.scanlist1)
					memcpy(p_line0 + 113, BITMAP_ScanList1, sizeof(BITMAP_ScanList1));
				if (att.scanlist2)
					memcpy(p_line0 + 120, BITMAP_ScanList2, sizeof(BITMAP_ScanList2));

				// compander symbol
#ifndef ENABLE_BIG_FREQ
				if (att.compander)
					memcpy(p_line0 + 120 + LCD_WIDTH, BITMAP_compand, sizeof(BITMAP_compand));
#else
				// TODO:  // find somewhere else to put the symbol
#endif


				switch (gEeprom.CHANNEL_DISPLAY_MODE)
				{
					case MDF_FREQUENCY:	// show the channel frequency
						sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);
#ifdef ENABLE_BIG_FREQ
						if(frequency < _1GHz_in_KHz) {
							// show the remaining 2 small frequency digits
							UI_PrintStringSmallNormal(String + 7, 113, 0, line + 1);
							String[7] = 0;
							// show the main large frequency digits
							UI_DisplayFrequency(String, 32, line, false);
						}
						else
#endif
						{
							// show the frequency in the main font
							UI_PrintString(String, 32, 0, line, 8);
						}

						break;

					case MDF_CHANNEL:	// show the channel number
						sprintf(String, "CH-%03u", gEeprom.ScreenChannel[vfo_num] + 1);
						UI_PrintString(String, 32, 0, line, 8);
						break;

					case MDF_NAME:		// show the channel name
					case MDF_NAME_FREQ:	// show the channel name and frequency

						SETTINGS_FetchChannelName(String, gEeprom.ScreenChannel[vfo_num]);
						if (String[0] == 0)
						{	// no channel name, show the channel number instead
							sprintf(String, "CH-%03u", gEeprom.ScreenChannel[vfo_num] + 1);
						}

						if (gEeprom.CHANNEL_DISPLAY_MODE == MDF_NAME) {
							UI_PrintString(String, 32, 0, line, 8);
						}
						else {
							UI_PrintStringSmallBold(String, 32 + 4, 0, line);
							// show the channel frequency below the channel number/name
							sprintf(String, "%03u.%05u", frequency / 100000, frequency % 100000);
							UI_PrintStringSmallNormal(String, 32 + 4, 0, line + 1);
						}

						break;
				}
			}
			else
			{	// frequency mode
				sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);

#ifdef ENABLE_BIG_FREQ
				if(frequency < _1GHz_in_KHz) {
					// show the remaining 2 small frequency digits
					UI_PrintStringSmallNormal(String + 7, 113, 0, line + 1);
					String[7] = 0;
					// show the main large frequency digits
					UI_DisplayFrequency(String, 32, line, false);
				}
				else
#endif
				{
					// show the frequency in the main font
					UI_PrintString(String, 32, 0, line, 8);
				}

				// show the channel symbols
				const ChannelAttributes_t att = gMR_ChannelAttributes[gEeprom.ScreenChannel[vfo_num]];
				if (att.compander)
#ifdef ENABLE_BIG_FREQ
					memcpy(p_line0 + 120, BITMAP_compand, sizeof(BITMAP_compand));
#else
					memcpy(p_line0 + 120 + LCD_WIDTH, BITMAP_compand, sizeof(BITMAP_compand));
#endif

			}
		}

		// ************

		{	// show the TX/RX level
		#ifdef ENABLE_SMALL_BOLD

		#else
			uint8_t Level = 0;

			if (mode == VFO_MODE_TX)
			{	// TX power level
				switch (gRxVfo->OUTPUT_POWER)
				{
					case OUTPUT_POWER_LOW:  Level = 2; break;
					case OUTPUT_POWER_MID:  Level = 4; break;
					case OUTPUT_POWER_HIGH: Level = 6; break;
				}
			}
			else
			if (mode == VFO_MODE_RX)
			{	// RX signal level
				#ifndef ENABLE_RSSI_BAR
					// bar graph
					if (gVFO_RSSI_bar_level[vfo_num] > 0)
						Level = gVFO_RSSI_bar_level[vfo_num];
				#endif
			}
			
			if(Level)
				DrawSmallAntennaAndBars(p_line1 + LCD_WIDTH, Level);
		#endif
		}

		// ************

		String[0] = '\0';
		const VFO_Info_t *vfoInfo = &gEeprom.VfoInfo[vfo_num];

		// show the modulation symbol
		uint8_t ModPos = 0;
		const char * s = "";
		const ModulationMode_t mod = vfoInfo->Modulation;
		switch (mod){
			case MODULATION_FM: {
				const FREQ_Config_t *pConfig = (mode == VFO_MODE_TX) ? vfoInfo->pTX : vfoInfo->pRX;
				const unsigned int code_type = pConfig->CodeType;
				const char *code_list[] = {"FM", "(FM)CT", "(FM)DCS", "(FM)DCR"};
				if (code_type < ARRAY_SIZE(code_list))
					s = code_list[code_type];
				if ( code_type != 0 )
					ModPos = 12;
				break;
			}
			default:
				s = gModulationStr[mod];
			break;
		}
#ifdef ENABLE_SMALL_BOLD
		GUI_DisplaySmallest(s, 16 - ModPos, line == 0 ? 17 : 49, false, true);
#else
		UI_PrintStringSmall(s, LCD_WIDTH + 24, 0, line + 1);
#endif		

		if (state == VFO_STATE_NORMAL || state == VFO_STATE_ALARM)
		{	// show the TX power
			int i = vfoInfo->OUTPUT_POWER % 3;

#ifdef ENABLE_SMALL_BOLD
			const char *powerNames[] = {"LOW", "MID", "HIGH"};
			GUI_DisplaySmallest(powerNames[i], 34, line == 0 ? 17 : 49, false, true);
#else
			const char pwr_list[][2] = {"L","M","H"};			
			UI_PrintStringSmallNormal(pwr_list[i], LCD_WIDTH + 46, 0, line + 1);
#endif				

		}

		if (vfoInfo->freq_config_RX.Frequency != vfoInfo->freq_config_TX.Frequency)
		{	// show the TX offset symbol
			const char dir_list[] = "\0+-";
			const unsigned int i = gEeprom.VfoInfo[vfo_num].TX_OFFSET_FREQUENCY_DIRECTION;
			String[0] = (i < sizeof(dir_list)) ? dir_list[i] : '?';
			String[1] = '\0';		
#ifdef ENABLE_SMALL_BOLD
			GUI_DisplaySmallest(String, 54, line == 0 ? 17 : 49, false, true);
#else			
			UI_PrintStringSmall(String, LCD_WIDTH + 54, 0, line + 1);
#endif				
		}

		// show the TX/RX reverse symbol
		if (gEeprom.VfoInfo[vfo_num].FrequencyReverse)
#ifdef ENABLE_SMALL_BOLD			
			GUI_DisplaySmallest("R", 62, line == 0 ? 17 : 49, false, true);
#else
			UI_PrintStringSmall("R", LCD_WIDTH + 62, 0, line + 1);
#endif				

		{	// show the narrow band symbol			
#ifdef ENABLE_SMALL_BOLD
			const char *bwNames[3] = {"WIDE", "NARROW", "NARROWER"};
			GUI_DisplaySmallest(bwNames[gEeprom.VfoInfo[vfo_num].CHANNEL_BANDWIDTH], 68, line == 0 ? 17 : 49, false, true);
#else
			String[0] = '\0';
			if (gEeprom.VfoInfo[vfo_num].CHANNEL_BANDWIDTH == BANDWIDTH_NARROW)
			{
				String[0] = 'N';
				String[1] = '\0';
			}
			UI_PrintStringSmall(String, LCD_WIDTH + 70, 0, line + 1);
#endif				
		}

#ifdef ENABLE_DTMF_CALLING
		// show the DTMF decoding symbol		
		if (gEeprom.VfoInfo[vfo_num].DTMF_DECODING_ENABLE || gSetting_KILLED)
#ifdef ENABLE_SMALL_BOLD
			GUI_DisplaySmallest("DTMF", 95, line == 0 ? 17 : 49, false, true);
#else
			UI_PrintStringSmall("DTMF", LCD_WIDTH + 78, 0, line + 1);
#endif				
#endif
		// show the audio scramble symbol
		if (gEeprom.VfoInfo[vfo_num].SCRAMBLING_TYPE > 0 && gSetting_ScrambleEnable)
#ifdef ENABLE_SMALL_BOLD
			GUI_DisplaySmallest("SCR", 112, line == 0 ? 17 : 49, false, true);
#else
			UI_PrintStringSmall("SCR", LCD_WIDTH + 106, 0, line + 1);
#endif			
	}

#ifdef ENABLE_AGC_SHOW_DATA
	center_line = CENTER_LINE_IN_USE;
	UI_MAIN_PrintAGC(false);
#endif

	if (center_line == CENTER_LINE_NONE)
	{	// we're free to use the middle line

		const bool rx = FUNCTION_IsRx();

#ifdef ENABLE_AUDIO_BAR
		if (gSetting_mic_bar && gCurrentFunction == FUNCTION_TRANSMIT) {
			center_line = CENTER_LINE_AUDIO_BAR;
			UI_DisplayAudioBar();
		}
		else
#endif

#ifdef ENABLE_RSSI_BAR
		if (rx) {
			center_line = CENTER_LINE_RSSI;
			DisplayRSSIBar(false);
		}
		else
#endif
		if (rx || gCurrentFunction == FUNCTION_FOREGROUND || gCurrentFunction == FUNCTION_POWER_SAVE)
		{
			#if 1
				if (gSetting_live_DTMF_decoder && gDTMF_RX_live[0] != 0)
				{	// show live DTMF decode
					const unsigned int len = strlen(gDTMF_RX_live);
					const unsigned int idx = (len > (17 - 5)) ? len - (17 - 5) : 0;  // limit to last 'n' chars

					if (gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
						|| gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
						)
						return;

					center_line = CENTER_LINE_DTMF_DEC;

					sprintf(String, "DTMF %s", gDTMF_RX_live + idx);
					UI_PrintStringSmallNormal(String, 2, 0, 3);
				}
			#else
				if (gSetting_live_DTMF_decoder && gDTMF_RX_index > 0)
				{	// show live DTMF decode
					const unsigned int len = gDTMF_RX_index;
					const unsigned int idx = (len > (17 - 5)) ? len - (17 - 5) : 0;  // limit to last 'n' chars

					if (gScreenToDisplay != DISPLAY_MAIN ||
						gDTMF_CallState != DTMF_CALL_STATE_NONE)
						return;

					center_line = CENTER_LINE_DTMF_DEC;

					sprintf(String, "DTMF %s", gDTMF_RX_live + idx);
					UI_PrintStringSmallNormal(String, 2, 0, 3);
				}
			#endif

#ifdef ENABLE_SHOW_CHARGE_LEVEL
			else if (gChargingWithTypeC)
			{	// charging .. show the battery state
				if (gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
					|| gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
					)
					return;

				center_line = CENTER_LINE_CHARGE_DATA;

				sprintf(String, "Charge %u.%02uV %u%%",
					gBatteryVoltageAverage / 100, gBatteryVoltageAverage % 100,
					BATTERY_VoltsToPercent(gBatteryVoltageAverage));
				UI_PrintStringSmallNormal(String, 2, 0, 3);
			}
#endif
		}
	}

	ST7565_BlitFullScreen();
}

// ***************************************************************************
