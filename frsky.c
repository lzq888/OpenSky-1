/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

   author: fishpepper <AT> gmail.com
*/
#include "frsky.h"
#include <string.h>
#include "debug.h"
#include "timeout.h"
#include "led.h"
#include "delay.h"
#include "dma.h"
#include "wdt.h"
#include "adc.h"
#include "storage.h"
#include "ppm.h"
#include "apa102.h"
#include "failsafe.h"
#include "sbus.h"

//this will make binding not very reliable, use for debugging only!
#define FRSKY_DEBUG_BIND_DATA 0
#define FRSKY_DEBUG_HOPTABLE 1

//hop data & config
//__xdata uint8_t storage.frsky_txid[2] = {0x16, 0x68};
//__xdata uint8_t storage.frsky_hop_table[FRSKY_HOPTABLE_SIZE] = {0x01, 0x42, 0x83, 0xC4, 0x1A, 0x5B, 0x9C, 0xDD, 0x33, 0x74, 0xB5, 0x0B, 0x4C, 0x8D, 0xCE, 0x24, 0x65, 0xA6, 0xE7, 0x3D, 0x7E, 0xBF, 0x15, 0x56, 0x97, 0xD8, 0x2E, 0x6F, 0xB0, 0x06, 0x47, 0x88, 0xC9, 0x1F, 0x60, 0xA1, 0xE2, 0x38, 0x79, 0xBA, 0x10, 0x51, 0x92, 0xD3, 0x29, 0x6A, 0xAB};
//__xdata int8_t storage.frsky_freq_offset;
__xdata uint8_t frsky_current_ch_idx;

//rssi
__xdata uint8_t frsky_rssi;
__xdata uint8_t frsky_link_quality;

//pll calibration
__xdata uint8_t frsky_calib_fscal1_table[FRSKY_HOPTABLE_SIZE];
__xdata uint8_t frsky_calib_fscal2;
__xdata uint8_t frsky_calib_fscal3;
//__xdata int16_t storage.frsky_freq_offset_acc;

//rf rxtx buffer
__xdata volatile uint8_t frsky_packet_buffer[FRSKY_PACKET_BUFFER_SIZE];
__xdata volatile uint8_t frsky_packet_received;
__xdata volatile uint8_t frsky_packet_sent;
__xdata volatile uint8_t frsky_mode;

//dma config
__xdata DMA_DESC frsky_dma_config;

void frsky_init(void){
    //uint8_t i;
    debug("frsky: init\n"); debug_flush();

    frsky_link_quality = 0;

    frsky_packet_received = 0;
    frsky_packet_sent = 0;

    frsky_rssi = 100;

    //init frsky registersttings for cc2500
    frsky_configure();

    if (frsky_bind_jumper_set()){
        //do binding
        frsky_do_bind();
        //binding will never return/continue
    }

    //show info:
    debug("frsky: using txid 0x"); debug_flush();
    debug_put_hex8(storage.frsky_txid[0]);
    debug_put_hex8(storage.frsky_txid[1]);
    debug_put_newline();

    //init txid matching
    frsky_configure_address();

    //tune cc2500 pll and save the values to ram
    frsky_calib_pll();

    debug("frsky: init done\n");debug_flush();
}


void frsky_configure(void){
    debug("frsky: configure\n"); debug_flush();

    //start idle
    RFST = RFST_SIDLE;

    //not necessary here IOCFG0 = 0x01
    //not necessary here IOCFG2 = 0x0E
    MCSM1    = 0x0F; //go back to rx after transmission completed //0x0C;
    MCSM0    = 0x18;
    PKTLEN   = FRSKY_PACKET_LENGTH; //on 251x this has to be exactly our size
    PKTCTRL0 = 0x05;
    PA_TABLE0  = 0xFF;
    FSCTRL1  = 0x08;
    FSCTRL0  = 0x00;
    //set base freq 2404 mhz
    FREQ2    = 0x5C;
    FREQ1    = 0x76;
    FREQ0    = 0x27;
    MDMCFG4  = 0xAA;
    MDMCFG3  = 0x39;
    MDMCFG2  = 0x11;
    MDMCFG1  = 0x23;
    MDMCFG0  = 0x7A;
    DEVIATN  = 0x42;
    FOCCFG   = 0x16;
    BSCFG    = 0x6C;
    AGCCTRL2 = 0x03;
    AGCCTRL1 = 0x40;
    AGCCTRL0 = 0x91;
    FREND1   = 0x56;
    FREND0   = 0x10;
    FSCAL3   = 0xA9;
    FSCAL2   = 0x05;
    FSCAL1   = 0x00;
    FSCAL0   = 0x11;
    //???FSTEST   = 0x59;
    TEST2    = 0x88;
    TEST1    = 0x31;
    TEST0    = 0x0B;
    //???FIFOTHR  = 0x07;
    ADDR     = 0x00;

    //for now just append status
    PKTCTRL1 = CC2500_PKTCTRL1_APPEND_STATUS;

    debug("frsky: configure done\n"); debug_flush();
}


