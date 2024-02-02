#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "sys/param.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"
#include "esp_https_server.h"
#include "esp_tls.h"
#include "sdkconfig.h"
#include "driver/gpio.h"


#define MOTOR_PIN 5

#define TRIGGER_PIN GPIO_NUM_2
#define ECHO_PIN GPIO_NUM_4

#define PULSE_DURATION_US 10
#define TIMEOUT_US 5000  // Ajusta este valor según sea necesario
#define PRINT_PERIOD_MS 1000



static const char *TAG = "main";

float nivel = 0.0f;
float temperatura = 5.0f;
bool onoff = false;
bool motor_state = false;

/* Función para medir el nivel */
void vTask1(void* pvParameters) {
    uint32_t distance;

    TickType_t xLastWakeTime;
    const TickType_t xFrequency = pdMS_TO_TICKS(PRINT_PERIOD_MS);

    xLastWakeTime = xTaskGetTickCount();

    while (1) {
        // Generar pulso en el pin de trigger
        gpio_set_level(TRIGGER_PIN, 0);
        ets_delay_us(PULSE_DURATION_US);
        gpio_set_level(TRIGGER_PIN, 1);
        ets_delay_us(PULSE_DURATION_US);
        gpio_set_level(TRIGGER_PIN, 0);

        // Esperar el pulso de eco
        uint64_t start = esp_timer_get_time();
        while (gpio_get_level(ECHO_PIN) == 0 && (esp_timer_get_time() - start) < TIMEOUT_US) {}
        
        start = esp_timer_get_time();
        while (gpio_get_level(ECHO_PIN) == 1 && (esp_timer_get_time() - start) < TIMEOUT_US) {}

        // Calcular la distancia en centímetros
        nivel = (esp_timer_get_time() - start) / 58;
        printf("Distancia: %f cm\n", nivel);

        // Esperar hasta el próximo período de impresión
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

/* Función para medir la temperatura */
void medir_temperatura() {
    // Esta función debería medir la temperatura y actualizar la variable global 'temperatura'
}

/* Función para activar/desactivar el motor */
void toggle_motor() {
    motor_state = !motor_state;
    gpio_set_level(MOTOR_PIN, motor_state);
    printf("Motor %s\n", motor_state ? "encendido" : "apagado");
}

/* An HTTP GET handler */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    extern unsigned char view_start[] asm("_binary_view_html_start");
    extern unsigned char view_end[] asm("_binary_view_html_end");
    size_t view_len = view_end - view_start;
    char viewHtml[view_len];
    memcpy(viewHtml, view_start, view_len);
    ESP_LOGI(TAG, "URI: %s", req->uri);

    char viewHtmlUpdated[view_len + 50]; // Espacio adicional para los valores de temperatura y nivel
    int formattedStrResult = snprintf(viewHtmlUpdated, sizeof(viewHtmlUpdated), viewHtml, nivel, temperatura);

    httpd_resp_set_type(req, "text/html");

    if (formattedStrResult > 0 && formattedStrResult < sizeof(viewHtmlUpdated))
    {
        httpd_resp_send(req, viewHtmlUpdated, strlen(viewHtmlUpdated));
    }
    else
    {
        ESP_LOGE(TAG, "Error updating variables");
        httpd_resp_send(req, viewHtml, view_len);
    }

    char buf[10];
    size_t buf_len = sizeof(buf);
    // Verificar si la solicitud es para encender o apagar el motor
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
        char param[10];
        if (httpd_query_key_value(buf, "motor", param, sizeof(param)) == ESP_OK) {
            if (strcmp(param, "on") == 0) {
                toggle_motor();
            } else if (strcmp(param, "off") == 0) {
                toggle_motor();
            }
        }
    }

    return ESP_OK;
}


static const httpd_uri_t root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server");

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.transport_mode = HTTPD_SSL_TRANSPORT_INSECURE;
    esp_err_t ret = httpd_ssl_start(&server, &conf);
    if (ESP_OK != ret)
    {
        ESP_LOGI(TAG, "Error starting server!");
        return NULL;
    }

    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    httpd_register_uri_handler(server, &root);
    return server;
}

static void stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_ssl_stop(server);
}

static void disconnect_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server)
    {
        stop_webserver(*server);
        *server = NULL;
    }
}

static void connect_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server == NULL)
    {
        *server = start_webserver();
    }
}

esp_err_t init_sensor();
void vTask1(void* pvParameters);

void app_main(void)
{   
    static httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
    ESP_ERROR_CHECK(example_connect());

    // Configurar el pin del motor
    gpio_reset_pin(MOTOR_PIN);
    gpio_set_direction(MOTOR_PIN, GPIO_MODE_OUTPUT);

    //Configuracion nivel
    init_sensor();

    xTaskCreatePinnedToCore(vTask1, "vTask1", 1024 * 2, NULL, 1, NULL, 1);
    // Iniciar el servidor web
    server = start_webserver();

}

esp_err_t init_sensor() {
    gpio_reset_pin(TRIGGER_PIN);
    gpio_set_direction(TRIGGER_PIN, GPIO_MODE_OUTPUT);

    gpio_reset_pin(ECHO_PIN);
    gpio_set_direction(ECHO_PIN, GPIO_MODE_INPUT);

    return ESP_OK;
}