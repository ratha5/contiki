/**
* \file
*         WIMEA-ICT Gen3 Sink Node
* \details
*	ATMEGFA256RFR2 RSS2 MOTEwith RTC, SD card
* \author
*         Maximus Byamukama <maximus.byamukama@cedat.mak.ac.ug>
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <avr/io.h>
#include <avr/sleep.h>
#include "contiki.h"
#include "rss2.h"
#include "netstack.h"
#include "rf230bb.h"
#include "net/rime/rime.h"
#include "net/packetbuf.h"
#include "dev/leds.h"
#include "dev/adc.h"
#include "dev/i2c.h"
#include "dev/ds3231.h"
#include "lib/fs/fat/diskio.h"
#include "lib/fs/fat/ff.h"
#include "lib/fs/fat/integer.h"

#define BATT_VLOW 4.70

float v_in, v_mcu;
uint8_t rssi,lqi,transmit_busy=0;
uint8_t p1,p2; /*temporary upper and lower parts of floating point variables*/
uint16_t err=0,v_low=0,lines=0,count=0;

char report [200],rtc_date[20],buf[200],sinkrep[200];

datetime_t datetime;

struct broadcast_message {
	uint8_t head; 
	uint8_t seqno;
	char buf[120]; 
};


FATFS FatFs;	// FatFs work area 
FIL *fp;		// file object 
UINT bw; //function result holder


/*---------------------------------------------------------------------------*/
PROCESS(sinknode_process, "Sink Node Process");
PROCESS(uplink_process, "Uplink Process");
PROCESS(batt_check_process, "Battery Check Process");
AUTOSTART_PROCESSES(&sinknode_process, &uplink_process, &batt_check_process);
/*---------------------------------------------------------------------------*/
/* User Provided RTC Function called by FatFs module       */
/* Used to provide a Timestamp for SDCard files and folders*/
DWORD get_fattime (void)
{
	ds3231_get_datetime(&datetime);
	return (DWORD)(datetime.year +276  ) << 25
	| (DWORD)(datetime.month      ) << 21
	| (DWORD)(datetime.day        ) << 16
	| (DWORD)(datetime.hours      ) << 11
	| (DWORD)(datetime.mins       ) << 5
	| (DWORD)(datetime.secs/2     );
}

static void
broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from)
{
	struct broadcast_message *msg;
	
	msg = packetbuf_dataptr();
	rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);
	lqi = packetbuf_attr(PACKETBUF_ATTR_LINK_QUALITY);
	
	ds3231_get_datetime(&datetime);

	sprintf(report, "RTC_T=20%d-%02d-%02d %02d:%02d:%02d %s [ADDR=%-d.%-d SEQ=%-d TTL=%-u RSSI=%-u LQI=%-u]\n",
	datetime.year,datetime.month,datetime.day, datetime.hours,datetime.mins,datetime.secs,msg->buf,
	from->u8[0], from->u8[1],msg->seqno, msg->head & 0xF, rssi,lqi); //datetime string

	/*write to sd card*/
	if (f_open(fp, "data.txt", FA_WRITE | FA_OPEN_ALWAYS) == FR_OK) 
	{	// Open existing or create new file
		leds_on(LEDS_RED);
		if (f_lseek(fp, f_size(fp)) == FR_OK) 
		{
			f_write(fp, report, strlen(report), &bw);	// Write data to the file
		} 	
		if (bw == strlen(report))  //we wrote the entire string
		{ 
			++lines;
			++count;
			leds_off(LEDS_RED);
		}

	}else ++err;
	f_close(fp);// close the file
	
	if(count>100 && !v_low) //500 reports accumulated (30 mins MUK station) and battery level OK
	{
		count=0;
		process_post(&uplink_process, 0x10, NULL);
	}
}