void frsky_configure_address(void){
    //start idle
    RFST = RFST_SIDLE;

    //freq offset
    FSCTRL0 = storage.frsky_freq_offset;

    //never automatically calibrate, po_timeout count = 64
    //no autotune as (we use our pll map)
    MCSM0 = 0x8;

    //set address
    ADDR = storage.frsky_txid[0];

    //append status, filter by address, autoflush on bad crc, PQT=0
    PKTCTRL1 = CC2500_PKTCTRL1_APPEND_STATUS | CC2500_PKTCTRL1_CRC_AUTOFLUSH | CC2500_PKTCTRL1_FLAG_ADR_CHECK_01;
}


void frsky_tune_channel(uint8_t ch){
    //start idle
    RFST = RFST_SIDLE;

    //set channel number
    CHANNR = ch;

    //start Self calib:
    RFST = RFST_SCAL;

    //wait for scal end
    while(MARCSTATE != 0x01);

    //now FSCAL3..1 shold be set up correctly! yay!
}

void frsky_rf_interrupt(void) __interrupt RF_VECTOR{
    //clear int flag
    RFIF &= ~(1<<4);

    //clear general statistics reg
    S1CON &= ~0x03;


    if (frsky_mode == FRSKY_MODE_RX){
        //mark as received:
        frsky_packet_received = 1;
        //re arm DMA channel 0
        DMAARM = DMA_ARM_CH0;
    }else{
        frsky_packet_sent = 1;
    }
}

void frsky_setup_rf_dma(uint8_t mode){
    // CPU has priority over DMA
    // Use 8 bits for transfer count
    // No DMA interrupt when done
    // DMA triggers on radio
    // Single transfer per trigger.
    // One byte is transferred each time.

    dma_config[0].PRIORITY       = DMA_PRI_HIGH;
    dma_config[0].M8             = DMA_M8_USE_8_BITS;
    dma_config[0].IRQMASK        = DMA_IRQMASK_DISABLE;
    dma_config[0].TRIG           = DMA_TRIG_RADIO;
    dma_config[0].TMODE          = DMA_TMODE_SINGLE;
    dma_config[0].WORDSIZE       = DMA_WORDSIZE_BYTE;

    //store mode
    frsky_mode = mode;

    if (frsky_mode == FRSKY_MODE_TX) {
        // Transmitter specific DMA settings
        // Source: radioPktBuffer
        // Destination: RFD register
        // Use the first byte read + 1
        // Sets the maximum transfer count allowed (length byte + data)
        // Data source address is incremented by 1 byte
        // Destination address is constant
        SET_WORD(dma_config[0].SRCADDRH, dma_config[0].SRCADDRL, frsky_packet_buffer);
        SET_WORD(dma_config[0].DESTADDRH, dma_config[0].DESTADDRL, &X_RFD);
        dma_config[0].VLEN           = DMA_VLEN_FIRST_BYTE_P_1;
        SET_WORD(dma_config[0].LENH, dma_config[0].LENL, (FRSKY_PACKET_LENGTH+1));
        dma_config[0].SRCINC         = DMA_SRCINC_1;
        dma_config[0].DESTINC        = DMA_DESTINC_0;
    }else{
        // Receiver specific DMA settings:
        // Source: RFD register
        // Destination: radioPktBuffer
        // Use the first byte read + 3 (incl. 2 status bytes)
        // Sets maximum transfer count allowed (length byte + data + 2 status bytes)
        // Data source address is constant
        // Destination address is incremented by 1 byte for each write
        SET_WORD(dma_config[0].SRCADDRH, dma_config[0].SRCADDRL, &X_RFD);
        SET_WORD(dma_config[0].DESTADDRH, dma_config[0].DESTADDRL, &frsky_packet_buffer[0]);
        dma_config[0].VLEN           = DMA_VLEN_FIRST_BYTE_P_3;
        SET_WORD(dma_config[0].LENH, dma_config[0].LENL, (FRSKY_PACKET_LENGTH+3));
        dma_config[0].SRCINC         = DMA_SRCINC_0;
        dma_config[0].DESTINC        = DMA_DESTINC_1;
    }

    // Save pointer to the DMA configuration struct into DMA-channel 0
    // configuration registers
    SET_WORD(DMA0CFGH, DMA0CFGL, &dma_config[0]);

    frsky_packet_received = 0;
}

