#include "private_user_packet_handler.h"

#include "btstack_event.h"
#include "user_packet_handler.h"
#include "ad_parser.h"
#include "service/transmission_service.h"
#include "service/transmission_callback.h"
#include "gatt_client.h"
#include "att_dispatch.h"
#include "btstack_defines.h"


#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "timers.h"

#include "common/flash_data.h"

#include "util/utils.h"
#include "util/buffer.h"
#include "util/rtos_util.h"

#include "uart_io.h"
#include "util/circular_queue.h"
#include "at/at_parser.h"

#include "router.h"

extern private_flash_data_t g_power_off_save_data_in_ram;

extern uint8_t send_start_flag;

//==============================================================================================================
//* Global variable
//==============================================================================================================
hci_con_handle_t master_connect_handle = INVALID_HANDLE;
hci_con_handle_t slave_connect_handle = INVALID_HANDLE;

static uint8_t g_rx_data_passthrough_to_ble_en = BT_PRIVT_DISABLE;



static at_processor_t at_processor;
//==============================================================================================================



volatile uint8_t send_slave_flow_control_to_be_continued = 0;
volatile uint8_t send_master_flow_control_to_be_continued = 0;
extern volatile uint8_t send_to_slave_data_to_be_continued;
extern volatile uint8_t send_to_master_data_to_be_continued;


//==============================================================================================================
//* GATT read/write callback
//==============================================================================================================
extern uint8_t notify_enable;
extern uint16_t g_ble_input_handle;
extern uint16_t g_ble_output_handle;
extern uint16_t g_ble_output_desc_handle;
extern uint16_t g_ble_flow_ctrl_handle;
//==============================================================================================================



extern uint8_t send_to_master_flow_control_enable;
extern uint8_t send_to_slave_flow_control_enable;
extern uint8_t recv_from_master_flow_control_enable;
extern uint8_t recv_from_slave_flow_control_enable;



uint32_t rx_sum = 0;
uint32_t tx_sum = 0;
uint32_t receive_slave_sum = 0;
uint32_t receive_master_sum = 0;
uint32_t send_to_slave_sum = 0;
uint32_t send_to_master_sum = 0;








//==============================================================================================================
//* GATT discovery
//==============================================================================================================
gatt_client_service_t slave_service;
gatt_client_characteristic_t slave_input_char;
gatt_client_characteristic_t slave_output_char;
gatt_client_characteristic_descriptor_t slave_output_desc;
gatt_client_characteristic_t slave_flow_ctrl_char;          // flow ctrl
gatt_client_notification_t slave_output_notify;

gatt_client_service_t master_service;
gatt_client_characteristic_t master_flow_ctrl_char;         // flow ctrl

static uint16_t char_config_notification = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION;
static uint16_t char_config_none = 0;



static void output_notification_handler(uint8_t packet_type, uint16_t _, const uint8_t *packet, uint16_t size)
{
    const gatt_event_value_packet_t *value_packet;
    uint16_t value_size;
    switch (packet[0])
    {
    case GATT_EVENT_NOTIFICATION:
        value_packet = gatt_event_notification_parse(packet, size, &value_size);
    
        receive_ble_slave_data((uint8_t*)&value_packet->value, value_size);
        
        notify_slave_port_has_data();
    
        break;
    }
}

void config_notify_callback(uint8_t packet_type, uint16_t channel, const uint8_t *packet, uint16_t size)
{
    switch (packet[0])
    {
    case GATT_EVENT_QUERY_COMPLETE:
        if (gatt_event_query_complete_parse(packet)->status != 0)
            return;
        LOG_INFO("cmpl");
        break;
    }
}





