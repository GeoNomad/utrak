/*
 * main tracker software
 *
 * Stefan Biereigel
 *
 */

#include <msp430.h>
#include <inttypes.h>
#include "gps.h"
#include "main.h"
#include "nmea.h"
#include "si4060.h"
#include "spi.h"
#include "string.h"

/*
 * GLOBAL VARIABLES
 */

/*
 * housekeeping variables
 */
volatile uint16_t seconds = 0;		/* timekeeping via timer */
volatile uint16_t overflows = 0;	/* ISR overflow counter */
volatile uint16_t tick = 0;		/* flag for timer handling (ISR -> main) */
volatile uint16_t adc_result;		/* ADC result for temp / voltage (ISR -> main) */
uint16_t sent_id = 0;			/* sentence id */
volatile uint32_t ta1_freq = 0;		/* measured frequency of ta1clk (ISR -> main) */
volatile uint16_t ta1_ovf = 0;		/* ta1 frequency counter ISR overflow counter */
volatile uint16_t fc_tick = 0;		/* flag for frequency handling */
volatile uint16_t stx = 0;			/* uart byte to transmit (main -> ISR) */
volatile uint16_t stx_len = 0;		/* length of uart byte (main -> ISR) */
/*
 * the NMEA data buffer
 * it was confirmed that the Linx RXM-GPS-RM sticks to the standard
 */
volatile uint16_t nmea_buf_index = 0;	/* the index for writing to the buffer */
volatile uint16_t nmea_buf_rdy = 0;	/* the ready-flag (ISR -> main) */
volatile char nmea_buf[NMEA_BUF_SIZE] = { 0 };	/* the actual buffer */

/*
 * the TX data buffer
 * contains ASCII data, which is either transmitted as CW oder RTTY
 */
uint16_t tx_buf_index = 0;			/* the index for reading from the buffer */
uint16_t tx_buf_rdy = 0;			/* the read-flag (main -> main) */
uint16_t tx_buf_length = 0;			/* how many chars to send */
char tx_buf[TX_BUF_MAX_LENGTH] = {SYNC_PREFIX "$$" PAYLOAD_NAME ","};	/* the actual buffer */

/*
 * GPS fix data and data for tlm string
 * extracted from NMEA sentences by GPS data processing
 */
uint16_t tlm_sent_id_length;
char tlm_sent_id[SENT_ID_LENGTH_MAX] = { 0 };
char tlm_time[TIME_LENGTH] = { 0 };
char tlm_lat[LAT_LENGTH+1] = { 0 };
char tlm_lon[LON_LENGTH+1] = { 0 };
uint8_t tlm_alt_length;
char tlm_alt[ALT_LENGTH_MAX] = { 0 };
char tlm_sat[SAT_LENGTH] = { 0 };
char tlm_volt[VOLT_LENGTH] = { 0 };
char tlm_temp[TEMP_LENGTH+1] = { 0 };

/*
 * hw_init
 *
 * hardware initialisation routine
 *
 * GPIO init
 *   UCSI-pin direction is don't care (see UG), pull down for MISO
 * init eUSCI_A to UART (9600/8N1)
 *   clocked by SMCLK
 * init eUSCI_B to SPI
 *   clocked by SMCLK
 * init TACCR0 for systick
 *   clocked by SMCLK
 *
 */
