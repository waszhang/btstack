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

#define __BTSTACK_FILE__ "goep_client.c"
 
#include "btstack_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack_debug.h"
#include "hci_dump.h"
#include "bluetooth_sdp.h"
#include "btstack_event.h"
#include "classic/goep_client.h"
#include "classic/obex.h"
#include "classic/obex_iterator.h"
#include "classic/rfcomm.h"
#include "classic/sdp_client.h"
#include "classic/sdp_util.h"
#include "l2cap.h"

//------------------------------------------------------------------------------------------------------------
// goep_client.c
//

// #define ENABLE_GOEP_L2CAP

#ifdef ENABLE_GOEP_L2CAP
#ifndef ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE
#error "ENABLE_GOEP_L2CAP requires ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE. Please enable ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE or disable ENABLE_GOEP_L2CAP"
#endif
#endif

typedef enum {
    GOEP_INIT,
    GOEP_W4_SDP,
    GOEP_W4_CONNECTION,
    GOEP_CONNECTED,
} goep_state_t;

typedef struct {
    uint16_t         cid;
    goep_state_t     state;
    bd_addr_t        bd_addr;
    hci_con_handle_t con_handle;
    uint8_t          incoming;
    uint8_t          rfcomm_port;
    uint16_t         l2cap_psm;
    uint16_t         bearer_cid;
    uint16_t         bearer_mtu;
    uint32_t         pbap_supported_features;

    uint8_t          obex_opcode;
    uint32_t         obex_connection_id;
    int              obex_connection_id_set;

    btstack_packet_handler_t client_handler;
} goep_client_t;

static goep_client_t _goep_client;
static goep_client_t * goep_client = &_goep_client;

static uint8_t            attribute_value[30];
static const unsigned int attribute_value_buffer_size = sizeof(attribute_value);

static uint8_t goep_packet_buffer[100];

#ifdef ENABLE_GOEP_L2CAP
static uint8_t ertm_buffer[1000];
static l2cap_ertm_config_t ertm_config = {
    1,  // ertm mandatory
    2,  // max transmit, some tests require > 1
    2000,
    12000,
    512,    // l2cap ertm mtu
    2,
    2,
    0,      // No FCS
};
#endif

static inline void goep_client_emit_connected_event(goep_client_t * context, uint8_t status){
    uint8_t event[15];
    int pos = 0;
    event[pos++] = HCI_EVENT_GOEP_META;
    pos++;  // skip len
    event[pos++] = GOEP_SUBEVENT_CONNECTION_OPENED;
    little_endian_store_16(event,pos,context->cid);
    pos+=2;
    event[pos++] = status;
    memcpy(&event[pos], context->bd_addr, 6);
    pos += 6;
    little_endian_store_16(event,pos,context->con_handle);
    pos += 2;
    event[pos++] = context->incoming;
    event[1] = pos - 2;
    if (pos != sizeof(event)) log_error("goep_client_emit_connected_event size %u", pos);
    context->client_handler(HCI_EVENT_PACKET, context->cid, &event[0], pos);
}   

static inline void goep_client_emit_connection_closed_event(goep_client_t * context){
    uint8_t event[5];
    int pos = 0;
    event[pos++] = HCI_EVENT_GOEP_META;
    pos++;  // skip len
    event[pos++] = GOEP_SUBEVENT_CONNECTION_CLOSED;
    little_endian_store_16(event,pos,context->cid);
    pos+=2;
    event[1] = pos - 2;
    if (pos != sizeof(event)) log_error("goep_client_emit_connection_closed_event size %u", pos);
    context->client_handler(HCI_EVENT_PACKET, context->cid, &event[0], pos);
}   

static inline void goep_client_emit_can_send_now_event(goep_client_t * context){
    uint8_t event[5];
    int pos = 0;
    event[pos++] = HCI_EVENT_GOEP_META;
    pos++;  // skip len
    event[pos++] = GOEP_SUBEVENT_CAN_SEND_NOW;
    little_endian_store_16(event,pos,context->cid);
    pos+=2;
    event[1] = pos - 2;
    if (pos != sizeof(event)) log_error("goep_client_emit_can_send_now_event size %u", pos);
    context->client_handler(HCI_EVENT_PACKET, context->cid, &event[0], pos);
}   