void descriptor_discovery_callback(uint8_t packet_type, uint16_t _, const uint8_t *packet, uint16_t size)
{
    switch (packet[0])
    {
    case GATT_EVENT_ALL_CHARACTERISTIC_DESCRIPTORS_QUERY_RESULT:
        {
            const gatt_event_all_characteristic_descriptors_query_result_t *result =
                gatt_event_all_characteristic_descriptors_query_result_parse(packet);
            if (get_sig_short_uuid(result->descriptor.uuid128) ==
                SIG_UUID_DESCRIP_GATT_CLIENT_CHARACTERISTIC_CONFIGURATION)
            {
                slave_output_desc = result->descriptor;
                LOG_INFO("output desc: %d", slave_output_desc.handle);
            }
        }
        break;
    case GATT_EVENT_QUERY_COMPLETE:
        if (gatt_event_query_complete_parse(packet)->status != 0)
            break;

        if (slave_output_desc.handle != INVALID_HANDLE)
        {
            gatt_client_listen_for_characteristic_value_updates(&slave_output_notify, output_notification_handler,
                                                                slave_connect_handle, slave_output_char.value_handle);
                
            gatt_client_write_characteristic_descriptor_using_descriptor_handle(config_notify_callback, slave_connect_handle,
                slave_output_desc.handle, sizeof(char_config_notification),
                (uint8_t *)&char_config_notification);
        }
        break;
    }
}

void characteristic_discovery_callback(uint8_t packet_type, uint16_t _, const uint8_t *packet, uint16_t size)
{
    switch (packet[0])
    {
    case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
        {
            const gatt_event_characteristic_query_result_t *result =
                gatt_event_characteristic_query_result_parse(packet);
            if (get_sig_short_uuid(result->characteristic.uuid128) == TRANSMISSION_CHARACTER_INPUT_UUID_16)
            {
                slave_input_char = result->characteristic;
                LOG_INFO("input handle: %d", slave_input_char.value_handle);
            }
            else if (get_sig_short_uuid(result->characteristic.uuid128) == TRANSMISSION_CHARACTER_OUPUT_UUID_16)
            {
                slave_output_char = result->characteristic;
                LOG_INFO("output handle: %d", slave_output_char.value_handle);
            }
            else if (get_sig_short_uuid(result->characteristic.uuid128) == TRANSMISSION_CHARACTER_FLOW_CTRL_UUID_16)
            {
                slave_flow_ctrl_char = result->characteristic;
                LOG_INFO("output handle: %d", slave_flow_ctrl_char.value_handle);
            }
        }
        break;
    case GATT_EVENT_QUERY_COMPLETE:
        if (gatt_event_query_complete_parse(packet)->status != 0)
            break;

        if (INVALID_HANDLE == slave_input_char.value_handle ||
            INVALID_HANDLE == slave_output_char.value_handle ||
            INVALID_HANDLE == slave_flow_ctrl_char.value_handle)
        {
            LOG_ERROR("characteristic not found, disc");
            gap_disconnect(slave_connect_handle);
        }
        else
        {
            gatt_client_discover_characteristic_descriptors(descriptor_discovery_callback, slave_connect_handle, &slave_output_char);
        }
        break;
    }
}

void service_discovery_callback(uint8_t packet_type, uint16_t _, const uint8_t *packet, uint16_t size)
{
    printf("discovered service %d\r\n", packet[0]);
    switch (packet[0])
    {
    case GATT_EVENT_SERVICE_QUERY_RESULT:
        {
            const gatt_event_service_query_result_t *result =
                    gatt_event_service_query_result_parse(packet);
            
            printf("Service short UUID: %08X", get_sig_short_uuid(result->service.uuid128));
            
            if (get_sig_short_uuid(result->service.uuid128) == TRANSMISSION_SERVICE_UUID_16)
            {
                slave_service = result->service;
                printf("service handle: %d %d", slave_service.start_group_handle, slave_service.end_group_handle);
            }
        }
        break;
    case GATT_EVENT_QUERY_COMPLETE:
        printf("query complete\r\n");
    
        if (gatt_event_query_complete_parse(packet)->status != 0)
            break;
        if (slave_service.start_group_handle != INVALID_HANDLE)
        {
            gatt_client_discover_characteristics_for_service(characteristic_discovery_callback, slave_connect_handle,
                                                           slave_service.start_group_handle,
                                                           slave_service.end_group_handle);
        }
        else
        {
            printf("service not found, disc");
            gap_disconnect(slave_connect_handle);
        }
        break;
    }
}



