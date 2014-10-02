/*
 * brickd
 * Copyright (C) 2014 Olaf Lüke <olaf@tinkerforge.com>
 *
 * red_extension.c: Extension initialization for RED Brick
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */


#include "red_extension.h"

#include "red_rs485_extension.h"
#include "red_ethernet_extension.h"

#include <daemonlib/red_i2c_eeprom.h>
#include <daemonlib/red_gpio.h>
#include <daemonlib/log.h>

#define LOG_CATEGORY LOG_CATEGORY_RED_BRICK

#define EXTENSION_NUM_MAX 2
#define EXTENSION_EEPROM_TYPE_LOCATION 0
#define EXTENSION_EEPROM_TYPE_SIZE 4


#define EXTENSION_EEPROM_RS485_ADDRESS_LOCATION                  4
#define EXTENSION_EEPROM_RS485_SLAVE_ADDRESSES_START_LOCATION    100
#define EXTENSION_EEPROM_RS485_BAUDRATE_LOCATION                 400
#define EXTENSION_EEPROM_RS485_PARTIY_LOCATION                   404
#define EXTENSION_EEPROM_RS485_STOPBITS_LOCATION                 405

#define EXTENSION_EEPROM_ETHERNET_MAC_ADDRESS                    (32*4)

typedef enum  {
	EXTENSION_TYPE_NONE = 0,
	EXTENSION_TYPE_CHIBI = 1,
	EXTENSION_TYPE_RS485 = 2,
	EXTENSION_TYPE_WIFI = 3,
	EXTENSION_TYPE_ETHERNET = 4
} ExtensionType;


#define EXTENSION_POS0_GPIO0  {GPIO_PORT_B, GPIO_PIN_13}
#define EXTENSION_POS0_GPIO1  {GPIO_PORT_B, GPIO_PIN_14}
#define EXTENSION_POS0_GPIO2  {GPIO_PORT_B, GPIO_PIN_19}
#define EXTENSION_POS0_SELECT {GPIO_PORT_G, GPIO_PIN_9}

#define EXTENSION_POS1_GPIO0  {GPIO_PORT_G, GPIO_PIN_2}
#define EXTENSION_POS1_GPIO1  {GPIO_PORT_G, GPIO_PIN_3}
#define EXTENSION_POS1_GPIO2  {GPIO_PORT_G, GPIO_PIN_4}
#define EXTENSION_POS1_SELECT {GPIO_PORT_G, GPIO_PIN_13}

#define EXTENSION_SPI_CLK     {GPIO_PORT_G, GPIO_PIN_10}
#define EXTENSION_SPI_MOSI    {GPIO_PORT_G, GPIO_PIN_11}
#define EXTENSION_SPI_MISO    {GPIO_PORT_G, GPIO_PIN_12}

#define EXTENSION_SER_TXD     {GPIO_PORT_C, GPIO_PIN_16}
#define EXTENSION_SER_RXD     {GPIO_PORT_C, GPIO_PIN_17}
#define EXTENSION_SER_RTS     {GPIO_PORT_C, GPIO_PIN_19}

typedef struct {
	GPIOPin pin[2];
	GPIOMux mux;
	int value;   // If input: 0 = default, 1 = Pullup. If Output: 0 = low, 1 = high. Else ignored.
} ExtensionPinConfiguration;

typedef struct {
	int num_configs;
	ExtensionPinConfiguration config[];
} ExtensionPinConfigurationArray;

static ExtensionPinConfigurationArray extension_startup = {1, {
	{{EXTENSION_POS0_SELECT, EXTENSION_POS1_SELECT}, GPIO_MUX_OUTPUT, 0}, // Deselect eeprom
}};

static ExtensionPinConfigurationArray extension_rs485_pin_config = {7, {
	{{EXTENSION_POS0_GPIO0,  EXTENSION_POS1_GPIO0},  GPIO_MUX_OUTPUT, 0}, // RXE low = RX enable
	{{EXTENSION_POS0_GPIO1,  EXTENSION_POS1_GPIO1},  GPIO_MUX_INPUT,  1}, // Unused
	{{EXTENSION_POS0_GPIO2,  EXTENSION_POS1_GPIO2},  GPIO_MUX_INPUT,  1}, // Unused
	{{EXTENSION_POS0_SELECT, EXTENSION_POS1_SELECT}, GPIO_MUX_OUTPUT, 0}, // Default = deselect eeprom
	{{EXTENSION_SER_TXD,     EXTENSION_SER_TXD},     GPIO_MUX_4,      0}, // Mux to UART3_TX
	{{EXTENSION_SER_RXD,     EXTENSION_SER_RXD},     GPIO_MUX_4,      0}, // Mux to UART3_RX
	{{EXTENSION_SER_RTS,     EXTENSION_SER_RTS},     GPIO_MUX_4,      0}, // Mux to UART3_RTS
}};