static void goep_client_handle_connection_opened(goep_client_t * context, uint8_t status, uint16_t mtu){
    if (status) {
        context->state = GOEP_INIT;
        log_info("goep_client: open failed, status %u", status);
    } else {
        context->bearer_mtu = mtu;
        context->state = GOEP_CONNECTED;
        log_info("goep_client: connection opened. cid %u, max frame size %u", context->bearer_cid, context->bearer_mtu);
    }
    goep_client_emit_connected_event(context, status);
}

static void goep_client_handle_connection_close(goep_client_t * context){
    context->state = GOEP_INIT;
    goep_client_emit_connection_closed_event(context);
}

static void goep_client_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    goep_client_t * context = goep_client;
    switch (packet_type){
        case HCI_EVENT_PACKET:
            switch (hci_event_packet_get_type(packet)) {
#ifdef ENABLE_GOEP_L2CAP
                case L2CAP_EVENT_CHANNEL_OPENED:
                    goep_client_handle_connection_opened(context, l2cap_event_channel_opened_get_status(packet),
                        btstack_min(l2cap_event_channel_opened_get_remote_mtu(packet), l2cap_event_channel_opened_get_local_mtu(packet)));
                    return;
                case L2CAP_EVENT_CAN_SEND_NOW:
                    goep_client_emit_can_send_now_event(context);
                    break;
                case L2CAP_EVENT_CHANNEL_CLOSED:
                    goep_client_handle_connection_close(context);
                    break;
#endif
                case RFCOMM_EVENT_CHANNEL_OPENED:
                    goep_client_handle_connection_opened(context, rfcomm_event_channel_opened_get_status(packet), rfcomm_event_channel_opened_get_max_frame_size(packet));
                    return;
                case RFCOMM_EVENT_CAN_SEND_NOW:
                    goep_client_emit_can_send_now_event(context);
                    break;
                case RFCOMM_EVENT_CHANNEL_CLOSED:
                    goep_client_handle_connection_close(context);
                    break;
                default:
                    break;
            }
            break;
        case L2CAP_DATA_PACKET:
        case RFCOMM_DATA_PACKET:
            context->client_handler(GOEP_DATA_PACKET, context->cid, packet, size);
            break;
        default:
            break;
    }
}

