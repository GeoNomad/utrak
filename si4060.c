/*
 * si4060 software library
 *
 * API description: see EZRadioPRO-API-v1.1.2.zip
 *
 * Stefan Biereigel
 *
 */

#include <inttypes.h>
#include <msp430.h>	/* for __delay_cycles() */
#include "hw.h"
#include "spi.h"
#include "si4060.h"
#include "main.h"	/* for GPIO constants */

/*
 * si4060_read_cmd_buf
 *
 * reads the Si4060 command buffer from via SPI
 *
 * deselect: 	whether to deselect the slave after reading the response or not.
 * 		any command reading subsequent bytes after the CTS should use this
 * 		function to get CTS and continue doing spi_read afterwards
 * 		and finally deselecting the slave.
 *
 * returns:	the value of the first command buffer byte (i.e. CTS or not)
 */
uint8_t si4060_read_cmd_buf(uint8_t deselect) {
	uint8_t ret;
	spi_select();
	spi_write(CMD_READ_CMD_BUF);
	ret = spi_read();
	if (deselect) {
		spi_deselect();
	}
	return ret;
}

/*
 * si4060_get_cts
 *
 * waits for a CTS from the Si4060, includes a "time out"
 *
 * read_response: do not deselect the slave, if you want to read from it afterwards
 */
uint8_t si4060_get_cts(uint8_t read_response) {
	uint8_t temp = 0;
	uint8_t timeout = 0;
	if (!read_response) {
		while (si4060_read_cmd_buf(1) != 0xff && timeout < SI_TIMEOUT) {
			//timeout++;
		}
	} else {
		while (temp != 0xff && timeout < SI_TIMEOUT) {
			//timeout++;
			temp = si4060_read_cmd_buf(0);
			if (temp != 0xff) {
				spi_deselect();
			}
		}
	}
	return 0;

}

/*
 * si4060_shutdown
 *
 * makes the Si4060 go to shutdown state.
 * all register content is lost.
 */
void si4060_shutdown(void) {
	P1OUT |= SI_SHDN;
	/* wait 10us */
	__delay_cycles(50000);
}

/*
 * si4060_wakeup
 *
 * wakes up the Si4060 from shutdown state.
 * si4060_power_up and si4060_setup have to be called afterwards
 */
void si4060_wakeup(void) {
	P1OUT &= ~SI_SHDN;
	/* wait 20ms */
	__delay_cycles(50000);
	si4060_get_cts(0);
}

/*
 * si4060_reset
 *
 * cleanly does the POR as specified in datasheet
 */
void si4060_reset(void) {
	si4060_shutdown();
	si4060_wakeup();
}

/*
 * si4060_power_up
 *
 * powers up the Si4060 by issuing the POWER_UP command
 *
 * warning: 	the si4060 can lock after issuing this command if input clock
 * 		is not available for the internal RC oscillator calibration.
 */
void si4060_power_up(void) {
	/* wait for CTS */
	si4060_get_cts(0);
	spi_select();
	spi_write(CMD_POWER_UP);
	__delay_cycles(10000);
	spi_write(FUNC);
#ifdef USE_TCXO
	spi_write(TCXO);
#else
	spi_write(0);			/* TCXO if used */
#endif
	spi_write((uint8_t) (XO_FREQ >> 24));
	spi_write((uint8_t) (XO_FREQ >> 16));
	spi_write((uint8_t) (XO_FREQ >> 8));
	spi_write((uint8_t) XO_FREQ);
	spi_deselect();
	/* wait for CTS */
	si4060_get_cts(0);
}

/*
 * si4060_change_state
 *
 * changes the internal state machine state of the Si4060
 */
void si4060_change_state(uint8_t state) {
	si4060_get_cts(0);
	spi_select();
	spi_write(CMD_CHANGE_STATE);
	spi_write(state);
	spi_deselect();

}

/*
 * si4060_nop
 *
 * implements the NOP command on the Si4060
 */
void si4060_nop(void) {
	spi_select();
	spi_write(CMD_NOP);
	spi_deselect();
	si4060_get_cts(0);
}

/*
 * si4060_set_property_8
 *
 * sets an 8 bit (1 byte) property in the Si4060
 *
 * group:	the group number of the property
 * prop:	the number (index) of the property
 * val:		the value to set
 */