void master_characteristic_discovery_callback(uint8_t packet_type, uint16_t _, const uint8_t *packet, uint16_t size)
{
    switch (packet[0])
    {
    case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
        {
            const gatt_event_characteristic_query_result_t *result =
                gatt_event_characteristic_query_result_parse(packet);
            if (get_sig_short_uuid(result->characteristic.uuid128) == TRANSMISSION_CHARACTER_FLOW_CTRL_UUID_16)
            {
                master_flow_ctrl_char = result->characteristic;
                LOG_INFO("flow_ctrl handle: %d", master_flow_ctrl_char.value_handle);
            }
        }
        break;
    case GATT_EVENT_QUERY_COMPLETE:
        if (gatt_event_query_complete_parse(packet)->status != 0)
            break;

        if (INVALID_HANDLE == master_flow_ctrl_char.value_handle)
        {
            LOG_ERROR("characteristic not found, disc");
            gap_disconnect(master_connect_handle);
        }
        break;
    }
}

void master_service_discovery_callback(uint8_t packet_type, uint16_t _, const uint8_t *packet, uint16_t size)
{
    switch (packet[0])
    {
    case GATT_EVENT_SERVICE_QUERY_RESULT:
        {
            const gatt_event_service_query_result_t *result =
                    gatt_event_service_query_result_parse(packet);
            
            if (get_sig_short_uuid(result->service.uuid128) == TRANSMISSION_SERVICE_UUID_16)
            {
                master_service = result->service;
                printf("service handle: %d %d\r\n", master_service.start_group_handle, master_service.end_group_handle);
            }
        }
        break;
    case GATT_EVENT_QUERY_COMPLETE:
        printf("query complete\r\n");
    
        if (gatt_event_query_complete_parse(packet)->status != 0)
            break;
        if (master_service.start_group_handle != INVALID_HANDLE)
        {
            gatt_client_discover_characteristics_for_service(master_characteristic_discovery_callback, master_connect_handle,
                                                           master_service.start_group_handle,
                                                           master_service.end_group_handle);
        }
        else
        {
            printf("service not found\r\n");
            gap_disconnect(master_connect_handle);
        }
        break;
    }
}





static void discovery_service()
{
    
    send_to_master_flow_control_enable = BT_PRIVT_DISABLE;
    send_to_slave_flow_control_enable = BT_PRIVT_DISABLE;
    recv_from_master_flow_control_enable = BT_PRIVT_ENABLE;
    recv_from_slave_flow_control_enable = BT_PRIVT_ENABLE;
    
    if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER) {
        gatt_client_discover_primary_services(service_discovery_callback, slave_connect_handle);
    } else if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_SLAVE) {
        gatt_client_discover_primary_services(master_service_discovery_callback, master_connect_handle);
    }
}

//==============================================================================================================



//==============================================================================================================
//* GATT flow control
//==============================================================================================================

void send_slave_flow_control()
{
    uint8_t enable = recv_from_slave_flow_control_enable == BT_PRIVT_ENABLE ? BT_PRIVT_DISABLE : BT_PRIVT_ENABLE;
    
    
    uint8_t buffer[2] = { FLOW_CONTROL_SLAVE_SEND_DATA_TO_MASTER, enable};
    uint8_t r = gatt_client_write_value_of_characteristic_without_response(slave_connect_handle, 
                                                                           slave_flow_ctrl_char.value_handle, 
                                                                           sizeof(buffer), 
                                                                           buffer);
    if (r == 0) {
        recv_from_slave_flow_control_enable = enable;
        send_slave_flow_control_over();
    } else {
        att_dispatch_client_request_can_send_now_event(slave_connect_handle); // Request can send now
    }
}
void send_slave_flow_control_start()
{
    if (send_slave_flow_control_to_be_continued == 1)
        return;
    printf("send slave flow control start\r\n");
    send_slave_flow_control_to_be_continued = 1;
    send_slave_flow_control();
}
void send_slave_flow_control_over()
{
    printf("send slave flow control over \r\n");
    send_slave_flow_control_to_be_continued = 0;
}