static void goep_client_handle_sdp_query_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    goep_client_t * context = goep_client;

    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    des_iterator_t des_list_it;
    des_iterator_t prot_it;
    uint8_t status;


    switch (hci_event_packet_get_type(packet)){
        case SDP_EVENT_QUERY_ATTRIBUTE_VALUE:

            // check if relevant attribute
            switch(sdp_event_query_attribute_byte_get_attribute_id(packet)){
                case BLUETOOTH_ATTRIBUTE_PROTOCOL_DESCRIPTOR_LIST:
                case BLUETOOTH_ATTRIBUTE_PBAP_SUPPORTED_FEATURES:
#ifdef ENABLE_GOEP_L2CAP
                case BLUETOOTH_ATTRIBUTE_GOEP_L2CAP_PSM:
#endif
                    break;
                default:
                    return;
            }

            // warn if attribute too large to fit in our buffer
            if (sdp_event_query_attribute_byte_get_attribute_length(packet) > attribute_value_buffer_size) {
                log_error("SDP attribute value size exceeded for attribute %x: available %d, required %d", sdp_event_query_attribute_byte_get_attribute_id(packet), attribute_value_buffer_size, sdp_event_query_attribute_byte_get_attribute_length(packet));
                break;
            }

            // store single byte
            attribute_value[sdp_event_query_attribute_byte_get_data_offset(packet)] = sdp_event_query_attribute_byte_get_data(packet);

            // wait until value fully received
            if ((uint16_t)(sdp_event_query_attribute_byte_get_data_offset(packet)+1) != sdp_event_query_attribute_byte_get_attribute_length(packet)) break;

            // process attributes
            switch(sdp_event_query_attribute_byte_get_attribute_id(packet)) {
                case BLUETOOTH_ATTRIBUTE_PROTOCOL_DESCRIPTOR_LIST:
                    for (des_iterator_init(&des_list_it, attribute_value); des_iterator_has_more(&des_list_it); des_iterator_next(&des_list_it)) {
                        uint8_t       *des_element;
                        uint8_t       *element;
                        uint32_t       uuid;
#ifdef ENABLE_GOEP_L2CAP
                        uint16_t       l2cap_psm;
#endif

                        if (des_iterator_get_type(&des_list_it) != DE_DES) continue;

                        des_element = des_iterator_get_element(&des_list_it);
                        des_iterator_init(&prot_it, des_element);

                        // first element is UUID
                        element = des_iterator_get_element(&prot_it);
                        if (de_get_element_type(element) != DE_UUID) continue;

                        uuid = de_get_uuid32(element);
                        des_iterator_next(&prot_it);
                        if (!des_iterator_has_more(&prot_it)) continue;

                        // second element is RFCOMM server channel or L2CAP PSM
                        element = des_iterator_get_element(&prot_it);
                        switch (uuid){
#ifdef ENABLE_GOEP_L2CAP
                            case BLUETOOTH_PROTOCOL_L2CAP:
                                if (de_element_get_uint16(element, &l2cap_psm)){
                                    context->l2cap_psm = l2cap_psm;
                                }
                                break;
#endif
                            case BLUETOOTH_PROTOCOL_RFCOMM:
                                context->rfcomm_port = element[de_get_header_size(element)];
                                break;
                            default:
                                break;
                        }
                    }
                    break;
#ifdef ENABLE_GOEP_L2CAP
                case BLUETOOTH_ATTRIBUTE_GOEP_L2CAP_PSM:
                    de_element_get_uint16(attribute_value, &context->l2cap_psm);
                    break;
#endif
                case BLUETOOTH_ATTRIBUTE_PBAP_SUPPORTED_FEATURES:
                    if (de_get_element_type(attribute_value) != DE_UINT) break;
                    if (de_get_size_type(attribute_value)    != DE_SIZE_32) break;
                    context->pbap_supported_features  = big_endian_read_32(attribute_value, de_get_header_size(attribute_value));
                    log_info("pbap_supported_features 0x%x", context->pbap_supported_features);
                    break;
                default:
                    break;
            }
            break;

        case SDP_EVENT_QUERY_COMPLETE:
            status = sdp_event_query_complete_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                log_info("GOEP client, SDP query failed 0x%02x", status);
                context->state = GOEP_INIT;
                goep_client_emit_connected_event(goep_client, status);
                break;
            }
            if (context->rfcomm_port == 0 && context->l2cap_psm == 0){
                log_info("No GOEP RFCOMM or L2CAP server found");
                context->state = GOEP_INIT;
                goep_client_emit_connected_event(goep_client, ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                break;
            }
#ifdef ENABLE_GOEP_L2CAP
            if (context->l2cap_psm){
                log_info("Remote GOEP L2CAP PSM: %u", context->l2cap_psm);
                l2cap_create_ertm_channel(&goep_client_packet_handler, context->bd_addr, context->l2cap_psm,
                                          &ertm_config, ertm_buffer, sizeof(ertm_buffer), &context->bearer_cid);
                return;
            }
#endif
            log_info("Remote GOEP RFCOMM Server Channel: %u", context->rfcomm_port);
            rfcomm_create_channel(&goep_client_packet_handler, context->bd_addr, context->rfcomm_port, &context->bearer_cid);
    }
}

static uint8_t * goep_client_get_outgoing_buffer(goep_client_t * context){
    if (context->l2cap_psm){
        return goep_packet_buffer;
    } else {
        return rfcomm_get_outgoing_buffer();
    }
}

static void goep_client_packet_append(const uint8_t * data, uint16_t len){
     goep_client_t * context = goep_client;
     uint8_t * buffer = goep_client_get_outgoing_buffer(context);
     uint16_t pos = big_endian_read_16(buffer, 1);
     memcpy(&buffer[pos], data, len);
     pos += len;
     big_endian_store_16(buffer, 1, pos);
}

static void goep_client_packet_init(uint16_t goep_cid, uint8_t opcode){
    UNUSED(goep_cid);
    goep_client_t * context = goep_client;
    if (context->l2cap_psm){
    } else {
        rfcomm_reserve_packet_buffer();
    }
    uint8_t * buffer = goep_client_get_outgoing_buffer(context);
    buffer[0] = opcode;
    big_endian_store_16(buffer, 1, 3);
    // store opcode for parsing of response
    context->obex_opcode = opcode;
}