inline void si4060_set_property_8(uint8_t group, uint8_t prop, uint8_t val) {
	si4060_get_cts(0);
	spi_select();
	spi_write(CMD_SET_PROPERTY);
	spi_write(group);
	spi_write(1);
	spi_write(prop);
	spi_write(val);
	spi_deselect();
}

/*
 * si4060_get_property_8
 *
 * gets an 8 bit (1 byte) property in the Si4060
 *
 * group:	the group number of the property
 * prop:	the number (index) of the property
 *
 * returns:	the value of the property
 */
inline uint8_t si4060_get_property_8(uint8_t group, uint8_t prop) {
	uint8_t temp = 0;
	spi_select();
	spi_write(CMD_GET_PROPERTY);
	spi_write(group);
	spi_write(1);
	spi_write(prop);
	spi_deselect();
	si4060_get_cts(1);
	temp = spi_read(); /* read property */
	spi_deselect();
	return temp;
}

/*
 * si4060_set_property_16
 *
 * sets an 16 bit (2 byte) property in the Si4060
 *
 * group:	the group number of the property
 * prop:	the number (index) of the property
 * val:		the value to set
 */
inline void si4060_set_property_16(uint8_t group, uint8_t prop, uint16_t val) {
	si4060_get_cts(0);
	spi_select();
	spi_write(CMD_SET_PROPERTY);
	spi_write(group);
	spi_write(2);
	spi_write(prop);
	spi_write(val >> 8);
	spi_write(val);
	spi_deselect();
}

/*
 * si4060_set_property_16_nocts
 *
 * sets an 16 bit (2 byte) property in the Si4060
 * does not check for CTS from the Si4060
 *
 * group:	the group number of the property
 * prop:	the number (index) of the property
 * val:		the value to set
 */
inline void si4060_set_property_16_nocts(uint8_t group, uint8_t prop, uint16_t val) {
	spi_select();
	spi_write(CMD_SET_PROPERTY);
	spi_write(group);
	spi_write(2);
	spi_write(prop);
	spi_write(val >> 8);
	spi_write(val);
	spi_deselect();
}

/*
 * si4060_set_property_24
 *
 * sets an 24 bit (3 byte) property in the Si4060
 *
 * group:	the group number of the property
 * prop:	the number (index) of the property
 * val:		the value to set
 */
inline void si4060_set_property_24(uint8_t group, uint8_t prop, uint32_t val) {
	si4060_get_cts(0);
	spi_select();
	spi_write(CMD_SET_PROPERTY);
	spi_write(group);
	spi_write(3);
	spi_write(prop);
	spi_write(val >> 16);
	spi_write(val >> 8);
	spi_write(val);
	spi_deselect();
}

/*
 * si4060_set_property_32
 *
 * sets an 32 bit (4 byte) property in the Si4060
 *
 * group:	the group number of the property
 * prop:	the number (index) of the property
 * val:		the value to set
 */
inline void si4060_set_property_32(uint8_t group, uint8_t prop, uint32_t val) {
	si4060_get_cts(0);
	spi_select();
	spi_write(CMD_SET_PROPERTY);
	spi_write(group);
	spi_write(4);
	spi_write(prop);
	spi_write(val >> 24);
	spi_write(val >> 16);
	spi_write(val >> 8);
	spi_write(val);
	spi_deselect();
}

/*
 * si4060_gpio_pin_cfg
 *
 * configures the GPIOs on the Si4060
 * see the GPIO_*-defines for reference
 *
 * gpio(0..3):	setting flags for respective GPIOs
 * drvstrength:	the driver strength
 */
void si4060_gpio_pin_cfg(uint8_t gpio0, uint8_t gpio1, uint8_t gpio2, uint8_t gpio3, uint8_t drvstrength) {
	si4060_get_cts(0);
	spi_select();
	spi_write(CMD_GPIO_PIN_CFG);
	spi_write(gpio0);
	spi_write(gpio1);
	spi_write(gpio2);
	spi_write(gpio3);
	spi_write(NIRQ_MODE_DONOTHING);
	spi_write(SDO_MODE_DONOTHING);
	spi_write(drvstrength);
	spi_deselect();
}