void hw_init(void) {
	/* DCO init, SMCLK is 5.37MHz divided by 8 */
	CSCTL0_H = 0xA5;					/* write CS password */
	CSCTL1 = 0;						/* set DCO to 5.37MHz */
	CSCTL2 = SELA__DCOCLK + SELS__DCOCLK + SELM__DCOCLK;	/* DCO as ACLK, SMCLK, MCLK */
	CSCTL3 = DIVA__1 + DIVS__8 + DIVM__1;			/* divide SMCLK by 8 */
	CSCTL4 = XT1OFF + XT2OFF;				/* disable oscillators */

	/* GPIO init Port 1 */
	P1OUT &= ~(MISO + CLK_GPS + CLK_SI + UART);
	P1REN |= MISO;
	P1DIR = SI_SHDN + SI_DATA + UART;					/* GPIOs for output */
	P1SEL1 |= MOSI + MISO + CLK_SI;						/* USCI_B MOSI, MISO */
	P1SEL1 &= ~(SI_SHDN + SI_DATA + CLK_GPS + UART);
	P1SEL0 &= ~(SI_SHDN + SI_DATA + MOSI + MISO + CLK_SI + CLK_GPS + UART);	/* USCI_B MOSI, MISO */

	/* GPIO init Port 2 */
	P2DIR = TXD;				/* GPIOs for output */
	P2SEL1 |= RXD + TXD + SCLK;		/* USCI_A RXD, TXD, USCI_B CLK */
	P2SEL0 &= ~(RXD + TXD + SCLK);		/* USCI_A RXD, TXD, USCI_B CLK */

	/* GPIO init Port J */
	PJOUT |= CS;
	PJDIR = CS;

	/* USCI_A (GPS UART) init */
	UCA0CTL1 = UCSWRST; 			/* reset USCI */
	UCA0CTL1 |= UCSSEL_2;			/* SMCLK */
	UCA0BR0 = 4;
	UCA0BR1 = 0;
	UCA0MCTLW = (0xFD<<8)+(5<<4)+UCOS16;	/* set UCA0BRS */
	UCA0CTL1 &= ~UCSWRST;			/* release from reset */
	//UCA0IE |= UCRXIE;			/* Enable RX interrupt */

	/* USCI_B (Si4060 SPI) init */
	UCB0CTLW0 = UCSWRST;			/* Put state machine in reset */
	UCB0CTLW0 |= UCMST+UCSYNC+UCCKPH+UCMSB;	/* 3-pin, 8-bit SPI master */
						/* Clock polarity high, MSB */
	UCB0CTLW0 |= UCSSEL_1;			/* ACLK */
	UCB0BR0 = 0;				/* divide by /1 */
	UCB0BR1 = 0;
	UCB0CTLW0 &= ~UCSWRST;			/* Initialize USCI state machine */
	UCB0IE |= UCRXIE;			/* Enable RX interrupt */

	TA0CCTL0 = CCIE;			/* TACCR0 interrupt enabled */
	TA0CCR0 = (DCO_FREQ / 8 + RTTY_BAUDRATE/2) / RTTY_BAUDRATE;
	TA0CTL = TASSEL_2 + MC_1;		/* SMCLK, UP mode */

	/* external clock for Timer A1 */
	TA1CTL = TASSEL__TACLK + MC__CONTINUOUS + TACLR;

	P1IES |= CLK_GPS;			/* interrupt on falling edge */
	P1IE &= ~CLK_GPS;			/* deactivate interrupt on reset */

	TB0CTL = TBSSEL__ACLK + ID__1 + MC__UP + TBCLR;
	TB0CCR0 = (DCO_FREQ + UART_BAUDRATE/2) / UART_BAUDRATE;
	TB0CCTL0 = 0;

	/* Enable Interrupts */
	__bis_SR_register(GIE);			/* set interrupt enable bit */
}

/*
 * get_battery_voltage
 *
 * reads ADC channel 1, where the lithium cell is connected
 *
 * returns:	the voltage in millivolts (3000 = 3000mV = 3,0V)
 */
uint16_t get_battery_voltage(void) {
	uint16_t i;
	uint16_t voltage;
	/* enable ADC */
	ADC10CTL0 = ADC10SHT_2 + ADC10ON;	/* ADC10ON, S&H=16 ADC clks */
	ADC10CTL1 = ADC10SHP + ADC10SSEL0 + ADC10SSEL1;		/* ADCCLK = SMCLK */
	ADC10CTL2 = ADC10RES;			/* 10-bit conversion results */
	ADC10MCTL0 = ADC10INCH_2;		/* A1 ADC input select; Vref=AVCC */
	ADC10IE = ADC10IE0;			/* Enable ADC conv complete interrupt */
	__delay_cycles(5000);			/* Delay for Ref to settle */
	voltage = 0;
	for (i = 0; i < 10; i++) {
		ADC10CTL0 |= ADC10ENC + ADC10SC;	/* Sampling and conversion start */
		__bis_SR_register(CPUOFF + GIE);	/* LPM0, ADC10_ISR will force exit */
		/* take ADC reading */
		voltage += adc_result * 32 / 10;		/* convert to mV */
	}
	voltage /= 10;
	/* disable ADC */
	ADC10IE &= ~ADC10IE0;			/* Enable ADC conv complete interrupt */
	ADC10CTL0 &= ~ADC10ON;			/* ADC10 off */

	return voltage;
}