static ExtensionPinConfigurationArray extension_ethernet_pin_config = {7, {
	{{EXTENSION_POS0_GPIO0,  EXTENSION_POS1_GPIO0},  GPIO_MUX_OUTPUT, 1}, // nRESET = high
	{{EXTENSION_POS0_GPIO1,  EXTENSION_POS1_GPIO1},  GPIO_MUX_6,      0}, // Mux to EINT3/EINT28
	{{EXTENSION_POS0_GPIO2,  EXTENSION_POS1_GPIO2},  GPIO_MUX_OUTPUT, 0}, // PWDN = low
	{{EXTENSION_POS0_SELECT, EXTENSION_POS1_SELECT}, GPIO_MUX_2,      0}, // Mux to SPI1_CS0
	{{EXTENSION_SPI_CLK,     EXTENSION_SPI_CLK},     GPIO_MUX_2,      0}, // Mux to SPI1_CLK
	{{EXTENSION_SPI_MOSI,    EXTENSION_SPI_MOSI},    GPIO_MUX_2,      0}, // Mux to SPI1_MOSI
	{{EXTENSION_SPI_MISO,    EXTENSION_SPI_MISO},    GPIO_MUX_2,      0}, // Mux to SPI1_MISO
}};

// Discovered extension types (for both extensions)
static ExtensionType _red_extension_type[EXTENSION_NUM_MAX] = {EXTENSION_TYPE_NONE, EXTENSION_TYPE_NONE};


static void red_extension_configure_pin(ExtensionPinConfiguration *config, int extension) {
	gpio_mux_configure(config->pin[extension], config->mux);
	if(config->value == 0) {
		gpio_output_clear(config->pin[extension]);
	} else {
		gpio_output_set(config->pin[extension]); // This should enable pull-up in case of input
	}
}

int red_extension_read_rs485_config(I2CEEPROM *i2c_eeprom, ExtensionRS485Config *rs485_config) {
	uint8_t buf[4];

	// Config: ADDRESS
	if(i2c_eeprom_read(i2c_eeprom, EXTENSION_EEPROM_RS485_ADDRESS_LOCATION, buf, 4) < 4) {
		log_error("RS485: Could not read config ADDRESS from EEPROM");
		return -1;
	}

	rs485_config->address = (buf[0] << 0) | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);

	// Config: BAUDRATE
	if(i2c_eeprom_read(i2c_eeprom, EXTENSION_EEPROM_RS485_BAUDRATE_LOCATION, buf, 4) < 4) {
		log_error("RS485: Could not read config BAUDRATE from EEPROM");
		return -1;
	}

	rs485_config->baudrate = (buf[0] << 0) | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);

	if(rs485_config->baudrate < 8) {
		log_error("RS485: Configured baudrate is too low");
		return -1;
	}

	// Config: PARITY
	if(i2c_eeprom_read(i2c_eeprom, EXTENSION_EEPROM_RS485_PARTIY_LOCATION, buf, 1) < 1) {
		log_error("RS485: Could not read config PARITY from EEPROM");
		return -1;
	}

	if(buf[0] == RS485_EXTENSION_SERIAL_PARITY_NONE) {
		rs485_config->parity = RS485_EXTENSION_SERIAL_PARITY_NONE;
	} else if(buf[0] == RS485_EXTENSION_SERIAL_PARITY_EVEN){
		rs485_config->parity = RS485_EXTENSION_SERIAL_PARITY_EVEN;
	} else {
		rs485_config->parity = RS485_EXTENSION_SERIAL_PARITY_ODD;
	}

	// Config: STOPBITS
	if(i2c_eeprom_read(i2c_eeprom, EXTENSION_EEPROM_RS485_STOPBITS_LOCATION, buf, 1) < 1) {
		log_error("RS485: Could not read config STOPBITS from EEPROM");
		return -1;
	}
	rs485_config->stopbits = buf[0];

	// Config (if master): SLAVE ADDRESSES
	if(rs485_config->address == 0) {
		rs485_config->slave_num = 0;
		uint16_t current_eeprom_location = EXTENSION_EEPROM_RS485_SLAVE_ADDRESSES_START_LOCATION;
		uint32_t current_slave_address;

		while(rs485_config->slave_num < EXTENSION_RS485_SLAVES_MAX) {
			// Config: SLAVE ADDRESS
			if(i2c_eeprom_read(i2c_eeprom, current_eeprom_location, buf, 4) < 4) {
				log_error("RS485: Could not read config SLAVE ADDRESSES from EEPROM");
				return -1;
			}

			current_slave_address = (buf[0] << 0) | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);

			if(current_slave_address == 0) {
				break;
			}

			rs485_config->slave_address[rs485_config->slave_num] = current_slave_address;
			rs485_config->slave_num++;
			current_eeprom_location += 4;
		}
	} else {
		log_error("RS485: Only master mode supported");
		return -1;
	}

	return 0;
}