/*
 * si4060_part_info
 *
 * gets the PART_ID from the Si4060
 * this can be used to check for successful communication.
 * as the SPI bus returns 0xFF (MISO=high) when no slave is connected,
 * reading CTS can not verify communication to the Si4060.
 *
 * returns:	the PART_ID - should be 0x4060
 */
uint16_t si4060_part_info(void) {
	uint16_t temp;

	si4060_get_cts(0);
	temp = 0;
	spi_select();
	spi_write(CMD_PART_INFO);
	spi_deselect();
	si4060_get_cts(1);
	spi_read();	 	/* ignore CHIPREV */
	temp = spi_read(); 	/* read PART[0] */
	temp = temp << 8;
	temp |= spi_read(); 	/* read PART[1] */
	spi_deselect();
	return temp;
}

/*
 * si4060_start_tx
 *
 * starts transmission by the Si4060.
 *
 * channel:	the channel to start transmission on
 */
void si4060_start_tx(uint8_t channel) {
	si4060_get_cts(0);
	spi_select();
	spi_write(CMD_START_TX);
	spi_write(channel);
	spi_write(START_TX_TXC_STATE_SLEEP | START_TX_RETRANSMIT_0 | START_TX_START_IMM);
	/* set length to 0 for direct mode (is this correct?) */
	spi_write(0x00);
	spi_write(0x00);
	spi_deselect();
}

/*
 * si4060_stop_tx
 *
 * makes the Si4060 stop all transmissions by transistioning to SLEEP state
 */
void si4060_stop_tx(void) {
	si4060_change_state(STATE_SLEEP);
}

/*
 * si4060_set_offset
 *
 * sets the FSK offset inside the Si4060 PLL. As PLL FRAC- and INTE-registers can't be modified while
 * transmitting, we must modify the MODEM_FREQ_DEV or MODEM_FREQ_OFFSET registers in transmission.
 * as we won't need more deviation than 25kHz, we can use the OFFSET register and save one byte.
 *
 * offset: frequency offset from carrier frequency (PLL tuning resolution)
 *
 */
inline void si4060_set_offset(uint16_t offset) {
	si4060_set_property_16_nocts(PROP_MODEM, MODEM_FREQ_OFFSET, offset);
}

/*
 * si4060_set_filter
 *
 * writes the bandpass filter coefficients into the Si4060. these realize a low pass filter for 
 * harmonics of the square wave modulation waveform and a high pass filter for the 
 * APRS preemphasis.
 *
 */
void si4060_set_filter(void) {
	//uint8_t coeff[9] = {0x1d, 0xe5, 0xb8, 0xaa, 0xc0, 0xf5, 0x36, 0x6b, 0x7f};	// 6dB@1200 Hz, 2400 Hz
	//uint8_t coeff[9] = {0x07, 0xde, 0xbf, 0xb9, 0xd4, 0x05, 0x40, 0x6d, 0x7f};	// 3db@1200 Hz, 2400 Hz
	//uint8_t coeff[9] = {0xfa, 0xe5, 0xd8, 0xde, 0xf8, 0x21, 0x4f, 0x71, 0x7f};	// LP only, 2400 Hz
	//uint8_t coeff[9] = {0xd9, 0xf1, 0x0c, 0x29, 0x44, 0x5d, 0x70, 0x7c, 0x7f}; 	// LP only, 4800 Hz
	//uint8_t coeff[9] = {0xd5, 0xe9, 0x03, 0x20, 0x3d, 0x58, 0x6d, 0x7a, 0x7f}; 	// LP only, 4400 Hz
	uint8_t coeff[9] = {0x81, 0x9f, 0xc4, 0xee, 0x18, 0x3e, 0x5c, 0x70, 0x76};	// 6dB@1200Hz, 4400 Hz (bad stopband)

	si4060_set_property_8(PROP_MODEM,
			MODEM_TX_FILTER_COEFF_8,
			coeff[8]);
	si4060_set_property_8(PROP_MODEM,
			MODEM_TX_FILTER_COEFF_7,
			coeff[7]);
	si4060_set_property_8(PROP_MODEM,
			MODEM_TX_FILTER_COEFF_6,
			coeff[6]);
	si4060_set_property_8(PROP_MODEM,
			MODEM_TX_FILTER_COEFF_5,
			coeff[5]);
	si4060_set_property_8(PROP_MODEM,
			MODEM_TX_FILTER_COEFF_4,
			coeff[4]);
	si4060_set_property_8(PROP_MODEM,
			MODEM_TX_FILTER_COEFF_3,
			coeff[3]);
	si4060_set_property_8(PROP_MODEM,
			MODEM_TX_FILTER_COEFF_2,
			coeff[2]);
	si4060_set_property_8(PROP_MODEM,
			MODEM_TX_FILTER_COEFF_1,
			coeff[1]);
	si4060_set_property_8(PROP_MODEM,
			MODEM_TX_FILTER_COEFF_0,
			coeff[0]);
}

