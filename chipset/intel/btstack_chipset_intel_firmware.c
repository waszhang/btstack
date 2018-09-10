#include "btstack_chipset_intel_firmware.h"
#include "hci_cmd.h"
#include "bluetooth.h"
#include "hci_dump.h"
#include "btstack_event.h"
#include "btstack_debug.h"

const hci_transport_t * transport;

static int state = 0;

static uint8_t hci_outgoing[300];

typedef struct {
    uint8_t status;
    uint8_t hw_platform;
    uint8_t hw_variant;
    uint8_t hw_revision;
    uint8_t fw_variant;
    uint8_t fw_revision;
    uint8_t fw_build_num;
    uint8_t fw_build_ww;
    uint8_t fw_build_yy;
    uint8_t fw_patch_num;
} intel_version_t;

static const hci_cmd_t hci_intel_read_version = {
    0xfc05, ""
};

static int transport_send_cmd_va_arg(const hci_cmd_t *cmd, va_list argptr){
    uint8_t * packet = hci_outgoing;
    uint16_t size = hci_cmd_create_from_template(packet, cmd, argptr);
    hci_dump_packet(HCI_COMMAND_DATA_PACKET, 0, packet, size);
    return transport->send_packet(HCI_COMMAND_DATA_PACKET, packet, size);
}

static int transport_send_cmd(const hci_cmd_t *cmd, ...){
    va_list argptr;
    va_start(argptr, cmd);
    int res = transport_send_cmd_va_arg(cmd, argptr);
    va_end(argptr);
    return res;
}

static void state_machine(uint8_t * packet){
    intel_version_t * version;
    switch (state){
        case 0:
            transport_send_cmd(&hci_reset);
            state++;
            break;
        case 1:
            transport_send_cmd(&hci_intel_read_version);
            state++;
            break;
        default:
            version = (intel_version_t*) hci_event_command_complete_get_return_parameters(packet);
            log_info("status       0x%02x", version->status);
            log_info("hw_platform  0x%02x", version->hw_platform);
            log_info("hw_variant   0x%02x", version->hw_variant);
            log_info("hw_revision  0x%02x", version->hw_revision);
            log_info("fw_variant   0x%02x", version->fw_variant);
            log_info("fw_revision  0x%02x", version->fw_revision);
            log_info("fw_build_num 0x%02x", version->fw_build_num);
            log_info("fw_build_ww  0x%02x", version->fw_build_ww);
            log_info("fw_build_yy  0x%02x", version->fw_build_yy);
            log_info("fw_patch_num 0x%02x", version->fw_patch_num);
            break;
    }    
}

static void transport_packet_handler (uint8_t packet_type, uint8_t *packet, uint16_t size){
    hci_dump_packet(packet_type, 1, packet, size);
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)){
        case HCI_EVENT_COMMAND_COMPLETE:
            state_machine(packet);
            break;
        default:
            break;
    }
}

void btstack_chipset_intel_download_firmware(const hci_transport_t * hci_transport, void (*done)(int result)){
	(void) done;

	transport = hci_transport;;

    // transport->init(NULL);
    transport->register_packet_handler(&transport_packet_handler);
    transport->open();

    // get started
    state = 0;
    state_machine(NULL);
}