void send_master_flow_control()
{
    uint8_t enable = recv_from_master_flow_control_enable == BT_PRIVT_ENABLE ? BT_PRIVT_DISABLE : BT_PRIVT_ENABLE;
    
    uint8_t buffer[2] = { FLOW_CONTROL_MASTER_SEND_DATA_TO_SLAVE, enable};
    uint8_t r = gatt_client_write_value_of_characteristic_without_response(master_connect_handle, 
                                                               master_flow_ctrl_char.value_handle, 
                                                               sizeof(buffer), 
                                                               buffer);
    if (r == 0) {
        recv_from_master_flow_control_enable = enable;
        send_master_flow_control_over();
    } else {
        att_dispatch_client_request_can_send_now_event(master_connect_handle); // Request can send now
    }
}
void send_master_flow_control_start()
{
    if (send_master_flow_control_to_be_continued == 1)
        return;
    printf("send master flow control start\r\n");
    send_master_flow_control_to_be_continued = 1;
    send_master_flow_control();
}
void send_master_flow_control_over()
{
    printf("send master flow control over\r\n");
    send_master_flow_control_to_be_continued = 0;
}

//==============================================================================================================










//==============================================================================================================
// * UART IO Receive
//==============================================================================================================

extern buffer_config_table_t buffer_table;




/**
 * UART io port data processing callback
 * 
 * 
 */
void process_uart_io_port_data()
{
}
//==============================================================================================================


















//==============================================================================================================
//* Adv
//==============================================================================================================
const static uint8_t scan_data[] = { 0 };

const static ext_adv_set_en_t adv_sets_en[] = { {.handle = 0, .duration = 0, .max_events = 0} };

static void config_adv_and_set_interval(uint32_t intervel)
{
    gap_set_adv_set_random_addr(0, g_power_off_save_data_in_ram. module_mac_address);
    gap_set_ext_adv_para(0,
                            CONNECTABLE_ADV_BIT | SCANNABLE_ADV_BIT | LEGACY_PDU_BIT,
                            intervel, intervel,        // Primary_Advertising_Interval_Min, Primary_Advertising_Interval_Max
                            PRIMARY_ADV_ALL_CHANNELS,  // Primary_Advertising_Channel_Map
                            BD_ADDR_TYPE_LE_RANDOM,    // Own_Address_Type
                            BD_ADDR_TYPE_LE_PUBLIC,    // Peer_Address_Type (ignore)
                            NULL,                      // Peer_Address      (ignore)
                            ADV_FILTER_ALLOW_ALL,      // Advertising_Filter_Policy
                            0x00,                      // Advertising_Tx_Power
                            PHY_1M,                    // Primary_Advertising_PHY
                            0,                         // Secondary_Advertising_Max_Skip
                            PHY_1M,                    // Secondary_Advertising_PHY
                            0x00,                      // Advertising_SID
                            0x00);                     // Scan_Request_Notification_Enable
    gap_set_ext_adv_data(0, sizeof(g_power_off_save_data_in_ram.module_adv_data), (uint8_t*)&g_power_off_save_data_in_ram.module_adv_data);
    gap_set_ext_scan_response_data(0, sizeof(scan_data), (uint8_t*)scan_data);
}
static void start_adv(void)
{
    gap_set_ext_adv_enable(1, sizeof(adv_sets_en) / sizeof(adv_sets_en[0]), adv_sets_en);
    
    LOG_INFO("Start Adv\r\n");
}
static void stop_adv(void)
{
    gap_set_ext_adv_enable(0, sizeof(adv_sets_en) / sizeof(adv_sets_en[0]), adv_sets_en);
    
    LOG_INFO("Stop Adv\r\n");
}
//==============================================================================================================
