/*
 * si4060_setup
 *
 * initializes the Si4060 by setting all neccesary internal registers.
 * has to be called after si4060_power_up.
 *
 * mod_type:	the type of modulation to use, use the MODEM_MOD_TYPE values (MOD_TYPE_*)
 */
void si4060_setup(uint8_t mod_type) {
	/* set high performance mode */
	si4060_set_property_8(PROP_GLOBAL,
			GLOBAL_CONFIG,
			GLOBAL_RESERVED | POWER_MODE_HIGH_PERF | SEQUENCER_MODE_FAST);
#ifdef	USE_TCXO
	si4060_set_property_8(PROP_GLOBAL,
			GLOBAL_XO_TUNE,
			0x00);
#endif
	/* set up GPIOs */
	si4060_gpio_pin_cfg(GPIO_MODE_DONOTHING,
			GPIO_MODE_DONOTHING,
			GPIO_MODE_DONOTHING,
			PULL_CTL + GPIO_MODE_INPUT,
			DRV_STRENGTH_HIGH);
	/* disable preamble */
	si4060_set_property_8(PROP_PREAMBLE,
			PREAMBLE_TX_LENGTH,
			0);
	/* do not transmit sync word */
	si4060_set_property_8(PROP_SYNC,
			SYNC_CONFIG,
			SYNC_NO_XMIT);
	/* setup the NCO modulo and oversampling mode */
	si4060_set_property_32(PROP_MODEM,
			MODEM_TX_NCO_MOD,
			MOD_TX_OSR_10 | (XO_FREQ / 10));
	/* setup the NCO data rate for APRS */
	si4060_set_property_24(PROP_MODEM,
			MODEM_DATA_RATE,
			RF_MOD_APRS_SR);
	/* use 2FSK from async GPIO0 */
	si4060_set_property_8(PROP_MODEM,
			MODEM_MOD_TYPE,
			(mod_type & 0x07) | MOD_SOURCE_DIRECT | MOD_GPIO_3 | MOD_DIRECT_MODE_SYNC);
	/* set up the PA duty cycle */
	si4060_set_property_8(PROP_PA,
			PA_BIAS_CLKDUTY,
			PA_BIAS_CLKDUTY_SIN_25);
	si4060_set_filter();
}

void si4060_freq_aprs_eu(void) {
	/* use 2GFSK from async GPIO0 */
	si4060_set_property_8(PROP_MODEM,
			MODEM_MOD_TYPE,
			MOD_TYPE_2GFSK | MOD_SOURCE_DIRECT | MOD_GPIO_3 | MOD_DIRECT_MODE_SYNC);
	/* setup divider to 24 (for 2m amateur radio band) */
	si4060_set_property_8(PROP_MODEM,
			MODEM_CLKGEN_BAND,
			SY_SEL_1 | FVCO_DIV_24);
	/* set up the integer divider */
	si4060_set_property_8(PROP_FREQ_CONTROL,
			FREQ_CONTROL_INTE,
			(uint8_t)(FDIV_INTE_2M_EU));
	/* set up the fractional divider */
	si4060_set_property_24(PROP_FREQ_CONTROL,
			FREQ_CONTROL_FRAC,
			(uint32_t)(FDIV_FRAC_2M_EU));
	/* setup frequency deviation offset */
	si4060_set_property_16(PROP_MODEM,
			MODEM_FREQ_OFFSET,
			0);
	/* setup frequency deviation */
	si4060_set_property_24(PROP_MODEM,
			MODEM_FREQ_DEV,
			(uint16_t)(2*FDEV_APRS));
}