void frsky_enter_rxmode(uint8_t channel){
    RFST = RFST_SIDLE;

    //set up dma for radio--->buffer
    frsky_setup_rf_dma(FRSKY_MODE_RX);

    //configure interrupt for every received packet
    IEN2 |= (IEN2_RFIE);

    //set highest prio for ch0 (RF)
    IP0 |= (1<<0);
    IP1 |= (1<<0);

    //mask done irq
    RFIM = (1<<4);
    //interrupts should be enabled globally already..
    //skip this! sei();

    //set & do a manual tuning for the given channel
    frsky_tune_channel(channel);

    //start receiving on dma channel 0
    DMAARM = DMA_ARM_CH0;

    //go back to rx mode
    RFST = RFST_SRX;
}

void frsky_autotune(void){
    uint8_t done = 0;
    uint8_t received_packet = 0;
    uint8_t state = 0;
    int8_t fscal0_min=127;
    int8_t fscal0_max=-127;
    int16_t fscal0_calc;

    debug("frsky: autotune\n"); debug_flush();


    //enter RX mode
    frsky_enter_rxmode(0);

    //find best offset:
    storage.frsky_freq_offset = 0;

    debug("frsky: entering bind loop\n"); debug_flush();

    //search for best fscal 0 match
    while(state != 5){
        //reset wdt
        wdt_reset();

        //handle any ovf conditions
        frsky_handle_overflows();

        //debug_put_uint8(state);

        //search full range quickly using binary search
        switch(state){
            default:
            case(0):
                //init left search:
                storage.frsky_freq_offset = -127;
                state = 1;
                break;

            case(1):
                //first search quickly through the full range:
                if (storage.frsky_freq_offset < 127-10){
                    storage.frsky_freq_offset += 9;
                }else{
                    //done one search, did we receive anything?
                    if (received_packet){
                        //finished, go to slow search
                        storage.frsky_freq_offset = fscal0_min - 9;
                        state = 2;
                    }else{
                        //no success, lets try again
                        state = 0;
                    }
                }
                break;

            case(2):
                if (storage.frsky_freq_offset < fscal0_max+9){
                    storage.frsky_freq_offset++;
                }else{
                    //done!
                    state = 5;
                }
                break;
        }

        //go to idle
        RFST = RFST_SIDLE;
        //set freq offset
        FSCTRL0 = storage.frsky_freq_offset;
        //go back to RX:
        delay_ms(1);
        RFST = RFST_SRX;

        //set timeout
        timeout_set(50);
        done = 0;

        LED_GREEN_ON();
        LED_RED_OFF();

        //debug("tune "); debug_put_int8(storage.frsky_freq_offset); debug_put_newline(); debug_flush();

        while((!timeout_timed_out()) && (!done)){
            //handle any ovf conditions
            frsky_handle_overflows();

            if (frsky_packet_received){
                //prepare for next packet:
                frsky_packet_received = 0;
                DMAARM = DMA_ARM_CH0;
                RFST = RFST_SRX;

                //valid packet?
                if (FRSKY_VALID_PACKET_BIND(frsky_packet_buffer)){
                    //bind packet!
                    debug_putc('B');

                    //packet received
                    received_packet = 1;

                    //this fscal value is done
                    done = 1;

                    //update min/max
                    fscal0_min = min(fscal0_min, storage.frsky_freq_offset);
                    fscal0_max = max(fscal0_max, storage.frsky_freq_offset);

                    //make sure we never read the same packet twice by crc flag
                    frsky_packet_buffer[FRSKY_PACKET_BUFFER_SIZE-1] = 0x00;
                }

                /*debug("[");debug_flush();
                for(cnt=0; cnt<FRSKY_PACKET_BUFFER_SIZE; cnt++){
                    debug_hex8(frsky_packet_buffer[cnt]);
                    debug_putc(' ');
                    debug_flush();
                }
                debug("]\n"); debug_flush();*/
            }
        }
        if (!done){
            debug_putc('-');
        }
    }

    //set offset to what we found out to be the best:
    fscal0_calc = (fscal0_max + fscal0_min)/2;

    debug("\nfrsky: fscal0 ");
    debug_put_int8(fscal0_min);
    debug(" - ");
    debug_put_int8(fscal0_max);
    debug_put_newline();
    debug_flush();

    //store new value
    storage.frsky_freq_offset = fscal0_calc;

    RFST = RFST_SIDLE;
    //set freq offset
    FSCTRL0 = storage.frsky_freq_offset;
    //go back to RX:
    delay_ms(1);
    RFST = RFST_SRX;

    debug("frsky: autotune done. offset=");
    debug_put_int8(storage.frsky_freq_offset);
    debug_put_newline();
    debug_flush();
}