//==============================================================================================================
//* Scan
//==============================================================================================================
static const scan_phy_config_t configs[1] =
{
    {
        .phy = PHY_1M,
        .type = SCAN_PASSIVE,
        .interval = 200,
        .window = 50
    }
};

static void config_scan(void)
{
    gap_set_ext_scan_para(BD_ADDR_TYPE_LE_RANDOM, SCAN_ACCEPT_ALL_EXCEPT_NOT_DIRECTED,
                          sizeof(configs) / sizeof(configs[0]),
                          configs);
}

static void start_scan(void)
{
    gap_set_ext_scan_enable(1, 0, 0, 0);   // start continuous scanning
                          
    LOG_INFO("Start Scan\r\n");
}

static void stop_scan(void)
{
    gap_set_ext_scan_enable(1, 0, 0, 0);   // stop scanning
                          
    LOG_INFO("Start Scan\r\n");
}

//==============================================================================================================











//==============================================================================================================
//* Unknown
//==============================================================================================================


static initiating_phy_config_t phy_configs[] =
{
    {
        .phy = PHY_1M,
        .conn_param =
        {
            .scan_int = 200,
            .scan_win = 180,
            .interval_min = 50,
            .interval_max = 50,
            .latency = 0,
            .supervision_timeout = 600,
            .min_ce_len = 90,
            .max_ce_len = 90
        }
    }
};
//==============================================================================================================



uint16_t private_att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset,
                                  uint8_t * buffer, uint16_t buffer_size)
{
    return module_handle_att_read_callback(connection_handle, att_handle, offset, buffer, buffer_size);
}

int private_att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode,
                              uint16_t offset, const uint8_t *buffer, uint16_t buffer_size)
{
    return module_handle_att_write_callback(connection_handle, att_handle, transaction_mode, offset, buffer, buffer_size);
}


void private_user_msg_handler(uint32_t msg_id, void *data, uint16_t size)
{
    switch (msg_id)
    {
    case USER_MSG_SEND_DATA_TO_BLE_SLAVE:
        send_data_to_ble_slave();
        break;
    case USER_MSG_SEND_DATA_TO_BLE_MASTER:
        send_data_to_ble_master();
        break;
    default:
        break;
    }
}

/**
 * Set the device address and turn on broadcasting and scanning
 */
void private_on_btstack_event_state(void)
{
    gap_set_random_device_address(g_power_off_save_data_in_ram.module_mac_address);
    
    config_adv_and_set_interval(50);
    start_adv();
    
    config_scan();
    start_scan();
}

/**
 * Receive broadcast event callbacks.
 * If the device meets the requirements, send a BLE connection request
 */
void private_on_hci_subevent_le_extended_advertising_report(const le_meta_event_ext_adv_report_t* report_complete)
{
    const le_ext_adv_report_t* report = report_complete->reports;
    
    if (ad_data_contains_uuid16(report->data_len, report->data, TRANSMISSION_SERVICE_UUID_16) == 0) {
        return;
    }
    
    bd_addr_t peer_dev_addr;
    reverse_bd_addr(report->address, peer_dev_addr);//
    
    platform_printf("dev addr: %02X%02X%02X%02X%02X%02X", peer_dev_addr[0], 
                                                          peer_dev_addr[1], 
                                                          peer_dev_addr[2], 
                                                          peer_dev_addr[3], 
                                                          peer_dev_addr[4], 
                                                          peer_dev_addr[5]);
    
    // If the device is not in the device record table, recorded in the table
    if (!at_contains_device(&at_processor, peer_dev_addr)) {
        at_processor_add_scan_device(&at_processor, peer_dev_addr);
    }
    
    // Connect only the device set by user
    if (memcmp(g_power_off_save_data_in_ram.peer_mac_address, peer_dev_addr, BT_AT_CMD_TTM_MAC_ADDRESS_LEN) != 0) {
        return;
    }
    
    bd_addr_t peer_addr;
    gap_set_ext_scan_enable(0, 0, 0, 0);
    reverse_bd_addr(report->address, peer_addr);
    LOG_MSG("connecting ...");  
    
    if (report->evt_type & HCI_EXT_ADV_PROP_USE_LEGACY)
        phy_configs[0].phy = PHY_1M;
    else
        phy_configs[0].phy = (phy_type_t)(report->s_phy != 0 ? report->s_phy : report->p_phy);
    gap_ext_create_connection(        INITIATING_ADVERTISER_FROM_PARAM, // Initiator_Filter_Policy,
                                      BD_ADDR_TYPE_LE_RANDOM,           // Own_Address_Type,
                                      report->addr_type,                // Peer_Address_Type,
                                      peer_addr,                        // Peer_Address,
                                      sizeof(phy_configs) / sizeof(phy_configs[0]),
                                      phy_configs);
}