void si4060_freq_aprs_us(void) {
	/* use 2GFSK from async GPIO0 */
	si4060_set_property_8(PROP_MODEM,
			MODEM_MOD_TYPE,
			MOD_TYPE_2GFSK | MOD_SOURCE_DIRECT | MOD_GPIO_3 | MOD_DIRECT_MODE_SYNC);
	/* setup divider to 24 (for 2m amateur radio band) */
	si4060_set_property_8(PROP_MODEM,
			MODEM_CLKGEN_BAND,
			SY_SEL_1 | FVCO_DIV_24);
	/* set up the integer divider */
	si4060_set_property_8(PROP_FREQ_CONTROL,
			FREQ_CONTROL_INTE,
			(uint8_t)(FDIV_INTE_2M_US));
	/* set up the fractional divider */
	si4060_set_property_24(PROP_FREQ_CONTROL,
			FREQ_CONTROL_FRAC,
			(uint32_t)(FDIV_FRAC_2M_US));
	/* setup frequency deviation offset */
	si4060_set_property_16(PROP_MODEM,
			MODEM_FREQ_OFFSET,
			0);
	/* setup frequency deviation */
	si4060_set_property_24(PROP_MODEM,
			MODEM_FREQ_DEV,
			(uint16_t)(2*FDEV_APRS));
}

void si4060_freq_aprs_cn(void) {
	/* use 2GFSK from async GPIO0 */
	si4060_set_property_8(PROP_MODEM,
			MODEM_MOD_TYPE,
			MOD_TYPE_2GFSK | MOD_SOURCE_DIRECT | MOD_GPIO_3 | MOD_DIRECT_MODE_SYNC);
	/* setup divider to 24 (for 2m amateur radio band) */
	si4060_set_property_8(PROP_MODEM,
			MODEM_CLKGEN_BAND,
			SY_SEL_1 | FVCO_DIV_24);
	/* set up the integer divider */
	si4060_set_property_8(PROP_FREQ_CONTROL,
			FREQ_CONTROL_INTE,
			(uint8_t)(FDIV_INTE_2M_CN));
	/* set up the fractional divider */
	si4060_set_property_24(PROP_FREQ_CONTROL,
			FREQ_CONTROL_FRAC,
			(uint32_t)(FDIV_FRAC_2M_CN));
	/* setup frequency deviation offset */
	si4060_set_property_16(PROP_MODEM,
			MODEM_FREQ_OFFSET,
			0);
	/* setup frequency deviation */
	si4060_set_property_24(PROP_MODEM,
			MODEM_FREQ_DEV,
			(uint16_t)(2*FDEV_APRS));
}

void si4060_freq_2m_rtty(void) {
	/* use 2FSK from async GPIO0 */
	si4060_set_property_8(PROP_MODEM,
			MODEM_MOD_TYPE,
			MOD_TYPE_2FSK | MOD_SOURCE_DIRECT | MOD_GPIO_3 | MOD_DIRECT_MODE_ASYNC);
	/* setup divider to 24 (for 2m-band) */
	si4060_set_property_8(PROP_MODEM,
			MODEM_CLKGEN_BAND,
			SY_SEL_1 | FVCO_DIV_24);
	/* set up the integer divider */
	si4060_set_property_8(PROP_FREQ_CONTROL,
			FREQ_CONTROL_INTE,
			(uint8_t)(FDIV_INTE_2M_RTTY));
	/* set up the fractional divider */
	si4060_set_property_24(PROP_FREQ_CONTROL,
			FREQ_CONTROL_FRAC,
			(uint32_t)(FDIV_FRAC_2M_RTTY));
	/* setup frequency deviation offset */
	si4060_set_property_16(PROP_MODEM,
			MODEM_FREQ_OFFSET,
			0);
	/* setup frequency deviation */
	si4060_set_property_24(PROP_MODEM,
			MODEM_FREQ_DEV,
			(uint16_t)(FDEV_RTTY));
}