static void goep_client_packet_add_connection_id(uint16_t goep_cid){
    UNUSED(goep_cid);
    goep_client_t * context = goep_client;
    // add connection_id header if set, must be first header if used
    if (context->obex_connection_id != OBEX_CONNECTION_ID_INVALID){
        goep_client_add_word_header(goep_cid, OBEX_HEADER_CONNECTION_ID, context->obex_connection_id);
    }
}

void goep_client_init(void){
    memset(goep_client, 0, sizeof(goep_client_t));
    goep_client->state = GOEP_INIT;
    goep_client->cid = 1;
    goep_client->obex_connection_id = OBEX_CONNECTION_ID_INVALID;
}

uint8_t goep_client_create_connection(btstack_packet_handler_t handler, bd_addr_t addr, uint16_t uuid, uint16_t * out_cid){
    goep_client_t * context = goep_client;
    if (context->state != GOEP_INIT) return BTSTACK_MEMORY_ALLOC_FAILED;
    context->client_handler = handler;
    context->state = GOEP_W4_SDP;
    context->l2cap_psm   = 0;
    context->rfcomm_port = 0;
    context->pbap_supported_features = PBAP_FEATURES_NOT_PRESENT;
    memcpy(context->bd_addr, addr, 6);
    sdp_client_query_uuid16(&goep_client_handle_sdp_query_event, context->bd_addr, uuid);
    *out_cid = context->cid;
    return 0;
}

uint32_t goep_client_get_pbap_supported_features(uint16_t goep_cid){
    UNUSED(goep_cid);
    goep_client_t * context = goep_client;
    return context->pbap_supported_features;
}

uint8_t goep_client_disconnect(uint16_t goep_cid){
    UNUSED(goep_cid);
    goep_client_t * context = goep_client;
    rfcomm_disconnect(context->bearer_cid);
    return 0;
}

void goep_client_set_connection_id(uint16_t goep_cid, uint32_t connection_id){
    UNUSED(goep_cid);
    goep_client_t * context = goep_client;
    context->obex_connection_id = connection_id;
}

uint8_t goep_client_get_request_opcode(uint16_t goep_cid){
    UNUSED(goep_cid);
    goep_client_t * context = goep_client;
    return context->obex_opcode;
}

void goep_client_request_can_send_now(uint16_t goep_cid){
    UNUSED(goep_cid);
    goep_client_t * context = goep_client;
    if (context->l2cap_psm){
        l2cap_request_can_send_now_event(context->bearer_cid);
    } else {
        rfcomm_request_can_send_now_event(context->bearer_cid);
    }
}

void goep_client_create_connect_request(uint16_t goep_cid, uint8_t obex_version_number, uint8_t flags, uint16_t maximum_obex_packet_length){
    UNUSED(goep_cid);
    goep_client_t * context = goep_client;
    goep_client_packet_init(goep_cid, OBEX_OPCODE_CONNECT);
    uint8_t fields[4];
    fields[0] = obex_version_number;
    fields[1] = flags;
    // workaround: limit OBEX packet len to L2CAP/RFCOMM MTU to avoid handling of fragemented packets
    maximum_obex_packet_length = btstack_min(maximum_obex_packet_length, context->bearer_mtu);
    big_endian_store_16(fields, 2, maximum_obex_packet_length);
    goep_client_packet_append(&fields[0], sizeof(fields));
}

void goep_client_create_get_request(uint16_t goep_cid){
    goep_client_packet_init(goep_cid, OBEX_OPCODE_GET | OBEX_OPCODE_FINAL_BIT_MASK);
    goep_client_packet_add_connection_id(goep_cid);
}

void goep_client_create_put_request(uint16_t goep_cid){
    goep_client_packet_init(goep_cid, OBEX_OPCODE_PUT | OBEX_OPCODE_FINAL_BIT_MASK);
    goep_client_packet_add_connection_id(goep_cid);
}

void goep_client_create_set_path_request(uint16_t goep_cid, uint8_t flags){
    UNUSED(goep_cid);
    goep_client_packet_init(goep_cid, OBEX_OPCODE_SETPATH);
    uint8_t fields[2];
    fields[0] = flags;
    fields[1] = 0;  // reserved
    goep_client_packet_append(&fields[0], sizeof(fields));
    goep_client_packet_add_connection_id(goep_cid);
}