void update_device_type()
{
    if (slave_connect_handle != INVALID_HANDLE && master_connect_handle != INVALID_HANDLE) {
        g_power_off_save_data_in_ram.dev_type = BLE_DEV_TYPE_MASTER_AND_SLAVE;
        printf("Master And Slave.\r\n");
    } else if (slave_connect_handle != INVALID_HANDLE) {
        g_power_off_save_data_in_ram.dev_type = BLE_DEV_TYPE_MASTER;
        printf("Master.\r\n");
    } else if (master_connect_handle != INVALID_HANDLE) {
        g_power_off_save_data_in_ram.dev_type = BLE_DEV_TYPE_SLAVE;
        printf("Slave.\r\n");
    } else {
        g_power_off_save_data_in_ram.dev_type = BLE_DEV_TYPE_NO_CONNECTION;
        printf("No Connection.\r\n");
    }
}

/**
 * Connection event callbacks
 * Update the passthrough device type
 * Discover the service
 */
void private_on_hci_subevent_le_enhanced_connection_complete(const le_meta_event_enh_create_conn_complete_t* conn_complete)
{
    LOG_MSG("Connected!\r\n");
    
    hint_ce_len(conn_complete->interval);
    
    att_set_db(conn_complete->handle, att_db_util_get_address());
    
    if (conn_complete->role == HCI_ROLE_SLAVE) {
        master_connect_handle = conn_complete->handle;
    } else {
        slave_connect_handle = conn_complete->handle;
    }
    update_device_type();
    
    gap_read_remote_used_features(conn_complete->handle);
    
    discovery_service();
}

void private_on_hci_subevent_le_phy_update_complete(const le_meta_phy_update_complete_t *hci_le_cmpl)
{
    LOG_MSG("PHY updated: Rx %d, Tx %d\r\n", hci_le_cmpl->rx_phy, hci_le_cmpl->tx_phy);
    
    if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER) {
        stop_adv();
        config_adv_and_set_interval(100);
        start_adv();
        LOG_INFO("master change adv param\r\n");
    }
}

/**
 * Disconnection event callbacks
 * Update the passthrough device type
 * 
 */
void private_on_hci_event_disconnection_complete(const event_disconn_complete_t * disconn_event)
{
    platform_printf("Disconnected!!\r\n");
    // TODO send disconnected info to AT console
    
    if (disconn_event->conn_handle == slave_connect_handle) {
        slave_connect_handle = INVALID_HANDLE;
    } else if (disconn_event->conn_handle == master_connect_handle) {
        master_connect_handle = INVALID_HANDLE;
    }
    update_device_type();
    
    if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER_AND_SLAVE) {
    } else if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_SLAVE) {
    } else if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER) {
        start_adv();
    } else {
        stop_adv();
        config_adv_and_set_interval(50);
        start_adv();
        start_scan();
    }
}