uint8_t frsky_bind_jumper_set(void){
    debug("frsky: BIND jumper set = "); debug_flush();
    if (P0 & (1<<SERVO_1)){
        debug("HI -> no binding\n");
        return 0;
    }else{
        debug("LO -> binding\n");
        return 1;
    }
}


void frsky_do_bind(void){
    debug("frsky: do bind\n"); debug_flush();

    //set txid to bind channel
    storage.frsky_txid[0] = 0x03;

    //frequency offset to zero (will do auto tune later on)
    storage.frsky_freq_offset = 0;

    //init txid matching
    frsky_configure_address();

    //set up leds:frsky_txid
    LED_RED_ON();
    LED_GREEN_ON();

    //start autotune:
    frsky_autotune();

    //now run the actual binding:
    frsky_fetch_txid_and_hoptable();

    //important: stop RF interrupts:
    IEN2 &= ~(IEN2_RFIE);
    RFIM = 0;

    //save to persistant storage:
    storage_write_to_flash();

    //done, end up in fancy blink code
    LED_RED_OFF();
    while(1){
        LED_GREEN_ON();
        delay_ms(500);
        wdt_reset();

        LED_GREEN_OFF();
        delay_ms(500);
        wdt_reset();
    }
}


void frsky_handle_overflows(void){
    if ((MARCSTATE & 0x1F) == 0x11){
        debug("frsky: RXOVF\n");
        RFST = RFST_SIDLE;
    }else if ((MARCSTATE & 0x1F) == 0x16){
        debug("frsky: TXOVF\n");
        RFST = RFST_SIDLE;
    }
}

void frsky_fetch_txid_and_hoptable(void){
    uint16_t hopdata_received = 0;
    uint8_t index;
    uint8_t i;

    //enter RX mode
    frsky_enter_rxmode(0);

    #define MAX_BIND_PACKET_COUNT 10
    //DONE when n times a one:
    #define HOPDATA_RECEIVE_DONE ((1<<(MAX_BIND_PACKET_COUNT))-1)

    //clear txid:
    storage.frsky_txid[0] = 0;
    storage.frsky_txid[1] = 0;

    //timeout to wait for packets
    timeout_set(9*3+1);

    //fetch hopdata array
    while(hopdata_received != HOPDATA_RECEIVE_DONE){
        //reset wdt
        wdt_reset();

        //handle any ovf conditions
        frsky_handle_overflows();

        //FIXME: this should be handled in a cleaner way.
        //as this is just for binding, stay with this fix for now...
        if (timeout_timed_out()){
            debug_putc('m');

            //next packet should be ther ein 9ms
            //if no packet for 3*9ms -> reset rx chain:
            timeout_set(3*9+1);

            //re-prepare for next packet:
            RFST = RFST_SIDLE;
            delay_ms(1);
            frsky_packet_received = 0;
            DMAARM = DMA_ARM_CH0;
            RFST = RFST_SRX;
        }

        if (frsky_packet_received){
            debug_putc('p');

            //prepare for next packet:
            frsky_packet_received = 0;
            DMAARM = DMA_ARM_CH0;
            RFST = RFST_SRX;


            #if FRSKY_DEBUG_BIND_DATA
            if (FRSKY_VALID_FRAMELENGTH(frsky_packet_buffer)){
                debug("frsky: RX ");
                debug_flush();
                for(i=0; i<FRSKY_PACKET_BUFFER_SIZE; i++){
                    debug_put_hex8(frsky_packet_buffer[i]);
                    debug_putc(' ');
                }
                debug_put_newline();
            }
            #endif


            //do we know our txid yet?
            if (FRSKY_VALID_PACKET_BIND(frsky_packet_buffer)){
                //next packet should be ther ein 9ms
                //if no packet for 3*9ms -> reset rx chain:
                timeout_set(3*9+1);

                debug_putc('B');
                if ((storage.frsky_txid[0] == 0) && (storage.frsky_txid[1] == 0)){
                    //no! extract this
                    storage.frsky_txid[0] = frsky_packet_buffer[3];
                    storage.frsky_txid[1] = frsky_packet_buffer[4];
                    //debug
                    debug("frsky: got txid 0x");
                    debug_put_hex8(storage.frsky_txid[0]);
                    debug_put_hex8(storage.frsky_txid[1]);
                    debug_put_newline();
                }

                //this is actually for us
                index = frsky_packet_buffer[5];

                //valid bind index?
                if (index/5 < MAX_BIND_PACKET_COUNT){
                    //copy data to our hop list:
                    for(i=0; i<5; i++){
                        if ((index+i) < FRSKY_HOPTABLE_SIZE){
                            storage.frsky_hop_table[index+i] = frsky_packet_buffer[6+i];
                        }
                    }
                    //mark as done: set bit flag for index
                    hopdata_received |= (1<<(index/5));
                }else{
                    debug("frsky: invalid bind idx");
                    debug_put_uint8(index/5);
                    debug_put_newline();
                }

                //make sure we never read the same packet twice by crc flag
                frsky_packet_buffer[FRSKY_PACKET_BUFFER_SIZE-1] = 0x00;
            }
        }
    }

    #if FRSKY_DEBUG_BIND_DATA
    debug("frsky: hop[] = ");
    for(i=0; i<FRSKY_HOPTABLE_SIZE; i++){
        debug_put_hex8(storage.frsky_hop_table[i]);
        debug_putc(' ');
        debug_flush();
    }
    debug_putc('\n');
    #endif

    //idle
    RFST = RFST_SIDLE;
}

