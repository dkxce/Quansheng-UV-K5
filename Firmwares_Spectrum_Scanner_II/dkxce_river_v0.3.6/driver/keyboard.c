/* Copyright 2023 Manuel Jinger
 * Copyright 2023 Dual Tachyon
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

#include "bsp/dp32g030/gpio.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "driver/systick.h"
#include "driver/i2c.h"
#include "misc.h"

#ifdef ENABLE_SCREEN_DUMP
	KEY_Code_t gSimulateKey     = KEY_INVALID;
	KEY_Code_t gSimulateHold     = KEY_INVALID;
	uint8_t gDebounceDefeat = 0;
	uint8_t gPttCounter = 0;
#endif

KEY_Code_t gKeyReading0     = KEY_INVALID;
KEY_Code_t gKeyReading1     = KEY_INVALID;
uint16_t   gDebounceCounter = 0;
bool       gWasFKeyPressed  = false;

static const struct {

	// Using a 16 bit pre-calculated shift and invert is cheaper
	// than using 8 bit and doing shift and invert in code.
	uint16_t set_to_zero_mask;

	// We are very fortunate.
	// The key and pin defines fit together in a single u8, making this very efficient
	struct {
		KEY_Code_t key : 5;
		uint8_t    pin : 3; // Pin 6 is highest
	} pins[4];

} keyboard[] = {

	{	// Zero row
		// Set to zero to handle special case of nothing pulled down
		.set_to_zero_mask = 0xffff,
		.pins = {
			{ .key = KEY_SIDE1,   .pin = GPIOA_PIN_KEYBOARD_0},
			{ .key = KEY_SIDE2,   .pin = GPIOA_PIN_KEYBOARD_1},

			// Duplicate to fill the array with valid values
			{ .key = KEY_INVALID, .pin = GPIOA_PIN_KEYBOARD_1},
			{ .key = KEY_INVALID, .pin = GPIOA_PIN_KEYBOARD_1}
		}
	},
	{	// First row
		.set_to_zero_mask = ~(1u << GPIOA_PIN_KEYBOARD_4) & 0xffff,
		.pins = {
			{ .key = KEY_MENU,  .pin = GPIOA_PIN_KEYBOARD_0},
			{ .key = KEY_1,     .pin = GPIOA_PIN_KEYBOARD_1},
			{ .key = KEY_4,     .pin = GPIOA_PIN_KEYBOARD_2},
			{ .key = KEY_7,     .pin = GPIOA_PIN_KEYBOARD_3}
		}
	},
	{	// Second row
		.set_to_zero_mask = ~(1u << GPIOA_PIN_KEYBOARD_5) & 0xffff,
		.pins = {
			{ .key = KEY_UP,    .pin = GPIOA_PIN_KEYBOARD_0},
			{ .key = KEY_2 ,    .pin = GPIOA_PIN_KEYBOARD_1},
			{ .key = KEY_5 ,    .pin = GPIOA_PIN_KEYBOARD_2},
			{ .key = KEY_8 ,    .pin = GPIOA_PIN_KEYBOARD_3}
		}
	},
	{	// Third row
		.set_to_zero_mask = ~(1u << GPIOA_PIN_KEYBOARD_6) & 0xffff,
		.pins = {
			{ .key = KEY_DOWN,  .pin = GPIOA_PIN_KEYBOARD_0},
			{ .key = KEY_3   ,  .pin = GPIOA_PIN_KEYBOARD_1},
			{ .key = KEY_6   ,  .pin = GPIOA_PIN_KEYBOARD_2},
			{ .key = KEY_9   ,  .pin = GPIOA_PIN_KEYBOARD_3}
		}
	},
	{	// Fourth row
		.set_to_zero_mask = ~(1u << GPIOA_PIN_KEYBOARD_7) & 0xffff,
		.pins = {
			{ .key = KEY_EXIT,  .pin = GPIOA_PIN_KEYBOARD_0},
			{ .key = KEY_STAR,  .pin = GPIOA_PIN_KEYBOARD_1},
			{ .key = KEY_0   ,  .pin = GPIOA_PIN_KEYBOARD_2},
			{ .key = KEY_F   ,  .pin = GPIOA_PIN_KEYBOARD_3}
		}
	}
};

KEY_Code_t KEYBOARD_Poll(void)
{
	#ifdef ENABLE_SCREEN_DUMP
		if(gSimulateKey != KEY_INVALID)
		{
			const KEY_Code_t temp = gSimulateKey;
			if(gDebounceDefeat++ >= 5)
				gSimulateKey = KEY_INVALID;
			return temp;		
		}
		if(gSimulateHold != KEY_INVALID)
		{
			if(gSimulateHold == KEY_PTT)
			{
				if(gPttCounter == 0)
					gSimulateHold = KEY_INVALID;
				else
					gPttCounter--;
			}
			return gSimulateHold;
		}
	#endif
	
	KEY_Code_t Key = KEY_INVALID;

//	if (!GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT))
//		return KEY_PTT;

	// *****************

	for (unsigned int j = 0; j < ARRAY_SIZE(keyboard); j++)
	{
		uint16_t reg;
		unsigned int i;
		unsigned int k;

		// Set all high
		GPIOA->DATA |=  1u << GPIOA_PIN_KEYBOARD_4 |
						1u << GPIOA_PIN_KEYBOARD_5 |
						1u << GPIOA_PIN_KEYBOARD_6 |
						1u << GPIOA_PIN_KEYBOARD_7;

		// Clear the pin we are selecting
		GPIOA->DATA &= keyboard[j].set_to_zero_mask;

		// Read all 4 GPIO pins at once .. with de-noise, max of 8 sample loops
		for (i = 0, k = 0, reg = 0; i < 3 && k < 8; i++, k++) {
			SYSTICK_DelayUs(1);
			uint16_t reg2 = GPIOA->DATA;
			i *= reg == reg2;
			reg = reg2;
		}

		if (i < 3)
			break;	// noise is too bad

		for (unsigned int i = 0; i < ARRAY_SIZE(keyboard[j].pins); i++)
		{
			const uint16_t mask = 1u << keyboard[j].pins[i].pin;
			if (!(reg & mask))
			{
				Key = keyboard[j].pins[i].key;
				break;
			}
		}

		if (Key != KEY_INVALID)
			break;
	}

	// Create I2C stop condition since we might have toggled I2C pins
	// This leaves GPIOA_PIN_KEYBOARD_4 and GPIOA_PIN_KEYBOARD_5 high
	I2C_Stop();

	// Reset VOICE pins
	GPIO_ClearBit(&GPIOA->DATA, GPIOA_PIN_KEYBOARD_6);
	GPIO_SetBit(  &GPIOA->DATA, GPIOA_PIN_KEYBOARD_7);

	return Key;
}
