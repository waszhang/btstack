#define __BTSTACK_FILE__ "btstack_chipset_intel_firmware.c"

#include <fcntl.h>
#include <unistd.h>

#include "btstack_chipset_intel_firmware.h"
#include "hci_cmd.h"
#include "bluetooth.h"
#include "hci_dump.h"
#include "btstack_event.h"
#include "btstack_debug.h"
#include "btstack_util.h"

const hci_transport_t * transport;

static int state = 0;

static uint8_t hci_outgoing[300];
static uint8_t fw_buffer[300];

static int fw_fd;
static uint32_t fw_offset;

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

typedef struct {
    uint8_t     status;
    uint8_t     otp_format;
    uint8_t     otp_content;
    uint8_t     otp_patch;
    uint16_t    dev_revid;
    uint8_t     secure_boot;
    uint8_t     key_from_hdr;
    uint8_t     key_type;
    uint8_t     otp_lock;
    uint8_t     api_lock;
    uint8_t     debug_lock;
    bd_addr_t   otp_bdaddr;
    uint8_t     min_fw_build_nn;
    uint8_t     min_fw_build_cw;
    uint8_t     min_fw_build_yy;
    uint8_t     limited_cce;
    uint8_t     unlocked_state;
} intel_boot_params_t;

static const hci_cmd_t hci_intel_read_version = {
    0xfc05, ""
};
static const hci_cmd_t hci_intel_read_secure_boot_params = {
    0xfc0d, ""
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

static int transport_send_intel_secure(uint8_t fragment_type, const uint8_t * data, uint16_t len){
    little_endian_store_16(hci_outgoing, 0, 0xfc09);
    hci_outgoing[2] = 1 + len;
    hci_outgoing[3] = fragment_type;
    memcpy(&hci_outgoing[4], data, len);
    uint16_t size = 3 +  1 + len;
    hci_dump_packet(HCI_ACL_DATA_PACKET, 0, hci_outgoing, size);
    return transport->send_packet(HCI_ACL_DATA_PACKET, hci_outgoing, size);
}

static void state_machine(uint8_t * packet);

// read data from fw file and send it via intel_secure + update state
static int intel_send_fragment(uint8_t fragment_type, uint16_t len){
    int res = read(fw_fd, fw_buffer, len);
    log_info("offset %6u, read %3u -> res %d", fw_offset, len, res);
    fw_offset += res;
    state++;
    return transport_send_intel_secure(fragment_type, fw_buffer, len);
}

static void state_machine(uint8_t * packet){
    intel_version_t     * version;
    intel_boot_params_t * boot_params;
    uint16_t dev_revid;
    bd_addr_t addr;
    char fw_name[30];
    int res;

    switch (state){
        case 0:
            transport_send_cmd(&hci_reset);
            state++;
            break;
        case 1:
            // Read Intel Version
            transport_send_cmd(&hci_intel_read_version);
            state++;
            break;
        case 2:
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
            // only supported hw_platform = 0x37
            // only supported hw_variant  = 0x0b
            // fw_variant = 0x06 bootloader mode / 0x23 operational mode
            if (version->fw_variant != 0x06) break;
            // Read Intel Secure Boot Params
            transport_send_cmd(&hci_intel_read_secure_boot_params);
            state++;
            break;
        case 3:
            boot_params = (intel_boot_params_t *) hci_event_command_complete_get_return_parameters(packet);
            dev_revid = little_endian_read_16((uint8_t*)&boot_params->dev_revid, 0);
            reverse_bd_addr(boot_params->otp_bdaddr, addr);
            log_info("Device revision: %u", dev_revid);
            log_info("Secure Boot:  %s", boot_params->secure_boot ? "enabled" : "disabled");
            log_info("OTP lock:     %s", boot_params->otp_lock    ? "enabled" : "disabled");
            log_info("API lock:     %s", boot_params->api_lock    ? "enabled" : "disabled");
            log_info("Debug lock:   %s", boot_params->debug_lock  ? "enabled" : "disabled");
            log_info("Minimum firmware build %u week %u %u", boot_params->min_fw_build_nn, boot_params->min_fw_build_cw, 2000 + boot_params->min_fw_build_yy);
            log_info("OTC BD_ADDR:  %s", bd_addr_to_str(addr));
            // commmmand complete is required 
            if (boot_params->limited_cce != 0) break;

            // firmware file
            snprintf(fw_name, sizeof(fw_name), "ibt-11-%u.sfi", dev_revid);
            log_info("Open firmware %s", fw_name);

            // open firmware file
            fw_fd = open(fw_name, O_RDONLY);
            if (fw_fd < 0){
                log_error("can't open file %s", fw_name);
                return;
            }

            // send CCS segment - offset 0
            intel_send_fragment(0x00, 128);
            break;
        case 4:
            // send public key / part 1 - offset 128
            intel_send_fragment(0x03, 128);
            break;
        case 5:
            // send public key / part 2 - offset 384
            intel_send_fragment(0x03, 128);
            break;
        case 6:
            // skip 4 bytes
            res = read(fw_fd, fw_buffer, 4);
            log_info("read res %d", res);
            fw_offset += res;

            // send signature / part 1 - offset 388
            intel_send_fragment(0x02, 128);
            break;
        case 7:
            // send signature / part 2 - offset 516
            intel_send_fragment(0x02, 128);
            break;
        case 8:
            // send firmware chunks - offset 644
            res = read(fw_fd, fw_buffer, 3);
            log_info("read command, res %d", res);
            if (res > 0 ){
                int param_len = fw_buffer[2];
                if (param_len){
                    res = read(fw_fd, &fw_buffer[3], param_len);
                }
                transport_send_intel_secure(0x01, fw_buffer, param_len+3);
                break;
            }

            // EOF
            /* fall through */

        default:

            state++;
            log_info("End of file");
            close(fw_fd);
            break;
    }    
}

static void transport_packet_handler (uint8_t packet_type, uint8_t *packet, uint16_t size){
    hci_dump_packet(packet_type, 1, packet, size);
    // if (packet_type != HCI_EVENT_PACKET) return;
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