/* get_die_temperature
 *
 * reads the ADC channel 10, where the internal temperature sensor is connected
 *
 * returns:	the temperature in degrees celsius
 */
int16_t get_die_temperature(void) {
	long temperature;

	/* enable ADC */
	// Configure ADC10 - Pulse sample mode; ADC10SC trigger
	ADC10CTL0 = ADC10SHT_8 + ADC10ON;	/* 16 ADC10CLKs; ADC ON,temperature sample period>30us */
	ADC10CTL1 = ADC10SHP + ADC10SSEL0 + ADC10SSEL1;	/* s/w trig, single ch/conv */
	ADC10CTL2 = ADC10RES;			/* 10-bit conversion results */
	ADC10MCTL0 = ADC10SREF_1 + ADC10INCH_10;/* ADC input ch A10 => temp sense */
	ADC10IE |= ADC10IE0;			/* enable the Interrupt */

	/* Configure internal reference */
	while(REFCTL0 & REFGENBUSY);		/* If ref generator busy, WAIT */
	REFCTL0 |= REFVSEL_0+REFON;		/* Select internal ref = 1.5V */

	__delay_cycles(400);			/* Delay for Ref to settle */

	/* take ADC reading */
	ADC10CTL0 |= ADC10ENC + ADC10SC;        /* Sampling and conversion start */
	__bis_SR_register(CPUOFF + GIE);	/* CPU off with interrupts enabled */
	temperature = adc_result;
	temperature = (temperature - CALADC10_15V_30C) *  (85-30) / (CALADC10_15V_85C-CALADC10_15V_30C) + 30;

	/* disable ADC */
	REFCTL0 &= ~REFON;			/* disable internal ref */
	ADC10IE &= ~ADC10IE0;			/* disable ADC conv complete interrupt */
	ADC10CTL0 &= ~ADC10ON;			/* ADC10 off */

	return temperature;
}


/*
 * uart_process
 *
 * checks the UART buffer status and processes full NMEA sentences
 *
 * returns:	0 if no fix was received or the last frame was not GPGGA at all
 * 		n - the number of satellites in the last fix
 */
uint8_t uart_process(void) {
	static uint8_t i = 0;
	if (nmea_buf_rdy) {
		nmea_buf_rdy = 0;
		if (NMEA_sentence_is_GGA(nmea_buf)) {
			if (GPGGA_has_fix(nmea_buf)) {
				i = GPGGA_get_data(nmea_buf, tlm_lat, tlm_lon, tlm_alt, &tlm_alt_length, tlm_sat, tlm_time);
				if (!i) {
					return 0;
				}
				atoi8(tlm_sat, SAT_LENGTH, &i);
				return i;
			}
		}
	}
	return i;
}

/*
 * tx_blips
 *
 * when called periodically (fast enough), transmits blips with ratio 1:5
 * checks the timer-tick flag for timing
 *
 * blips slow when no fix is available, and fast if fix is available but not enough sats
 */
void tx_blips(uint8_t sats) {
	static uint8_t count = 0;	/* keeps track of blip state */

	if (!tick)
		return;

	tick = 0;
	count++;
	switch (count) {
		case 1:
			P1OUT |= SI_DATA;
			break;
		case 5:
			P1OUT &= ~SI_DATA;
			break;
		case 30:
			if (sats != 0) {
				count = 0;
			}
			break;
		case 60:
			count = 0;
			break;
		default:
			break;
	}
}

/*
 * tx_rtty
 *
 * transmits the TX buffer via RTTY at 50 baud (at 100Hz tick)
 * LSB first, in 7bit-ASCII format, 1 start bit, 2 stop bits
 *
 * the systick-flag is used for timing.
 */