void frsky_calib_pll(void){
    uint8_t i;
    uint8_t ch;

    debug("frsky: calib pll\n");

    //fine tune offset
    FSCTRL0 = storage.frsky_freq_offset;

    debug("frsky: tuning hop[] =");

    //calibrate pll for all channels
    for(i=0; i<FRSKY_HOPTABLE_SIZE; i++){
        //reset wdt
        wdt_reset();

        //fetch channel from hop_table:
        ch = storage.frsky_hop_table[i];

        //debug info
        debug_putc(' ');
        debug_put_hex8(ch);

        //set channel number
        frsky_tune_channel(ch);

        //store pll calibration:
        frsky_calib_fscal1_table[i] = FSCAL1;
    }
    debug_put_newline();

    //only needed once:
    frsky_calib_fscal3 = FSCAL3;
    frsky_calib_fscal2 = FSCAL2;

    //return to idle
    RFST = RFST_SIDLE;

    debug("frsky: calib fscal1 = ");
    for(i=0; i<FRSKY_HOPTABLE_SIZE; i++){
        debug_put_hex8(frsky_calib_fscal1_table[i]);
        debug_putc(' ');
        debug_flush();
    }
    debug("\nfrsky: calib fscal2 = 0x");
    debug_put_hex8(frsky_calib_fscal2);
    debug("\nfrsky: calib fscal3 = 0x");
    debug_put_hex8(frsky_calib_fscal3);
    debug_put_newline();
    debug_flush();

    debug("frsky: calib pll done\n");
}


void frsky_set_channel(uint8_t hop_index){
    uint8_t ch = storage.frsky_hop_table[hop_index];
    //debug_putc('S'); debug_put_hex8(ch);

    //go to idle
    RFST = RFST_SIDLE;

    //fetch and set our stored pll calib data:
    FSCAL3 = frsky_calib_fscal3;
    FSCAL2 = frsky_calib_fscal2;
    FSCAL1 = frsky_calib_fscal1_table[hop_index];

    //set channel
    CHANNR = ch;
}


