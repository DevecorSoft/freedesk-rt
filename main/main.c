#include <stdio.h>
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "de_connect.h"

#define DE_FREEDESK_TOPIC CONFIG_DE_FREEDESK_TOPIC
#define DE_BROKER_URL CONFIG_DE_BROKER_URL
#define DE_BROKER_USER_NAME CONFIG_BROKER_USER_NAME
#define DE_BROKER_USER_PASS CONFIG_BROKER_USER_PASS

#define GPIO_OUTPUT_IO_0 12 // lower
#define GPIO_OUTPUT_IO_1 14 // raise
#define GPIO_OUTPUT_PIN_SEL ((1ULL << GPIO_OUTPUT_IO_0) | (1ULL << GPIO_OUTPUT_IO_1))

#define GPIO_INPUT_IO_2 4 // no raising
#define GPIO_INPUT_IO_3 5 // no lowering
#define GPIO_INPUT_PIN_SEL ((1ULL << GPIO_INPUT_IO_2) | (1ULL << GPIO_INPUT_IO_3))

static char s_freedesk_topic[32] = DE_FREEDESK_TOPIC;
static xQueueHandle gpio_evt_queue = NULL;
static const char *TAG = "DE_MQTT";

static void fd_gpio_input_event_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    ESP_LOGI(TAG, "GPIO input event  %d\n", gpio_num);
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_task_example(void *arg)
{
    uint32_t io_num;

    for (;;) {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            ESP_LOGI(TAG, "GPIO[%d] intr, val: %d\n", io_num, gpio_get_level(io_num));
        }
    }
}

void print_chip_info()
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is ESP8266 chip with %d CPU cores, WiFi, ",
           chip_info.cores);

    printf("silicon revision %d, ", chip_info.revision);

    printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
}

void initiate_gpio()
{
    gpio_config_t io_conf;
    // disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    // set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    // bit mask of the pins that you want to set,e.g.GPIO15/16
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    // disable pull-down mode
    io_conf.pull_down_en = 0;
    // disable pull-up mode
    io_conf.pull_up_en = 0;
    // configure GPIO with the given settings
    gpio_config(&io_conf);
}

void initiate_gpio_input()
{
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);
    gpio_install_isr_service(0);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_IO_2, fd_gpio_input_event_handler, (void *) GPIO_INPUT_IO_2);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_IO_3, fd_gpio_input_event_handler, (void *) GPIO_INPUT_IO_3);
}

void desk_control(bool lower, bool raise)
{
    gpio_set_level(GPIO_OUTPUT_IO_0, lower);
    gpio_set_level(GPIO_OUTPUT_IO_1, raise);

    ESP_LOGI(TAG, "gpio 0 level: %d", gpio_get_level(GPIO_OUTPUT_IO_0));
    ESP_LOGI(TAG, "gpio 1 level: %d", gpio_get_level(GPIO_OUTPUT_IO_1));
}

void desk_lower()
{
    desk_control(true, false);
}

void desk_raise()
{
    desk_control(false, true);
}

void desk_lock()
{
    desk_control(false, false);
}

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

        msg_id = esp_mqtt_client_subscribe(client, s_freedesk_topic, 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        if (strncmp(event->data, "raise", event->data_len) == 0)
        {
            ESP_LOGI(TAG, "---up");
            desk_raise();
        }
        else if (strncmp(event->data, "lower", event->data_len) == 0)
        {
            ESP_LOGI(TAG, "---lower");
            desk_lower();
        }
        else if (strncmp(event->data, "lock", event->data_len) == 0)
        {
            ESP_LOGI(TAG, "---lock");
            desk_lock();
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_DE_BROKER_URL,
        .username = CONFIG_DE_BROKER_USER_NAME,
        .password = CONFIG_DE_BROKER_USER_PASS};

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

void app_main(void)
{
    print_chip_info();

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());

    initiate_gpio();
    initiate_gpio_input();

    ESP_ERROR_CHECK(gpio_set_level(GPIO_OUTPUT_IO_0, 0));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_OUTPUT_IO_1, 0));

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(connect());

    mqtt_app_start();
}