void tx_rtty(void) {
	enum c_states {IDLE, START, CHARACTER, STOP1, STOP2};
	static uint16_t tx_state = 0;
	static uint16_t char_state = IDLE;
	static uint8_t data = 0;
	static uint16_t i = 0;
	static uint16_t wait = 0;
	if (!tx_buf_rdy) {
		if (tx_state == 1) {
			si4060_stop_tx();
			tx_state = 0;
		}
		return;
	}
	/* tx_buffer is ready */
	if (tx_state == 0) {
		si4060_start_tx(0);
		tx_state = 1;
		tx_buf_index = 0;
	}

	if (!tick)
		return;

	tick = 0;
	wait = !wait;
	if (wait)
		return;
	/* run this part only every second tick */
	switch (char_state) {
		case IDLE:
			P1OUT |= SI_DATA;
			i++;
			if (i == NUM_IDLE_BITS) {
				char_state = START;
				i = 0;
			}
			break;
		case START:
			P1OUT &= ~SI_DATA;
			i = 0;
			data = tx_buf[tx_buf_index];
			char_state = CHARACTER;
			break;
		case CHARACTER:
			i++;
			if (data & 0x01) {
				P1OUT |= SI_DATA;
			} else {
				P1OUT &= ~SI_DATA;
			}
			data >>= 1;
			if (i == 7) {
				char_state = STOP1;
			}
			break;
		case STOP1:
			P1OUT |= SI_DATA;
			char_state = STOP2;
			break;
		case STOP2:
			i = 0;
			char_state = START;
			tx_buf_index++;
			if (tx_buf_index >= (tx_buf_length)) {
				char_state = IDLE;
				tx_buf_rdy = 0;
			}
			break;
		default:
			break;
	}
}

/*
 * calculate_txbuf_checksum
 *
 * this routine calculates the 16bit checksum value used in the HAB protocol
 * it uses the MSP430 hardware CRC generator
 */
uint16_t calculate_txbuf_checksum(void) {
	int i;
	CRCINIRES = 0xffff;
	for (i = TX_BUF_CHECKSUM_BEGIN; i < TX_BUF_CHECKSUM_END; i++) {
		CRCDIRB_L = tx_buf[i];
	}
	return CRCINIRES;
}

/*
 * prepare_tx_buffer
 *
 * fills tx_buf with telemetry values. this depends on the
 * GPS having a fix and telemetry being extracted before
 *
 * telemetry format:
 * - callsign
 * - sentence id
 * - time
 * - latitude
 * - longitude
 * - altitude
 * - available satellites
 * - voltage of the AAA cell
 * - MSP430 temperature
 */
void prepare_tx_buffer(void) {
	int i;
	uint16_t crc;
	int16_t temp;
	uint16_t voltage;

	sent_id++;
	tlm_sent_id_length = i16toav(sent_id, tlm_sent_id);
	voltage = get_battery_voltage();
	i16toa(voltage, VOLT_LENGTH, tlm_volt);
	temp = get_die_temperature();
	if (temp < 0) {
		tlm_temp[0] = '-';
		temp = 0 - temp;
	} else {
		tlm_temp[0] = '+';
	}
	i16toa(temp, TEMP_LENGTH, tlm_temp + 1);

	for (i = 0; i < tlm_sent_id_length; i++)
		tx_buf[TX_BUF_SENT_ID_START + i] = tlm_sent_id[i];
	tx_buf[TX_BUF_SENT_ID_START + i] = ',';
	for (i = 0; i < TIME_LENGTH; i++)
		tx_buf[TX_BUF_TIME_START + i] = tlm_time[i];
	tx_buf[TX_BUF_TIME_START + i] = ',';
	for (i = 0; i < LAT_LENGTH + 1; i++)
		tx_buf[TX_BUF_LAT_START + i] = tlm_lat[i];
	tx_buf[TX_BUF_LAT_START + i] = ',';
	for (i = 0; i < LON_LENGTH + 1; i++)
		tx_buf[TX_BUF_LON_START + i] = tlm_lon[i];
	tx_buf[TX_BUF_LON_START + i] = ',';
	for (i = 0; i < tlm_alt_length; i++)
		tx_buf[TX_BUF_ALT_START + i] = tlm_alt[i];
	tx_buf[TX_BUF_ALT_START + i] = ',';
	for (i = 0; i < SAT_LENGTH; i++)
		tx_buf[TX_BUF_SAT_START + i] = tlm_sat[i];
	tx_buf[TX_BUF_SAT_START + i] = ',';
	for (i = 0; i < VOLT_LENGTH; i++)
		tx_buf[TX_BUF_VOLT_START + i] = tlm_volt[i];
	tx_buf[TX_BUF_VOLT_START + i] = ',';
	for (i = 0; i < TEMP_LENGTH + 1; i++)
		tx_buf[TX_BUF_TEMP_START + i] = tlm_temp[i];
	tx_buf[TX_BUF_TEMP_START + i] = '*';
	crc = calculate_txbuf_checksum();
	i16tox(crc, &tx_buf[TX_BUF_CHECKSUM_START]);
	for (i = 0; i < TX_BUF_POSTFIX_LENGTH; i++)
		tx_buf[TX_BUF_POSTFIX_START + i] = TX_BUF_POSTFIX[i];

	tx_buf_length = TX_BUF_FRAME_END;
	/* trigger transmission */
	tx_buf_rdy = 1;
}