void frsky_main(void){
    uint8_t send_telemetry = 0;
    uint8_t requested_telemetry_id = 0;
    uint8_t missing = 0;
    uint8_t hopcount = 0;
    uint8_t stat_rxcount = 0;
    //uint8_t badrx_test = 0;
    uint8_t conn_lost = 1;
    uint8_t packet_received = 0;
    //uint8_t i;

    debug("frsky: starting main loop\n");

    //start with any channel:
    frsky_current_ch_idx = 0;
    //first set channel uses enter rxmode, this will set up dma etc
    frsky_enter_rxmode(storage.frsky_hop_table[frsky_current_ch_idx]);

    //wait 500ms on the current ch on powerup
    timeout_set(500);

    //start with conn lost (allow full sync)
    conn_lost = 1;
    apa102_show_no_connection();

    //reset wdt once in order to have at least one second waiting for a packet:
    wdt_reset();

    //make sure we never read the same packet twice by crc flag
    frsky_packet_buffer[FRSKY_PACKET_BUFFER_SIZE-1] = 0x00;
conn_lost = 1;
    //start main loop
    while(1){
        if (timeout_timed_out()){
            LED_RED_ON();

            //next hop in 9ms
            if (!conn_lost){
                timeout_set(9);
            }else{
                timeout_set(500);
            }

            frsky_increment_channel(1);

            //strange delay from spi dumps
            delay_us(1000);

            //go back to rx mode
            frsky_packet_received = 0;
            DMAARM = DMA_ARM_CH0;
            RFST = RFST_SRX;

            //if enabled, send a sbus frame in case we lost that frame:
            #if SBUS_ENABLED
            if (!packet_received){
                //frame was lost, so there was no channel value update
                //and no transmission for the last frame slot.
                //therefore we will do a transmission now
                //(frame lost packet flag will be set)
                sbus_start_transmission(SBUS_FRAME_LOST);
            }
            #endif

            //check for packets
            if (packet_received){
                debug_putc('.');
            }else{
                debug_putc('!');
                missing++;
            }
            packet_received = 0;

            if (hopcount++ >= 100){
                debug("STAT: ");
                debug_put_uint8(stat_rxcount);
                debug_put_newline();

                //link quality
                frsky_link_quality = stat_rxcount;

                if (stat_rxcount==0){
                    conn_lost = 1;
                    //enter failsafe mode
                    failsafe_enter();
                    debug("\nCONN LOST!\n");
                    //no connection led info
                    apa102_show_no_connection();
                }

                //statistics
                hopcount = 1;
                stat_rxcount = 0;
            }

            LED_RED_OFF();
        }

        //handle ovfs
        frsky_handle_overflows();

        if (frsky_packet_received){
            //valid packet?
            if (FRSKY_VALID_PACKET(frsky_packet_buffer)){
                //ok, valid packet for us
                LED_GREEN_ON();

                //we hop to the next channel in 0.5ms
                //afterwards hops are in 9ms grid again
                //this way we can have up to +/-1ms jitter on our 9ms timebase
                //without missing packets
                delay_us(500);
                timeout_set(0);

                //reset wdt
                wdt_reset();

                //reset missing packet counter
                missing = 0;

                //every 4th frame is a telemetry frame (transmits every 36ms)
                if ((frsky_packet_buffer[3] % 4) == 2){
                    //next frame is a telemetry frame
                    send_telemetry = 1;
                }
                //always store the last telemtry request id
                requested_telemetry_id   = frsky_packet_buffer[4];

                //stats
                stat_rxcount++;
                packet_received=1;
                conn_lost = 0;

                //extract rssi in frsky format
                frsky_rssi = frsky_extract_rssi(frsky_packet_buffer[FRSKY_PACKET_BUFFER_SIZE-2]);

                //extract channel data:
                frsky_update_ppm();

                //debug_put_hex8(buffer[3]);

                //make sure we never read the same packet twice by crc flag
                frsky_packet_buffer[FRSKY_PACKET_BUFFER_SIZE-1] = 0x00;

                LED_GREEN_OFF();
            }else{
                //mark packet as invalid
                frsky_packet_received = 0;
            }
        }

        if (send_telemetry){
            //set timeout to 9ms grid
            timeout_set(9);

            //change channel:
            frsky_increment_channel(1);

            //DO NOT go to SRX here
            delay_us(900); //1340-500

            //build & send packet
            frsky_send_telemetry(requested_telemetry_id);

            //mark as done
            send_telemetry = 0;
        }

        //process leds:
        apa102_statemachine();

    }

    debug("frsky: main loop ended. THIS SHOULD NEVER HAPPEN!\n");
    while(1);
}