static const struct broadcast_callbacks broadcast_call = {broadcast_recv};
static struct broadcast_conn broadcast;
/*---------------------------------------------------------------------------*/
/* sinknode_process handles sink own reports and initializes RTC and SD card */
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(sinknode_process, ev, data)
{
	static struct etimer et;
	
	PROCESS_EXITHANDLER(broadcast_close(&broadcast);)
	PROCESS_BEGIN();
	broadcast_open(&broadcast, 129, &broadcast_call);	
	NETSTACK_RADIO.set_value(RADIO_PARAM_CHANNEL, 26);
	/*for rpc change*/
	NETSTACK_RADIO.off(); 
	rf230_set_rpc(0xFF); 
	NETSTACK_RADIO.on();
	
	/*** init rtc ****/
	ds3231_init();
	leds_init();
	DDRE |= (1 << PWR_1); //init power pin
	PORTE |= (1 << PWR_1); //and make sure its off -- pulled high
	
	
	/*** init sdcard ****/
	f_mount(0, &FatFs);		// Give a work area to the FatFs module 
	fp = (FIL *)malloc(sizeof (FIL)); 
	/******************/
	
	etimer_set(&et, CLOCK_SECOND * 60);
	while(1) {	
		PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
		etimer_reset(&et);
		if(!transmit_busy){
			NETSTACK_RADIO.off(); //switch off radio to ensure broadcast_recv doesn't try to access sd card
			
			leds_on(LEDS_YELLOW);
			int len=0;	
			/*get RTC datetime --this is part of the report*/
			ds3231_get_datetime(&datetime);
			sprintf(rtc_date, "20%d-%02d-%02d,%02d:%02d:%02d",datetime.year,datetime.month,
			datetime.day, datetime.hours,datetime.mins,datetime.secs);
			
			len += sprintf(sinkrep,"RTC_T=%s TXT=mak-gen3",rtc_date);
			
			v_in=adc_read_v_in();
			p1 = (int)v_in; //e.g. 4.93 gives 4
			p2 = (v_in*100)-(p1*100); // =93	
			len += sprintf(sinkrep," V_IN=%d.%02d ",p1,p2);
			
			if(v_in < BATT_VLOW)
				v_low=1;
				len += sprintf(sinkrep," V_LOW=1");
			}
			
			v_mcu = adc_read_v_mcu();
			p1 = (int)v_mcu; //e.g. 4.93 gives 4
			p2 = (v_mcu*100)-(p1*100); // =93	
			len += sprintf(sinkrep," V_MCU=%d.%02d SD.ERR=%d REPS=%d\n",p1,p2,err,lines);	
			
			report[len++] = '\0';	
			if(v_low==1)
			{
				NETSTACK_RADIO.off();
			}
			
			/*write to sd card*/
			if (f_open(fp, "data.txt", FA_WRITE | FA_OPEN_ALWAYS) == FR_OK) 
			{	// Open existing or create new file
				leds_on(LEDS_RED);
				if (f_lseek(fp, f_size(fp)) == FR_OK) 
				{
					f_write(fp, report, strlen(report), &bw);	// Write data to the file
				} 	
				if (bw == strlen(report))  //we wrote the entire string
				{ 
					++lines;
					leds_off(LEDS_RED);
				}

			}else ++err;
			f_close(fp);// close the file
			
			NETSTACK_RADIO.on(); //enable radio
			printf("%s \n", report);	
		}
	}
	PROCESS_END();
}

PROCESS_THREAD(batt_check_process, ev, data)
{
	static struct etimer et;
	PROCESS_BEGIN();
	while(1) {
		/* Delay 30 seconds */
		etimer_set(&et, CLOCK_SECOND * 60);
		PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));		
		v_in=adc_read_v_in();
		if(v_in >= BATT_VLOW && v_low==1){ //check if radio was stopped because of low power
			v_low=0;
			NETSTACK_RADIO.on();    //get back online
		}
	}
	PROCESS_END();
}

PROCESS_THREAD(uplink_process, ev, data) /*SIM5320E 3G uplink*/
{
	static struct etimer et;
	
	PROCESS_BEGIN();
	while(1) {
		PROCESS_YIELD_UNTIL(ev==0x10);	
		ev=0;		
		transmit_busy=1; //set transmit busy flag; block sinknode process
		NETSTACK_RADIO.off();
		//power on modem and wait for 20s to establish network
		PORTE &= ~ (1 << PWR_1); 	
		etimer_set(&et, CLOCK_SECOND * 20);
		PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
		
		/*START AT COMMANDS - NO ACK*/
		printf("AT+CGSOCKCONT=1,\"IP\",\"INTERNET\"\n");
		etimer_set(&et, CLOCK_SECOND * 1);
		PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
		printf("AT+CSOCKSETPN = 1\n");
		etimer_set(&et, CLOCK_SECOND * 1);
		PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
		printf("AT+CIPMODE = 0\n");
		etimer_set(&et, CLOCK_SECOND * 1);
		PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
		printf("AT+NETOPEN\n");
		etimer_set(&et, CLOCK_SECOND * 15);
		PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
		

		printf("AT+CIPOPEN = 0,\"TCP\",\"wimea.mak.ac.ug\",10000\n");
		etimer_set(&et, CLOCK_SECOND * 20);
		PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));	
		
		
		int i=0;
		
		/*Now ready to read saved data*/
		if (f_open(fp, "data.txt", FA_READ) == FR_OK) 
		{	/* Read all lines*/
			for (i = 0; (f_eof(fp) == 0); i++)
			{
				f_gets((char*)buf, sizeof(buf), fp); /*NB: The read operation continues until a '\n' is stored*/
				
				printf("AT+CIPSEND = 0,\n"); //send commands
				etimer_set(&et, CLOCK_SECOND/4);
				PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));	
				printf("%s%c",buf,26);  //send data NB: 26 = CTRL+Z
				etimer_set(&et, CLOCK_SECOND/4);
				PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
			}
			
			/*send "END" */
			printf("AT+CIPSEND = 0,\n"); //send commands
			etimer_set(&et, CLOCK_SECOND/4);
			PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));	
			printf("DISCONNECT%c",26);  //DISCONNECT instructs TCPListener on server to restart
			etimer_set(&et, CLOCK_SECOND/4);
			PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
				
		}
		f_close(fp);// close the file
		/*clear the file*/
		if (f_open(fp, "data.txt", FA_WRITE | FA_OPEN_ALWAYS) == FR_OK) 
		{
			f_truncate(fp);
		}
		f_close(fp);// close the file
		
		printf("AT + CIPCLOSE = 0\n"); //close connection (rarely works!)
		etimer_set(&et, CLOCK_SECOND/2);
		PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));	
		/*power off modem*/
		PORTE |= (1 << PWR_1);	
		
		/*switch radio back on*/
		transmit_busy=0; //reset transmit_busy flag to allow sinknode process
		NETSTACK_RADIO.on();
	}
	PROCESS_END();
}