/*
 * init_tx_buffer
 *
 * helper routine to fill the TX buffer with "x"es - if any of those get transmitted,
 * the field handling is not correct
 */
void init_tx_buffer(void) {
	uint16_t i;

	for (i = TX_BUF_START_OFFSET; i < TX_BUF_MAX_LENGTH; i++) {
		tx_buf[i] = 'x';
	}
}

int main(void) {
	uint16_t fix_sats = 0;
	uint16_t i;
	char uart_buf[8] = {0};
	/* set watchdog timer interval to 11 seconds */
	/* reset occurs if Si4060 does not respond or software locks up */
	/* divide SMCLK (5.370 MHz / 8) by by 2^23 in 32 bit timer */
	WDTCTL = WDTPW + WDTCNTCL + WDTIS1;
	/* init all hardware components */
	hw_init();
	/* initialize the transmission buffer (for development only) */
	init_tx_buffer();
	/* reset the radio chip from shutdown */
	si4060_reset();
	/* check radio communication */
	i = si4060_part_info();
	if (i != 0x4060) {
		while(1);
	}
	WDTCTL = WDTPW + WDTCNTCL + WDTIS1;
	/* wait for the GPS to boot */
	gps_startup_delay();
	/* tell it to use GPS only and output GGA messages on every fix */
	gps_set_nmea();
	gps_set_gps_only();
	gps_set_gga_only();
	gps_set_airborne_model();
	gps_enable_timepulse();
	gps_set_power_save();
	gps_power_save(0);
	gps_save_settings();
	/* power up the Si4060 and set it to OOK, for transmission of blips */
	/* the Si4060 occasionally locks up here, the watchdog gets it back */
	si4060_power_up();
	si4060_setup(MOD_TYPE_OOK);
	si4060_start_tx(0);
	P1IE |= CLK_GPS;	/* activate frequency counter */
	TA1CTL |= TAIE;
	while(1) {
		WDTCTL = WDTPW + WDTCNTCL + WDTIS1;
		if (fc_tick) {
			TB0CCTL0 = CCIE;	/* enable debug UART */
			fc_tick = 0;
			i32toa(ta1_freq, 8, &uart_buf);
			for (i=0;i<8;i++) {
				stx = ((uart_buf[i])<<1) + (1<<9);
				stx_len = 10;
				while(stx_len);
			}
			stx = (('\n')<<1) + (1<<9);
			stx_len = 10;
			while(stx_len);
			TB0CCTL0 = 0;	/* disable debug UART */
		}
	}
	/* entering wait state */
	/* the tracker outputs RF blips while waiting for a GPS fix */
	while (fix_sats < 5) {
		WDTCTL = WDTPW + WDTCNTCL + WDTIS1;
		fix_sats = uart_process();
		tx_blips(fix_sats);
	}
	si4060_stop_tx();
	/* modulation from now on will be RTTY */
	si4060_setup(MOD_TYPE_2FSK);
	/* activate power save mode as fix is stable */
	gps_power_save(1);
	seconds = TLM_INTERVAL + 1;
	/* entering operational state */
	/* in fixed intervals, a new TX buffer is prepared and transmitted */
	/* watchdog timer is active for resets, if somethings locks up */
	while(1) {
		WDTCTL = WDTPW + WDTCNTCL + WDTIS1;
		uart_process();
		if ((!tx_buf_rdy) && (seconds > TLM_INTERVAL)) {
			seconds = 0;
			prepare_tx_buffer();
		}
		tx_rtty();
	}
}