void private_on_att_event_can_send_now(void)
{
    printf("Into Scan send now\r\n");
    if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_SLAVE || 
        g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER_AND_SLAVE) {
        if (send_master_flow_control_to_be_continued == 1) {
            send_master_flow_control();
        }
    }
    if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER || 
        g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER_AND_SLAVE) {
        if (send_slave_flow_control_to_be_continued == 1) {
            send_slave_flow_control();
        }
    }
    if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER) {
        if (send_to_slave_data_to_be_continued == 1) {
            send_data_to_ble_slave();
        }
    } else if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_SLAVE) {
        if (send_to_master_data_to_be_continued == 1) {
            send_data_to_ble_master();
        }
    } else if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER_AND_SLAVE){
        if (send_to_master_data_to_be_continued == 1) {
            send_data_to_ble_master();
        }
        if (send_to_slave_data_to_be_continued == 1) {
            send_data_to_ble_slave();
        }
    } else {
        LOG_ERROR("Dev type Error!\r\n");
    }
}


extern buffer_config_table_t buffer_table;

uint8_t temp_buffer[300] = { 0 };

void process_rx_port_data()
{
    if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_NO_CONNECTION) {
        uint32_t len = circular_queue_get_elem_num(buffer_table.rx_in_cmd_out);
        circular_queue_dequeue_batch(buffer_table.rx_in_cmd_out, temp_buffer, len);
        
        at_processor_start(&at_processor, temp_buffer, len);
        
    } else if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER) {
        send_data_to_ble_slave_start();
    } else if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_SLAVE) {
        send_data_to_ble_master_start();
    } else if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER_AND_SLAVE) {
        LOG_ERROR("Dev type Error!\r\n");
    } else {
    }
}

void process_slave_port_data()
{
    if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER) {
    } else if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER_AND_SLAVE) {
        send_data_to_ble_master_start();
    } else {
        LOG_ERROR("Dev type Error!\r\n");
    }
}

void process_master_port_data()
{
    if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_SLAVE) {
    } else if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER_AND_SLAVE) {
        send_data_to_ble_slave_start();
    } else {
        LOG_ERROR("Dev type Error!\r\n");
    }
}


SemaphoreHandle_t rx_port_has_data;
static void rx_port_task(void *pdata)
{
    for (;;) {
        BaseType_t r = xSemaphoreTake(rx_port_has_data,  portMAX_DELAY);
        if (r != pdTRUE) continue;
        process_rx_port_data();
    }
}
void notify_rx_port_has_data()
{
    BaseType_t xHigherPriorityTaskWoke = pdFALSE;
    xSemaphoreGiveFromISR(rx_port_has_data, &xHigherPriorityTaskWoke);
}


SemaphoreHandle_t slave_port_has_data;
static void slave_port_task(void *pdata)
{
    for (;;) {
        BaseType_t r = xSemaphoreTake(slave_port_has_data,  portMAX_DELAY);
        if (r != pdTRUE) continue;
        process_slave_port_data();
    }
}
void notify_slave_port_has_data()
{
    BaseType_t xHigherPriorityTaskWoke = pdFALSE;
    xSemaphoreGiveFromISR(slave_port_has_data, &xHigherPriorityTaskWoke);
}

SemaphoreHandle_t master_port_has_data;
static void master_port_task(void *pdata)
{
    for (;;) {
        BaseType_t r = xSemaphoreTake(master_port_has_data,  portMAX_DELAY);
        if (r != pdTRUE) continue;
        process_master_port_data();
    }
}
void notify_master_port_has_data()
{
    BaseType_t xHigherPriorityTaskWoke = pdFALSE;
    xSemaphoreGiveFromISR(master_port_has_data, &xHigherPriorityTaskWoke);
}


void show_heap(void)
{
    static char buffer[200];
    platform_heap_status_t status;
    platform_get_heap_status(&status);
    sprintf(buffer, "heap status:\n"
                    "    free: %d B\n"
                    "min free: %d B\n", status.bytes_free, status.bytes_minimum_ever_free);
    platform_printf(buffer, strlen(buffer) + 1);
}

