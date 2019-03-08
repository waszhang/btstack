/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define __BTSTACK_FILE__ "map_client_test.c"

// *****************************************************************************
/* EXAMPLE_START(pbap_client_demo): Connect to Phonebook Server and get contacts.
 *
 * @text Note: The Bluetooth address of the remote Phonbook server is hardcoded. 
 * Change it before running example, then use the UI to connect to it, to set and 
 * query contacts.
 */
// *****************************************************************************


#include "btstack_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack_run_loop.h"
#include "l2cap.h"
#include "classic/rfcomm.h"
#include "btstack_event.h"
#include "classic/goep_client.h"
#include "map_client.h"

#ifdef HAVE_BTSTACK_STDIN
#include "btstack_stdin.h"
#endif

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static bd_addr_t    remote_addr;
// MBP2016 "F4-0F-24-3B-1B-E1"
// Nexus 7 "30-85-A9-54-2E-78"
// iPhone SE "BC:EC:5D:E6:15:03"
// PTS "001BDC080AA5"
// iPhone 5 static  char * remote_addr_string = "6C:72:E7:10:22:EE";
// Android
static  char * remote_addr_string = "a0:28:ed:04:33:b0";

static btstack_packet_callback_registration_t hci_event_callback_registration;
static uint16_t map_cid;

#ifdef HAVE_BTSTACK_STDIN
// Testig User Interface 
static void show_usage(void){
    bd_addr_t iut_address;
    gap_local_bd_addr(iut_address);

    printf("\n--- Bluetooth MAP Client Test Console %s ---\n", bd_addr_to_str(iut_address));
    printf("\n");
    printf("a - establish MAP connection to %s\n", bd_addr_to_str(remote_addr));
    printf("A - disconnect from %s\n", bd_addr_to_str(remote_addr));
    printf("f - get folder listing\n");
    printf("\n");
}

static void stdin_process(char c){
    switch (c){
        case 'a':
            printf("[+] Connecting to %s...\n", bd_addr_to_str(remote_addr));
            map_client_connect(&packet_handler, remote_addr, &map_cid);
            break;
        case 'A':
            printf("[+] Disconnect from %s...\n", bd_addr_to_str(remote_addr));
            map_client_disconnect(map_cid);
            break;

        case 'f':
            printf("[+] Get folder listing from %s...\n", bd_addr_to_str(remote_addr));
            map_client_get_folder_listing(map_cid);
            break;
        
        default:
            show_usage();
            break;
    }
}

// packet handler for interactive console
static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    int i;
    uint8_t status;
    // char buffer[32];
    switch (packet_type){
        case HCI_EVENT_PACKET:
            switch (hci_event_packet_get_type(packet)) {
                case BTSTACK_EVENT_STATE:
                    // BTstack activated, get started 
                    if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING){
                        show_usage();
                    }
                    break;
                case HCI_EVENT_MAP_META:
                    switch (hci_event_map_meta_get_subevent_code(packet)){
                        case MAP_SUBEVENT_CONNECTION_OPENED:
                            status = map_subevent_connection_opened_get_status(packet);
                            if (status){
                                printf("[!] Connection failed, status 0x%02x\n", status);
                            } else {
                                printf("[+] Connected\n");
                            }
                            break;
                        case MAP_SUBEVENT_CONNECTION_CLOSED:
                            printf("[+] Connection closed\n");
                            break;
                        case MAP_SUBEVENT_OPERATION_COMPLETED:
                            printf("[+] Operation complete\n");
                            break;
                        
                        default:
                            break;
                    }
                    break;
                default:
                    break;
            }
            break;
        case MAP_DATA_PACKET:
            for (i=0;i<size;i++){
                printf("%c", packet[i]);
            }
            break;
        default:
            break;
    }
}
#endif

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){

    (void)argc;
    (void)argv;
        
    // init L2CAP
    l2cap_init();

    // init RFCOM
    rfcomm_init();

    // init GOEP Client
    goep_client_init();

    // init PBAP Client
    map_client_init();

    // register for HCI events
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sscanf_bd_addr(remote_addr_string, remote_addr);

#ifdef HAVE_BTSTACK_STDIN
    btstack_stdin_setup(stdin_process);
#endif    

    // turn on!
    hci_power_control(HCI_POWER_ON);

    return 0;
}
/* EXAMPLE_END */