void goep_client_create_abort_request(uint16_t goep_cid){
    goep_client_packet_init(goep_cid, OBEX_OPCODE_ABORT);
    goep_client_packet_add_connection_id(goep_cid);
}

void goep_client_create_disconnect_request(uint16_t goep_cid){
    UNUSED(goep_cid);
    goep_client_packet_init(goep_cid, OBEX_OPCODE_DISCONNECT);
    goep_client_packet_add_connection_id(goep_cid);
}

void goep_client_add_variable_header(uint16_t goep_cid, uint8_t header_type, uint16_t header_data_length, const uint8_t * header_data){
    UNUSED(goep_cid);
    uint8_t header[3];
    header[0] = header_type;
    big_endian_store_16(header, 1, sizeof(header) + header_data_length);
    goep_client_packet_append(&header[0], sizeof(header));
    goep_client_packet_append(header_data, header_data_length);
}

void goep_client_add_byte_header(uint16_t goep_cid, uint8_t header_type, uint8_t value){
    UNUSED(goep_cid);
    uint8_t header[2];
    header[0] = header_type;
    header[1] = value;
    goep_client_packet_append(&header[0], sizeof(header));
}

void goep_client_add_word_header(uint16_t goep_cid, uint8_t header_type, uint32_t value){
    UNUSED(goep_cid);
    uint8_t header[5];
    header[0] = header_type;
    big_endian_store_32(header, 1, value);
    goep_client_packet_append(&header[0], sizeof(header));
}

void goep_client_add_header_srm_enable(uint16_t goep_cid){
    goep_client_add_byte_header(goep_cid, OBEX_HEADER_SINGLE_RESPONSE_MODE, OBEX_SRM_ENABLE);
}

void goep_client_add_header_target(uint16_t goep_cid, uint16_t length, const uint8_t * target){
    goep_client_add_variable_header(goep_cid, OBEX_HEADER_TARGET, length,  target);
}

void goep_client_add_header_application_parameters(uint16_t goep_cid, uint16_t length, const uint8_t * data){
    goep_client_add_variable_header(goep_cid, OBEX_HEADER_APPLICATION_PARAMETERS, length,  data);
}

void goep_client_add_header_challenge_response(uint16_t goep_cid, uint16_t length, const uint8_t * data){
    goep_client_add_variable_header(goep_cid, OBEX_HEADER_AUTHENTICATION_RESPONSE, length,  data);
}

void goep_client_add_body_static(uint16_t goep_cid,  uint32_t length, const uint8_t * data){
    UNUSED(goep_cid);
    goep_client_add_variable_header(goep_cid, OBEX_HEADER_END_OF_BODY, length,  data);
}

void goep_client_add_header_name(uint16_t goep_cid, const char * name){
    UNUSED(goep_cid);
    goep_client_t * context = goep_client;
    int len = strlen(name); 
    if (len) {
        // empty string does not have trailing \0
        len++;
    }
    uint8_t * buffer = goep_client_get_outgoing_buffer(context);
    uint16_t pos = big_endian_read_16(buffer, 1);
    buffer[pos++] = OBEX_HEADER_NAME;
    big_endian_store_16(buffer, pos, 1 + 2 + len*2);
    pos += 2;
    int i;
    // @note name[len] == 0 
    for (i = 0 ; i < len ; i++){
        buffer[pos++] = 0;
        buffer[pos++] = *name++;
    }
    big_endian_store_16(buffer, 1, pos);
 }

void goep_client_add_header_type(uint16_t goep_cid, const char * type){
    UNUSED(goep_cid);
    uint8_t header[3];
    header[0] = OBEX_HEADER_TYPE;
    int len_incl_zero = strlen(type) + 1;
    big_endian_store_16(header, 1, 1 + 2 + len_incl_zero);
    goep_client_packet_append(&header[0], sizeof(header));
    goep_client_packet_append((const uint8_t*)type, len_incl_zero);
}

int goep_client_execute(uint16_t goep_cid){
    UNUSED(goep_cid);
    goep_client_t * context = goep_client;
    uint8_t * buffer = goep_client_get_outgoing_buffer(context);
    uint16_t pos = big_endian_read_16(buffer, 1);
    if (context->l2cap_psm){
        return l2cap_send(context->bearer_cid, buffer, pos);
    } else {
        return rfcomm_send_prepared(context->bearer_cid, pos);
    }
}