//useful for debugging/sniffing packets from anothe tx or rx
//make sure to bind this rx before using this...
void frsky_frame_sniffer(void){
    uint8_t send_telemetry = 0;
    uint8_t missing = 0;
    uint8_t hopcount = 0;
    uint8_t stat_rxcount = 0;
    uint8_t badrx_test = 0;
    uint8_t conn_lost = 1;
    uint8_t packet_received = 0;
    uint8_t i;

    debug("frsky: entering sniffer mode\n");

    //start with any channel:
    frsky_current_ch_idx = 0;
    //first set channel uses enter rxmode, this will set up dma etc
    frsky_enter_rxmode(storage.frsky_hop_table[frsky_current_ch_idx]);

    //wait 500ms on the current ch on powerup
    timeout_set(500);

    //start with conn lost (allow full sync)
    conn_lost = 1;

    //reset wdt once in order to have at least one second waiting for a packet:
    wdt_reset();

    //start main loop
    while(1){
        if (timeout_timed_out()){
            LED_RED_ON();

            //next hop in 9ms
            if (!conn_lost){
                timeout_set(9);
            }else{
                timeout_set(500);
            }

            frsky_increment_channel(1);

            //strange delay
            //_delay_us(1000);
            delay_us(1000);

            //go back to rx mode
            frsky_packet_received = 0;
            DMAARM = DMA_ARM_CH0;
            RFST = RFST_SRX;

            //check for packets
            if (!packet_received){
                if (send_telemetry){
                    debug("< MISSING\n");
                    send_telemetry = 0;
                }else{
                    debug("> MISSING\n");
                }
                send_telemetry = 0;
                missing++;
            }
            packet_received = 0;

            if (hopcount++ >= 100){
                if (stat_rxcount==0){
                    conn_lost = 1;
                    debug("\nCONN LOST!\n");
                }

                //statistics
                hopcount = 1;
                stat_rxcount = 0;
            }

            LED_RED_OFF();
        }

        //handle ovfs
        frsky_handle_overflows();

        if (frsky_packet_received){
            if (FRSKY_VALID_PACKET(frsky_packet_buffer)){
                //ok, valid packet for us
                LED_GREEN_ON();

                //dump all packets!
                if (send_telemetry){
                    debug("< ");
                    send_telemetry = 0;
                }else{
                    debug("> ");
                }

                for(i=0; i<FRSKY_PACKET_BUFFER_SIZE; i++){
                    debug_put_hex8(frsky_packet_buffer[i]);
                    debug_putc(' ');
                }
                debug("\n");

                //we hop to the next channel in 0.5ms
                //afterwards hops are in 9ms grid again
                //this way we can have up to +/-1ms jitter on our 9ms timebase
                //without missing packets
                delay_us(500);
                timeout_set(0);

                //reset wdt
                wdt_reset();

                //reset missing packet counter
                missing = 0;

                //every 4th frame is a telemetry frame (transmits every 36ms)
                if ((frsky_packet_buffer[3] % 4) == 2){
                    send_telemetry = 1;
                }

                //stats
                stat_rxcount++;
                packet_received=1;
                conn_lost = 0;

                //make sure we never read the same packet twice by crc flag
                frsky_packet_buffer[FRSKY_PACKET_BUFFER_SIZE-1] = 0x00;

                LED_GREEN_OFF();
            }
        }

    }

    debug("frsky: sniffer loop ended. THIS SHOULD NEVER HAPPEN!\n");
    while(1);
}



void frsky_increment_channel(int8_t cnt){
    int8_t next = frsky_current_ch_idx;
    //add increment
    next+=cnt;
    //convert to a safe unsigned number:
    if(next<0){
        next += FRSKY_HOPTABLE_SIZE;
    }
    if (next >= FRSKY_HOPTABLE_SIZE){
        next -= FRSKY_HOPTABLE_SIZE;
    }

    frsky_current_ch_idx = next;
    frsky_set_channel(frsky_current_ch_idx);
}


void frsky_send_telemetry(uint8_t telemetry_id){
    uint8_t i;
    //uint16_t tmp16;
    uint8_t bytes_used = 0;
    static uint8_t test = 0;

    //Stop RX DMA
    RFST = RFST_SIDLE;
    //abort ch0
    DMAARM = DMA_ARM_ABORT | DMA_ARM_CH0;
    frsky_setup_rf_dma(FRSKY_MODE_TX);

    //length of byte (always 0x11 = 17 bytes)
    frsky_packet_buffer[0] = 0x11;
    //txid
    frsky_packet_buffer[1] = storage.frsky_txid[0];
    frsky_packet_buffer[2] = storage.frsky_txid[1];
    //ADC channels
    frsky_packet_buffer[3] = adc_get_scaled(0);
    frsky_packet_buffer[4] = adc_get_scaled(1);
    //RSSI
    frsky_packet_buffer[5] = frsky_rssi;

    //send ampere and voltage as hub telemetry data as well
    #if FRSKY_SEND_HUB_TELEMETRY
        //use telemetry id to decide which packet to send:
        if (telemetry_id & 1){
            //send voltage packet (undocumented sensor 0x39 = volts in 0.1 steps)
            tmp16 = test++; //123; //12.3V
            //convert adc to voltage:
            //float v = (vraw * 3.3/1024.0) * (ADC0_DIVIDER_A + ADC0_DIVIDER_B)) / (ADC0_DIVIDER_B);
            //continue here
            bytes_used = frsky_append_hub_data(FRSKY_HUB_TELEMETRY_VOLTAGE, tmp16, &frsky_packet_buffer[8]);
        }else{
            //send current
            tmp16 = 456; //45.6A
            bytes_used = frsky_append_hub_data(FRSKY_HUB_TELEMETRY_CURRENT, tmp16, &frsky_packet_buffer[8]);
        }

        //number of valid data bytes:
        frsky_packet_buffer[6] = bytes_used;
        //set up frame id
        frsky_packet_buffer[7] = telemetry_id;

        //do not use rest of frame, use only one hub frame per packet
        //in order not to have to handle the datastream splitting operation
    #else
        //no telemetry -> at least[6]+[7] should be zero
        //bytes 6-17 are zero
        for(i=6; i<FRSKY_PACKET_BUFFER_SIZE; i++){
            frsky_packet_buffer[i] = 0x00;
        }
    #endif

    //re arm adc dma etc
    //it is important to call this after reading the values...
    adc_process();

    //arm dma channel
    RFST = RFST_STX;
    DMAARM = DMA_ARM_CH0;

    //tricky: this will force an int request and
    //        initiate the actual transmission
    S1CON |= 0x03;

    //wait some time here. packet should be sent within our 9ms
    //frame (actually within 5-6ms). if not print an error...
    frsky_packet_sent = 0;
    while(!frsky_packet_sent){
        if (timeout_timed_out()){
            break;
        }
    }
    if (timeout_timed_out()){
        debug("\nfrsky: ERROR tx timed out\n");
    }

    frsky_packet_sent = 0;

    frsky_setup_rf_dma(FRSKY_MODE_RX);
    DMAARM = DMA_ARM_CH0;
    RFST = RFST_SRX;
}