/*
 * USCI A0 ISR
 *
 * USCI A is UART. RX appends incoming bytes to the NMEA buffer
 */
#pragma vector=USCI_A0_VECTOR
__interrupt void USCI_A0_ISR(void)
{
	switch(UCA0IV) {
		case 0:						/* Vector 0 - no interrupt */
			break;
		case 2:						/* Vector 2 - RXIFG */
			if (nmea_buf_index < (NMEA_BUF_SIZE - 1))
				nmea_buf_index++;
			if (UCA0RXBUF == '$')
				nmea_buf_index = 0;
			if (UCA0RXBUF == '\n')
				nmea_buf_rdy = 1;
			nmea_buf[nmea_buf_index] = UCA0RXBUF;
			break;
		case 4:						/* Vector 4 - TXIFG */
			break;
		default:
			break;
	}
}


/*
 * ADC10 ISR
 *
 * just resumes CPU operation, as ADC conversions are done in CPUOFF-state
 */
#pragma vector=ADC10_VECTOR
__interrupt void ADC10_ISR(void)
{
	switch(ADC10IV)
	{
		case  0: break;                          // No interrupt
		case  2: break;                          // conversion result overflow
		case  4: break;                          // conversion time overflow
		case  6: break;                          // ADC10HI
		case  8: break;                          // ADC10LO
		case 10: break;                          // ADC10IN
		case 12:adc_result = ADC10MEM0;		 // ADC10MEM0
			__bic_SR_register_on_exit(CPUOFF);
			break;
		default: break;
	}
}

/*
 * Timer A0 ISR
 *
 * realises a systick function. tick-flag can be polled by main program
 */
#pragma vector = TIMER0_A0_VECTOR
__interrupt void Timer_A (void)
{
	tick = 1;
	overflows++;
	if (overflows == 100) {
		seconds++;
		overflows = 0;
	}
}

/*
 * Port1 ISR
 *
 * measure TA1CLK and DCO frequency
 */
#pragma vector = PORT1_VECTOR
__interrupt void capture (void)
{
	static uint16_t old_ta1 = 0;
	uint16_t new_ta1;
	new_ta1 = TA1R;
	if (new_ta1 < 32768) {
		if (TA1IV & TA1IV_TAIFG) {
			ta1_ovf++;
			TA1IV &= ~TA1IV_TAIFG;
		}
	}

	ta1_freq = ta1_ovf * ((uint32_t)1<<16) + new_ta1 - old_ta1;
	ta1_ovf = 0;
	fc_tick = 1;
	old_ta1 = new_ta1;
	P1IFG &= ~CLK_GPS;
}

/*
 * Timer A1 ISR
 *
 * counts timer overflows
 */
#pragma vector = TIMER1_A1_VECTOR
__interrupt void count_ovf (void)
{
	ta1_ovf++;
	TA1IV &= ~TA1IV_TAIFG;
}

/*
 * Timer B0 ISR
 *
 * realises a software uart
 *
 * stx_len: length of uart transmission(10)
 * stx: byte to transmit with start and stop bits
 */
#pragma vector = TIMER0_B0_VECTOR
__interrupt void stx_isr (void)
{
	if (stx_len) {
		if (stx & 1)
			P1OUT |= UART;
		else
			P1OUT &= ~UART;
		stx = stx>>1;
		stx_len--;
	} else {
		P1OUT |= UART;
	}
}