int red_extension_read_ethernet_config(I2CEEPROM *i2c_eeprom, ExtensionEthernetConfig *ethernet_config) {
	if(i2c_eeprom_read(i2c_eeprom,
	                   EXTENSION_EEPROM_ETHERNET_MAC_ADDRESS,
	                   ethernet_config->mac,
	                   EXTENSION_ETHERNET_MAC_SIZE) < EXTENSION_ETHERNET_MAC_SIZE) {
		log_warn("Can't read MAC adress, using default address");
		ethernet_config->mac[0] = 0x40;
		ethernet_config->mac[1] = 0xD8;
		ethernet_config->mac[2] = 0x55;
		ethernet_config->mac[3] = 0x02;
		ethernet_config->mac[4] = 0xA1;
		ethernet_config->mac[5] = 0x00;

		return -1;
	}

	return 0;
}

int red_extension_init(void) {
    uint8_t buf[4];
    int i, j, ret;
    ExtensionBaseConfig base_config[2];

    // First we remove the Ethernet Extension kernel module (if there is one)
    // to make sure that there isn't a collision between SPI select and I2C select.
    red_ethernet_extension_rmmod();

    // Then we deselect all EEPROMS
    for(i = 0; i < EXTENSION_NUM_MAX; i++) {
		for(j = 0; j < extension_startup.num_configs; j++) {
			red_extension_configure_pin(&extension_startup.config[j], i);
		}
    }

    // Now we can try to find the configurations
    for(i = 0; i < EXTENSION_NUM_MAX; i++) {
    	I2CEEPROM i2c_eeprom;
    	log_debug("Checking for presence of Extension at position %d", i);

		base_config[i].extension = i;
		base_config[i].type = EXTENSION_TYPE_NONE;

    	if(i2c_eeprom_init(&i2c_eeprom, i) < 0) {
    		return -1;
    	}

		if(i2c_eeprom_read(&i2c_eeprom,
		                   EXTENSION_EEPROM_TYPE_LOCATION,
		                   buf,
		                   EXTENSION_EEPROM_TYPE_SIZE) < EXTENSION_EEPROM_TYPE_SIZE) {
			log_info("Could not find Extension at position %d", i);
			continue;
		}

		base_config[i].type = (buf[0] << 0) | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);

		// If there is an extension that is either not configured (Extension type NONE)
		// Or that we currently don't support (WIFI), we will log it, but try to
		// continue finding extensions. We can support an extension at position 1 if
		// there is an unsupported extension at position 0.
		if(base_config[i].type == EXTENSION_TYPE_NONE) {
			log_warn("Could not find Extension at position %d (Type None)", i);
			continue;
		}

		if((base_config[i].type != EXTENSION_TYPE_ETHERNET) && (base_config[i].type != EXTENSION_TYPE_RS485)) {
			log_warn("Extension at position %d not supported (type %d)", i, base_config[i].type);
			continue;
		}

		switch(base_config[i].type) {
			case EXTENSION_TYPE_RS485:
				ret = red_extension_read_rs485_config(&i2c_eeprom, (ExtensionRS485Config *) &base_config[i]);
				i2c_eeprom_release(&i2c_eeprom);

				if(ret < 0) {
					log_warn("Could not read RS485 config, ignoring extension at position %d", i);
					continue;
				}

				break;

			case EXTENSION_TYPE_ETHERNET:
				ret = red_extension_read_ethernet_config(&i2c_eeprom, (ExtensionEthernetConfig *) &base_config[i]);
				i2c_eeprom_release(&i2c_eeprom);

				if(ret < 0) {
					log_warn("Could not read Ethernet config, ignoring extension at position %d", i);
					continue;
				}

				break;
		}
    }

    // Configure the pins and initialize extensions
    for(i = 0; i < EXTENSION_NUM_MAX; i++) {
    	switch(base_config[i].type) {
			case EXTENSION_TYPE_RS485:
				log_info("Found RS485 Extension at position %d", i);

				for(j = 0; j < extension_rs485_pin_config.num_configs; j++) {
					red_extension_configure_pin(&extension_rs485_pin_config.config[j], i);
				}

				if(red_rs485_extension_init((ExtensionRS485Config *) &base_config[i]) < 0) {
					continue;
				}

				_red_extension_type[i] = EXTENSION_TYPE_RS485;
				break;

			case EXTENSION_TYPE_ETHERNET:
				log_info("Found Ethernet Extension at position %d", i);

				for(j = 0; j < extension_ethernet_pin_config.num_configs; j++) {
					red_extension_configure_pin(&extension_ethernet_pin_config.config[j], i);
				}

				if(red_ethernet_extension_init((ExtensionEthernetConfig *) &base_config[i]) < 0) {
					continue;
				}

				_red_extension_type[i] = EXTENSION_TYPE_ETHERNET;
				break;
    	}
    }

    return 0;
}

void red_extension_exit(void) {
    int i;

    for(i = 0; i < EXTENSION_NUM_MAX; i++) {
		switch(_red_extension_type[i]) {
			case EXTENSION_TYPE_RS485:
				red_rs485_extension_exit();
				break;

			case EXTENSION_TYPE_ETHERNET:
				red_ethernet_extension_exit();
				break;

			default:
				break; // Nothing to do here
		}

		_red_extension_type[i] = EXTENSION_TYPE_NONE;
    }
}