uint8_t frsky_extract_rssi(uint8_t rssi_raw){
    #define FRSKY_RSSI_OFFSET 70
    if (rssi_raw >= 128){
        //adapted to fit better to the original values... FIXME: find real formula
        //return (rssi_raw * 18)/32 - 82;
        return ((((uint16_t)rssi_raw) * 18)>>5) - 82;
    }else{
        return ((((uint16_t)rssi_raw) * 18)>>5) + 65;
    }
}

void frsky_update_ppm(void){
    //build uint16_t array from data:
    __xdata uint16_t channel_data[8];

    /*debug("[");debug_flush();
    for(cnt=0; cnt<FRSKY_PACKET_BUFFER_SIZE; cnt++){
        debug_put_hex8(frsky_packet_buffer[cnt]);
        debug_putc(' ');
        debug_flush();
    }
    debug("]\n"); debug_flush();
    */

    //extract channel data from packet:
    channel_data[0] = (uint16_t)(((frsky_packet_buffer[10] & 0x0F)<<8 | frsky_packet_buffer[6]));
    channel_data[1] = (uint16_t)(((frsky_packet_buffer[10] & 0xF0)<<4 | frsky_packet_buffer[7]));
    channel_data[2] = (uint16_t)(((frsky_packet_buffer[11] & 0x0F)<<8 | frsky_packet_buffer[8]));
    channel_data[3] = (uint16_t)(((frsky_packet_buffer[11] & 0xF0)<<4 | frsky_packet_buffer[9]));
    channel_data[4] = (uint16_t)(((frsky_packet_buffer[16] & 0x0F)<<8 | frsky_packet_buffer[12]));
    channel_data[5] = (uint16_t)(((frsky_packet_buffer[16] & 0xF0)<<4 | frsky_packet_buffer[13]));
    channel_data[6] = (uint16_t)(((frsky_packet_buffer[17] & 0x0F)<<8 | frsky_packet_buffer[14]));
    channel_data[7] = (uint16_t)(((frsky_packet_buffer[17] & 0xF0)<<4 | frsky_packet_buffer[15]));

    //set apa leds:
    apa102_update_leds(channel_data, frsky_link_quality);
    apa102_start_transmission();

    //exit failsafe mode
    failsafe_exit();

    //copy to output module:
    #if SBUS_ENABLED
    sbus_update(channel_data);
    sbus_start_transmission(SBUS_FRAME_NOT_LOST);
    #else
    ppm_update(channel_data);
    #endif
}


uint8_t frsky_append_hub_data(uint8_t sensor_id, uint16_t value, uint8_t *buf){
    uint8_t index = 0;
    uint8_t val8;

    //add header:
    buf[index++] = FRSKY_HUB_TELEMETRY_HEADER;
    //add sensor id
    buf[index++] = sensor_id;
    //add data, take care of byte stuffing
    //low byte
    val8 = LO(value);
    if (val8 == 0x5E){
        buf[index++] = 0x5D;
        buf[index++] = 0x3E;
    }else if (val8 == 0x5D){
        buf[index++] = 0x5D;
        buf[index++] = 0x3D;
    }else{
        buf[index++] = val8;
    }
    //high byte
    val8 = HI(value);
    if (val8 == 0x5E){
        buf[index++] = 0x5D;
        buf[index++] = 0x3E;
    }else if (val8 == 0x5D){
        buf[index++] = 0x5D;
        buf[index++] = 0x3D;
    }else{
        buf[index++] = val8;
    }
    //add footer:
    buf[index] = FRSKY_HUB_TELEMETRY_HEADER;

    //return index:
    return index;
}