static void timer_task(TimerHandle_t xTimer)
{
    //show_heap();

    if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_NO_CONNECTION) {
        printf("cmd processor\r\n");
    } else if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER) {
        printf("rx_sum:%d tx_sum:%d receive_slave_sum:%d send_to_slave_sum:%d buffer_num:%d flow_ctrl:%d\r\n", rx_sum, tx_sum, receive_slave_sum, send_to_slave_sum, circular_queue_get_elem_num(buffer_table.ble_slave_in_tx_out), send_to_slave_flow_control_enable);
    } else if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_SLAVE) {
        printf("rx_sum:%d tx_sum:%d receive_master_sum:%d send_to_master_sum:%d buffer_num:%d flow_ctrl:%d\r\n", rx_sum, tx_sum, receive_master_sum, send_to_master_sum, circular_queue_get_elem_num(buffer_table.ble_master_in_tx_out), send_to_master_flow_control_enable);
    } else if (g_power_off_save_data_in_ram.dev_type == BLE_DEV_TYPE_MASTER_AND_SLAVE) {
        // TODO
    }
}

TimerHandle_t probe_timer;

void start_probe()
{
    xTimerStart(probe_timer, portMAX_DELAY);
}

void stop_probe()
{
    xTimerStop(probe_timer, portMAX_DELAY);
}


void flow_control_read_callback(uint8_t packet_type, uint16_t channel, const uint8_t *packet, uint16_t size)
{
    switch (packet[0])
    {
    case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT:
        {
            uint16_t value_size;
            const gatt_event_value_packet_t *value =
                gatt_event_characteristic_value_query_result_parse(packet, size, &value_size);
            
            uint8_t* buffer = (uint8_t*)value->value;
            if (buffer[0] == BT_PRIVT_DISABLE) {
                printf("peer recv from master flow control disable");
                send_to_slave_flow_control_enable = BT_PRIVT_DISABLE;
                stop_probe();
            }
        }
        break;
    }
}

static void request_send_flow_control_enable(TimerHandle_t xTimer)
{
    gatt_client_read_value_of_characteristic_using_value_handle(flow_control_read_callback, 
                                                                slave_connect_handle, 
                                                                slave_flow_ctrl_char.value_handle);
}

void init_slave_and_master_port_task()
{
    rx_port_has_data = xSemaphoreCreateBinary();
    xTaskCreate(rx_port_task, "rx_port_task", 256, NULL, (configMAX_PRIORITIES - 1), NULL);
    
    slave_port_has_data = xSemaphoreCreateBinary();
    xTaskCreate(slave_port_task, "slave_port_task", 256, NULL, (configMAX_PRIORITIES - 2), NULL);
    
    master_port_has_data = xSemaphoreCreateBinary();
    xTaskCreate(master_port_task, "master_port_task", 256, NULL, (configMAX_PRIORITIES - 3), NULL);
    
    
    
    TimerHandle_t timer = xTimerCreate("led2", pdMS_TO_TICKS(2000), pdTRUE, NULL, timer_task); //1s
    xTimerStart(timer, portMAX_DELAY);
    
    probe_timer = xTimerCreate("probe_timer", pdMS_TO_TICKS(2000), pdTRUE, NULL, request_send_flow_control_enable); //1s
}

void init_private_user_packet_handler()
{
    add_on_btstack_event_state_listener(private_on_btstack_event_state);
    add_on_hci_subevent_le_extended_advertising_report_listener(private_on_hci_subevent_le_extended_advertising_report);
    add_on_hci_subevent_le_enhanced_connection_complete_listener(private_on_hci_subevent_le_enhanced_connection_complete);
    add_on_hci_subevent_le_phy_update_complete_listener(private_on_hci_subevent_le_phy_update_complete);
    add_on_hci_event_disconnection_complete_listener(private_on_hci_event_disconnection_complete);
    add_on_att_event_can_send_now_listener(private_on_att_event_can_send_now);
    add_on_att_read_listener(private_att_read_callback);
    add_on_att_write_listener(private_att_write_callback);
    add_on_user_msg_listener(private_user_msg_handler);
    
    init_rtos_util();
    
    init_router();
    
    init_service();
    
    init_at_processor(&at_processor);
    
    init_slave_and_master_port_task();
    
    init_uart_io();
}