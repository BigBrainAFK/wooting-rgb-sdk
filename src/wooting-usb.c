/*
* Copyright 2018 Wooting Technologies B.V.
*
* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/
#include "stdint.h"
#include "stdbool.h"
#include "string.h"
#include "wooting-usb.h"
#include "hidapi.h"

#define WOOTING_COMMAND_SIZE 8
#define WOOTING_REPORT_SIZE 129
#define WOOTING_VID 0x03EB
#define WOOTING_ONE_PID 0xFF01
#define WOOTING_TWO_PID 0xFF02

static WOOTING_USB_META wooting_usb_meta;

uint8_t key_code_limit = WOOTING_TWO_KEY_CODE_LIMIT;

static void_cb disconnected_callback = NULL;
static hid_device* keyboard_handle = NULL;

static uint16_t getCrc16ccitt(const uint8_t* buffer, uint16_t size)
{
	uint16_t crc = 0;

	while (size--) {
		crc ^= (*buffer++ << 8);

		for (uint8_t i = 0; i < 8; ++i) {
			if (crc & 0x8000) {
				crc = (crc << 1) ^ 0x1021;
			}
			else {
				crc = crc << 1;
			}
		}
	}

	return crc;
}


typedef void (*set_meta_func)();

static void reset_meta() {
	wooting_usb_meta.connected = false;

	wooting_usb_meta.model = "N/A";
	wooting_usb_meta.device_type = -1;
	wooting_usb_meta.led_index_max = 0;
	wooting_usb_meta.max_rows = 0;
	wooting_usb_meta.max_columns = 0;
	wooting_usb_meta.led_index_max = 0;
}


static void set_meta_wooting_one() {
	wooting_usb_meta.model = "Wooting one";
	wooting_usb_meta.device_type = DEVICE_KEYBOARD_TKL;
	wooting_usb_meta.led_index_max = WOOTING_ONE_KEY_CODE_LIMIT;
	wooting_usb_meta.max_rows = WOOTING_RGB_ROWS;
	wooting_usb_meta.max_columns = WOOTING_ONE_RGB_COLS;
	wooting_usb_meta.led_index_max = WOOTING_ONE_KEY_CODE_LIMIT;
}

static void set_meta_wooting_two() {
	wooting_usb_meta.model = "Wooting two";
	wooting_usb_meta.device_type = DEVICE_KEYBOARD;
	wooting_usb_meta.led_index_max = WOOTING_TWO_KEY_CODE_LIMIT;
	wooting_usb_meta.max_rows = WOOTING_RGB_ROWS;
	wooting_usb_meta.max_columns = WOOTING_TWO_RGB_COLS;
	wooting_usb_meta.led_index_max = WOOTING_TWO_KEY_CODE_LIMIT;
}

WOOTING_USB_META* wooting_usb_get_meta() {
	//We want to initialise the struct to the default values if it hasn't been set
	if (wooting_usb_meta.model == NULL){
		reset_meta();
	}

	return &wooting_usb_meta;
}

void wooting_usb_disconnect(bool trigger_cb) {
	#ifdef DEBUG_LOG
	printf("Keyboard disconnected\n");
	#endif
	reset_meta();
	hid_close(keyboard_handle);
	keyboard_handle = NULL;

	if (trigger_cb && disconnected_callback) {
		disconnected_callback();
	}
}

void wooting_usb_set_disconnected_cb(void_cb cb) {
	disconnected_callback = cb;
}

bool wooting_usb_find_keyboard() {
	if (keyboard_handle) {
		#ifdef DEBUG_LOG
		printf("Got keyboard handle already\n");
		#endif
		// If keyboard is disconnected read will return -1
		// https://github.com/signal11/hidapi/issues/55#issuecomment-5307209
		unsigned char stub = 0;
		if (hid_read_timeout(keyboard_handle, &stub, 0, 0) != -1) {
			#ifdef DEBUG_LOG
			printf("Keyboard succeeded test read\n");
			#endif
			return true;
		} else {
			#ifdef DEBUG_LOG
			printf("Keyboard failed test read, disconnecting...\n");
			#endif
			wooting_usb_disconnect(true);
		}
	} else {
		#ifdef DEBUG_LOG
		printf("No keyboard handle already\n");
		#endif
	}
	
	struct hid_device_info* hid_info;

	reset_meta();
	set_meta_func meta_func;
	if ((hid_info = hid_enumerate(WOOTING_VID, WOOTING_ONE_PID)) != NULL) {
		#ifdef DEBUG_LOG
		printf("Enumerate on Wooting One Successful\n");
		#endif
		meta_func = set_meta_wooting_one;
	}
	else if ((hid_info = hid_enumerate(WOOTING_VID, WOOTING_TWO_PID)) != NULL) {
		#ifdef DEBUG_LOG
		printf("Enumerate on Wooting Two Successful\n");
		#endif
		meta_func = set_meta_wooting_two;
	}
	else {
		#ifdef DEBUG_LOG
		printf("Enumerate failed\n");
		#endif
		return false;
	}

	bool keyboard_found = false;

	// The amount of interfaces is variable, so we need to look for the configuration interface
	// In the Wooting one keyboard the configuration interface is always 4 lower than the highest number
	struct hid_device_info* hid_info_walker = hid_info;
	uint8_t highestInterfaceNr = 0;
	while (hid_info_walker) {
		if (hid_info_walker->interface_number > highestInterfaceNr) {
			highestInterfaceNr = hid_info_walker->interface_number;
		}
		hid_info_walker = hid_info_walker->next;
	}

	uint8_t interfaceNr = highestInterfaceNr - 4;

	#ifdef DEBUG_LOG
	printf("Higest Interface No: %d, Search interface No: %d\n", highestInterfaceNr, interfaceNr);
	#endif

	// Reset walker and look for the interface number
	hid_info_walker = hid_info;
	while (hid_info_walker) {
		#ifdef DEBUG_LOG
		printf("Found interface No: %d\n", hid_info_walker->interface_number);
		#endif
		if (hid_info_walker->interface_number == interfaceNr) {
			#ifdef DEBUG_LOG
			printf("Attempting to open\n");
			#endif
			keyboard_handle = hid_open_path(hid_info_walker->path);
			if (keyboard_handle) {
				#ifdef DEBUG_LOG
				printf("Found keyboard_handle: %s\n", hid_info_walker->path);
				#endif
				// Once the keyboard is found send an init command and abuse two reads to make a 50 ms delay to make sure the keyboard is ready
				#ifdef DEBUG_LOG
				bool result = 
				#endif
				wooting_usb_send_feature(WOOTING_COLOR_INIT_COMMAND, 0, 0, 0, 0);
				#ifdef DEBUG_LOG
				printf("Color init result: %d\n", result);
				#endif
				unsigned char stub = 0;
				hid_read(keyboard_handle, &stub, 0);
				hid_read_timeout(keyboard_handle, &stub, 0, 50);

				keyboard_found = true;
			} else {
				#ifdef DEBUG_LOG
				printf("No Keyboard handle: %S\n", hid_error(NULL));
				#endif
			}
			break;
		}
	
		hid_info_walker = hid_info_walker->next;
	}

	hid_free_enumeration(hid_info);
	if (keyboard_found){
		meta_func();
		wooting_usb_meta.connected = true;
	}
	#ifdef DEBUG_LOG
	printf("Finished looking for keyboard returned: %d\n", keyboard_found);
	#endif
	return keyboard_found;
}

bool wooting_usb_send_buffer(RGB_PARTS part_number, uint8_t rgb_buffer[]) {
	if (!wooting_usb_find_keyboard()) {
		return false;
	}

	uint8_t report_buffer[WOOTING_REPORT_SIZE] = { 0 };

	report_buffer[0] = 0; // HID report index (unused)
	report_buffer[1] = 0xD0; // Magicword
	report_buffer[2] = 0xDA; // Magicword
	report_buffer[3] = WOOTING_RAW_COLORS_REPORT; // Report ID
	switch (part_number) {
		case PART0: {
			report_buffer[4] = 0; // Slave nr
			report_buffer[5] = 0; // Reg start address
			break;
		}
		case PART1: {
			report_buffer[4] = 0; // Slave nr
			report_buffer[5] = RGB_RAW_BUFFER_SIZE; // Reg start address
			break;
		}
		case PART2: {
			report_buffer[4] = 1; // Slave nr
			report_buffer[5] = 0; // Reg start address
			break;
		}
		case PART3: {
			report_buffer[4] = 1; // Slave nr
			report_buffer[5] = RGB_RAW_BUFFER_SIZE; // Reg start address
			break;
		}
		// wooting_rgb_array_update_keyboard will not run into this
		case PART4: {
			//Wooting One will not have this part of the report
			if (wooting_usb_meta.device_type != DEVICE_KEYBOARD) {
				return false;
			}
			report_buffer[4] = 2; // Slave nr
			report_buffer[5] = 0; // Reg start address
			break;
		}
		default: {
			return false;
		}
	}

	memcpy(&report_buffer[6], rgb_buffer, RGB_RAW_BUFFER_SIZE);

	uint16_t crc = getCrc16ccitt((uint8_t*)&report_buffer, WOOTING_REPORT_SIZE - 2);
	report_buffer[127] = (uint8_t)crc;
	report_buffer[128] = crc >> 8;
	int report_size = hid_write(keyboard_handle, report_buffer, WOOTING_REPORT_SIZE);
	if (report_size == WOOTING_REPORT_SIZE) {
		return true;
	}
	else {
		#ifdef DEBUG_LOG
		printf("Got report size: %d, expected: %d, disconnecting..\n", report_size, WOOTING_REPORT_SIZE);
		#endif
		wooting_usb_disconnect(true);
		return false;
	}
}

bool wooting_usb_send_feature(uint8_t commandId, uint8_t parameter0, uint8_t parameter1, uint8_t parameter2, uint8_t parameter3) {
	if (!wooting_usb_find_keyboard()) {
		return false;
	}
	// prevent sending unnecessary data to the wootings if the index exceedes it's capabilities
	if ((commandId == WOOTING_SINGLE_COLOR_COMMAND && parameter0 > key_code_limit)
	|| (commandId == WOOTING_SINGLE_RESET_COMMAND && parameter3 > key_code_limit)) {
		// this is not a usb error so let's return true. wooting_rgb_direct_set_key would also behave differently otherwise.
		return true;
	}

	uint8_t report_buffer[WOOTING_COMMAND_SIZE];

	report_buffer[0] = 0; // HID report index (unused)
	report_buffer[1] = 0xD0; // Magic word
	report_buffer[2] = 0xDA; // Magic word
	report_buffer[3] = commandId;
	report_buffer[4] = parameter3;
	report_buffer[5] = parameter2;
	report_buffer[6] = parameter1;
	report_buffer[7] = parameter0;
	int command_size = hid_send_feature_report(keyboard_handle, report_buffer, WOOTING_COMMAND_SIZE);
	if (command_size == WOOTING_COMMAND_SIZE) {
		return true;
	}
	else {
		#ifdef DEBUG_LOG
		printf("Got command size: %d, expected: %d, disconnecting..\n", command_size, WOOTING_COMMAND_SIZE);
		#endif

		wooting_usb_disconnect(true);
		return false;
	}
}
