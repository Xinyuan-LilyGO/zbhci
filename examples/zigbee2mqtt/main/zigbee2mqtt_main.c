/* Hello World Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "app_mqtt.h"
#include "zbhci.h"
#include "app_db.h"
#include "device.h"
#include "app_config.h"

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1


#ifndef ARDUINO
void main_loop();
#endif

static const char *TAG = "wifi station";

static int s_retry_num = 0;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

QueueHandle_t msg_queue;

static void event_handler(void            *arg,
                          esp_event_base_t event_base,
                          int32_t          event_id,
                          void            *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config;
    memcpy(wifi_config.sta.ssid, WIFI_SSID, strlen(WIFI_SSID));
    memcpy(wifi_config.sta.password, WIFI_PASS, strlen(WIFI_PASS));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT)
     * or connection failed for the maximum number of re-tries (WIFI_FAIL_BIT).
     * The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we
     * can test which event actually happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", WIFI_SSID, WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", WIFI_SSID, WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}


void app_nvs_init(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}


#ifdef ARDUINO
void setup()
#else
void app_main(void)
#endif
{

    app_nvs_init();
    wifi_init_sta();
    mqtt_app_start();
    app_db_init();

    msg_queue = xQueueCreate(10, sizeof(ts_HciMsg));

    zbhci_Init(msg_queue);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    zbhci_NetworkStateReq();

#ifndef ARDUINO
    main_loop();
#endif
}


#ifdef ARDUINO
void loop()
#else
void main_loop(void)
#endif
{
    ts_HciMsg sHciMsg;
    device_node_t *device = NULL;

    while (1)
    {
        bzero(&sHciMsg, sizeof(sHciMsg));
        if (xQueueReceive(msg_queue, &sHciMsg, portMAX_DELAY))
        {
            switch (sHciMsg.u16MsgType)
            {
                case ZBHCI_CMD_BDB_COMMISSION_FORMATION_RSP:
                {
                    zbhci_MgmtPermitJoinReq(0xFFFC, 0xFF, 1);
                }
                break;

                case ZBHCI_CMD_NETWORK_STATE_RSP:
                    if (sHciMsg.uPayload.sNetworkStateRspPayloasd.u16NwkAddr != 0x0000)
                    {
                        zbhci_BdbChannelSet(25);
                        vTaskDelay(100 / portTICK_PERIOD_MS);
                        zbhci_BdbCommissionFormation();
                    }
                    else
                    {
                        brige_node.nwk_addr  = sHciMsg.uPayload.sNetworkStateRspPayloasd.u16NwkAddr;
                        brige_node.mac_addr  = sHciMsg.uPayload.sNetworkStateRspPayloasd.u64IeeeAddr;
                        brige_node.pan_id    = sHciMsg.uPayload.sNetworkStateRspPayloasd.u16PanId;
                        brige_node.ex_pan_id = sHciMsg.uPayload.sNetworkStateRspPayloasd.u64extPanId;
                        brige_node.channel   = sHciMsg.uPayload.sNetworkStateRspPayloasd.u8Channel;
                        app_db_recover();
                        zbhci_MgmtPermitJoinReq(0xFFFC, 0xFF, 1);
                    }
                break;

                case ZBHCI_CMD_NODES_DEV_ANNCE_IND:
                {
                    device = get_empty_device();
                    if (!device) continue;
                    device->u16NwkAddr  = sHciMsg.uPayload.sNodesDevAnnceRspPayload.u16NwkAddr;
                    device->u64IeeeAddr = sHciMsg.uPayload.sNodesDevAnnceRspPayload.u64IEEEAddr;
                    device->u8Type      = sHciMsg.uPayload.sNodesDevAnnceRspPayload.u8Capability;
                }
                break;

                case ZBHCI_CMD_LEAVE_INDICATION:
                {
                    device = find_device_by_ieeeaddr(sHciMsg.uPayload.sLeaveIndicationPayload.u64MacAddr);
                    if (!sHciMsg.uPayload.sLeaveIndicationPayload.u8Rejoin)
                    {
                        if (!strncmp((const char *)device->au8ModelId,
                                        "lumi.sensor_motion.aq2",
                                        strlen("lumi.sensor_motion.aq2")))
                        {
                            rtcgq11lm_delete(device->u64IeeeAddr);
                        }
                        else if (!strncmp((const char *)device->au8ModelId,
                                            "lumi.weather",
                                            strlen("lumi.weather")))
                        {
                            wsdcgq11lm_delete(device->u64IeeeAddr);
                        }
                        memset(device, 0, sizeof(device_node_t));
                        app_db_save();
                    }
                }
                break;

                case ZBHCI_CMD_ZCL_REPORT_MSG_RCV:
                {
                    switch (sHciMsg.uPayload.sZclReportMsgRcvPayload.u16ClusterId)
                    {
                        case 0x0000: /* Basic Cluster */
                            for (size_t i = 0; i < sHciMsg.uPayload.sZclReportMsgRcvPayload.u8AttrNum; i++)
                            {
                                if (sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].u16AttrID == 0x0005)
                                {
                                    // lumi.sensor_motion.aq2
                                    device = find_device_by_nwkaddr(sHciMsg.uPayload.sZclReportMsgRcvPayload.u16SrcAddr);
                                    if (!device) continue;
                                    memcpy(device->au8ModelId, \
                                           sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].uAttrData.au8AttrData, \
                                           sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].u16DataLen);
                                    if (!strncmp((const char *)sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].uAttrData.au8AttrData,
                                                 "lumi.sensor_motion.aq2",
                                                 strlen("lumi.sensor_motion.aq2")))
                                    {
                                        app_db_save();
                                        rtcgq11lm_add(device->u64IeeeAddr);
                                    }
                                    else if (!strncmp((const char *)sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].uAttrData.au8AttrData,
                                                      "lumi.weather",
                                                      strlen("lumi.weather")))
                                    {
                                        app_db_save();
                                        wsdcgq11lm_add(device->u64IeeeAddr);
                                    }
                                }
                            }
                        break;

                        case 0x0400: /* Illuminance Measurement Cluster */
                        {
                            device = find_device_by_nwkaddr(sHciMsg.uPayload.sZclReportMsgRcvPayload.u16SrcAddr);
                            if (!device) continue;
                            for (size_t i = 0; i < sHciMsg.uPayload.sZclReportMsgRcvPayload.u8AttrNum; i++)
                            {
                                if (sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].u16AttrID == 0x0000)
                                {
                                    device->device_data.rtcgq11lm.u16Illuminance = sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].uAttrData.u16AttrData;
                                    if (!strncmp((const char *)device->au8ModelId,
                                                "lumi.sensor_motion.aq2",
                                                strlen("lumi.sensor_motion.aq2")))
                                    {
                                        rtcgq11lm_report(device->u64IeeeAddr,
                                                         device->device_data.rtcgq11lm.u8Occupancy,
                                                         device->device_data.rtcgq11lm.u16Illuminance);
                                    }
                                }
                            }
                        }
                        break;

                        case 0x0402: /* Temperature Measurement Cluster */
                        {
                            device = find_device_by_nwkaddr(sHciMsg.uPayload.sZclReportMsgRcvPayload.u16SrcAddr);
                            if (!device) continue;
                            for (size_t i = 0; i < sHciMsg.uPayload.sZclReportMsgRcvPayload.u8AttrNum; i++)
                            {
                                if (sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].u16AttrID == 0x0000)
                                {
                                    device->device_data.wsdcgq11lm.i16Temperature = (int16_t)sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].uAttrData.u16AttrData;
                                    if (!strncmp((const char *)device->au8ModelId,
                                                "lumi.weather",
                                                strlen("lumi.weather")))
                                    {
                                            wsdcgq11lm_report(device->u64IeeeAddr,
                                                              device->device_data.wsdcgq11lm.i16Temperature,
                                                              device->device_data.wsdcgq11lm.i16Humidity,
                                                              device->device_data.wsdcgq11lm.i16Pressure);
                                    }
                                }
                            }
                        }
                        break;

                        case 0x0403:/* Pressure Measurement Cluster */
                        {
                            device = find_device_by_nwkaddr(sHciMsg.uPayload.sZclReportMsgRcvPayload.u16SrcAddr);
                            if (!device) continue;
                            for (size_t i = 0; i < sHciMsg.uPayload.sZclReportMsgRcvPayload.u8AttrNum; i++)
                            {
                                if (sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].u16AttrID == 0x0000)
                                {
                                    device->device_data.wsdcgq11lm.i16Pressure = (int16_t)sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].uAttrData.u16AttrData;
                                    if (!strncmp((const char *)device->au8ModelId,
                                                "lumi.weather",
                                                strlen("lumi.weather")))
                                    {
                                            wsdcgq11lm_report(device->u64IeeeAddr,
                                                              device->device_data.wsdcgq11lm.i16Temperature,
                                                              device->device_data.wsdcgq11lm.i16Humidity,
                                                              device->device_data.wsdcgq11lm.i16Pressure);
                                    }
                                }
                            }
                        }
                        break;

                        case 0x0405: /* Relative Humidity Measurement Cluster */
                        {
                            device = find_device_by_nwkaddr(sHciMsg.uPayload.sZclReportMsgRcvPayload.u16SrcAddr);
                            if (!device) continue;
                            for (size_t i = 0; i < sHciMsg.uPayload.sZclReportMsgRcvPayload.u8AttrNum; i++)
                            {
                                if (sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].u16AttrID == 0x0000)
                                {
                                    device->device_data.wsdcgq11lm.i16Humidity = (int16_t)sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].uAttrData.u16AttrData;
                                    if (!strncmp((const char *)device->au8ModelId,
                                                "lumi.weather",
                                                strlen("lumi.weather")))
                                    {
                                            wsdcgq11lm_report(device->u64IeeeAddr,
                                                              device->device_data.wsdcgq11lm.i16Temperature,
                                                              device->device_data.wsdcgq11lm.i16Humidity,
                                                              device->device_data.wsdcgq11lm.i16Pressure);
                                    }
                                }
                            }
                        }
                        break;

                        case 0x0406: /* Occupancy Sensing Cluster */
                        {
                            device = find_device_by_nwkaddr(sHciMsg.uPayload.sZclReportMsgRcvPayload.u16SrcAddr);
                            if (!device) continue;
                            for (size_t i = 0; i < sHciMsg.uPayload.sZclReportMsgRcvPayload.u8AttrNum; i++)
                            {
                                if (sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].u16AttrID == 0x0000)
                                {
                                    device->device_data.rtcgq11lm.u8Occupancy = sHciMsg.uPayload.sZclReportMsgRcvPayload.asAttrList[i].uAttrData.u8AttrData;
                                    if (!strncmp((const char *)device->au8ModelId,
                                                "lumi.sensor_motion.aq2",
                                                strlen("lumi.sensor_motion.aq2")))
                                    {
                                        rtcgq11lm_report(device->u64IeeeAddr,
                                                         device->device_data.rtcgq11lm.u8Occupancy,
                                                         device->device_data.rtcgq11lm.u16Illuminance);
                                    }
                                }
                            }
                        }
                        break;

                        default:
                        break;
                    }

                }
                break;

                default:
                break;
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}