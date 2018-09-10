#include "btstack_chipset_intel_firmware.h"
#include "hci_cmd.h"
#include "bluetooth.h"
#include "hci_dump.h"

const hci_transport_t * transport;

static uint8_t hci_outgoing[300];

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

static void transport_packet_handler (uint8_t packet_type, uint8_t *packet, uint16_t size){
    hci_dump_packet(packet_type, 1, packet, size);
}

void btstack_chipset_intel_download_firmware(const hci_transport_t * hci_transport, void (*done)(int result)){
	(void) done;

	transport = hci_transport;;

    // transport->init(NULL);
    transport->register_packet_handler(&transport_packet_handler);
    transport->open();

    // send first command
    transport_send_cmd(&hci_reset);

}
