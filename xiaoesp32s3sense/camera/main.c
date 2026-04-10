


#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

static const char *TAG = "XIAO_S3_PRO";

/* --- WIFI CONFIG --- */ 
#define WIFI_SSID "xxxxxxx"
#define WIFI_PASS "xxxxx"
#define MOUNT_POINT "/sdcard"  


/* --- Dashboard HTML (Updated for same-window navigation) --- */
const char* index_html = 
"<!DOCTYPE html><html><head><title>XIAO S3</title><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
"<style>body{background:#000;color:#fff;font-family:sans-serif;text-align:center;margin:0;padding:10px;}"
".view{width:100%;max-width:640px;margin:auto;border-radius:10px;background:#111;min-height:240px;border:1px solid #333;overflow:hidden;display:flex;align-items:center;justify-content:center;}"
"img{width:100%;display:none;}"
".btn{display:block;width:100%;max-width:640px;margin:10px auto;padding:15px;border-radius:10px;font-weight:bold;color:#fff;border:none;cursor:pointer;text-decoration:none;}"
".blue{background:#007aff;}.green{background:#34c759;}.red{background:#ff3b30;}"
"</style></head><body>"
"<h3>XIAO S3 PRO</h3>"
"<div class='view'><h4 id='m'>STANDBY</h4><img src='' id='s'></div>"
"<button id='tb' onclick='t()' class='btn blue'>START STREAM</button>"
"<button onclick='p()' class='btn green'>VIEW 1080P PHOTO</button>"
"<a href='/save' class='btn green'>SAVE TO SD</a>"
"<script>"
"var st=false; var s=document.getElementById('s'); var b=document.getElementById('tb'); var m=document.getElementById('m');"
"function t(){ if(!st){ s.src='/stream'; s.style.display='block'; m.style.display='none'; b.innerHTML='STOP'; b.className='btn red'; st=true; }"
"else{ s.src=''; s.style.display='none'; m.style.display='block'; b.innerHTML='START STREAM'; b.className='btn blue'; st=false; window.stop(); }}"
"function p(){ if(st)t(); setTimeout(()=>{ window.location.href='/photo'; }, 700); }"
"</script></body></html>";

/* --- SD Card (SPI) --- */
esp_err_t init_sd_card() {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {.format_if_mount_failed=true,.max_files=5,.allocation_unit_size=16*1024};
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {.mosi_io_num=21,.miso_io_num=21,.sclk_io_num=7,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=4000};
    spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs=22; slot_config.host_id=host.slot;
    return esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, NULL);
}

/* --- Handlers --- */
esp_err_t index_handler(httpd_req_t *req) { return httpd_resp_send(req, index_html, strlen(index_html)); }

esp_err_t photo_handler(httpd_req_t *req) {
    sensor_t * s = esp_camera_sensor_get();
    s->set_framesize(s, FRAMESIZE_FHD);
    s->set_quality(s, 20); 
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    for(int i=0; i<4; i++){ camera_fb_t * f = esp_camera_fb_get(); if(f) esp_camera_fb_return(f); }
    
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    
    // FORCE A CLEAN DISCONNECT AFTER THE PHOTO
    httpd_resp_set_hdr(req, "Connection", "close"); 
    
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    
    s->set_framesize(s, FRAMESIZE_QVGA);
    return res;
}


esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    char* boundary = "\r\n--123456789000000000000987654321\r\n";
    char* part = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
    
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=123456789000000000000987654321");
    
    // TELL THE BROWSER NOT TO REUSE THIS SOCKET FOR PHOTOS
    httpd_resp_set_hdr(req, "Connection", "close"); 

    while(true) { 
        fb = esp_camera_fb_get();
        if (!fb) { res = ESP_FAIL; break; }
        char hdr[128];
        int hlen = sprintf(hdr, part, fb->len);
        res = httpd_resp_send_chunk(req, boundary, strlen(boundary));
        if(res == ESP_OK) res = httpd_resp_send_chunk(req, hdr, hlen);
        if(res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        if(res != ESP_OK) break;
        // Inside your stream_handler while(true) loop, at the very bottom:
        vTaskDelay(200 / portTICK_PERIOD_MS); // 200ms delay = exactly 5 FPS
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return res;
}

esp_err_t save_handler(httpd_req_t *req) {
    sensor_t * s = esp_camera_sensor_get();
    s->set_framesize(s, FRAMESIZE_FHD);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    for(int i=0; i<4; i++) { camera_fb_t * f = esp_camera_fb_get(); if(f) esp_camera_fb_return(f); }
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) return httpd_resp_send_500(req);

    static int img_count = 0;
    char path[32]; sprintf(path, MOUNT_POINT "/pic_%d.jpg", img_count++);
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(fb->buf, 1, fb->len, f); fclose(f); httpd_resp_send(req, "Saved!", 6); }
    esp_camera_fb_return(fb);
    s->set_framesize(s, FRAMESIZE_QVGA);
    return ESP_OK;
}

/* --- WiFi --- */
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void app_main(void) {
    nvs_flash_init();
    init_sd_card();

    // 1. INIT AT MAXIMUM RESOLUTION (FHD)
    camera_config_t config = {
        .pin_pwdn=-1,.pin_reset=-1,.pin_xclk=10,.pin_sscb_sda=40,.pin_sscb_scl=39,
        .pin_d7=48,.pin_d6=11,.pin_d5=12,.pin_d4=14,.pin_d3=16,.pin_d2=18,.pin_d1=17,.pin_d0=15,
        .pin_vsync=38,.pin_href=47,.pin_pclk=13,.xclk_freq_hz=20000000,
        .pixel_format=PIXFORMAT_JPEG,
        .frame_size=FRAMESIZE_FHD,   // <-- CRITICAL: Forces large DMA buffer allocation
        .jpeg_quality=15,            // <-- Base quality for the big photo
        .fb_count=2,
        .fb_location=CAMERA_FB_IN_PSRAM,
        .grab_mode=CAMERA_GRAB_WHEN_EMPTY
    };
    
    esp_camera_init(&config);
    

    // 2. IMMEDIATELY DOWNSCALE FOR STREAMING
    sensor_t* s = esp_camera_sensor_get();
    if(s) { 
        s->set_vflip(s, 1); 
        s->set_hmirror(s, 0); 
        s->set_framesize(s, FRAMESIZE_QVGA); 
        s->set_quality(s, 8); // <-- Drop to 8 or 10 for near-lossless QVGA
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID, 
            .password = WIFI_PASS, 
            .threshold.authmode = WIFI_AUTH_WPA2_PSK
        }
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    // SERVER CONFIG
    httpd_config_t server_cfg = HTTPD_DEFAULT_CONFIG();
    server_cfg.lru_purge_enable = true;
    server_cfg.stack_size = 12288; 
    
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &server_cfg) == ESP_OK) {
        httpd_uri_t uri_idx={.uri="/",.method=HTTP_GET,.handler=index_handler};
        httpd_uri_t uri_str={.uri="/stream",.method=HTTP_GET,.handler=stream_handler};
        httpd_uri_t uri_pho={.uri="/photo",.method=HTTP_GET,.handler=photo_handler};
        httpd_uri_t uri_sav={.uri="/save",.method=HTTP_GET,.handler=save_handler};
        httpd_register_uri_handler(server, &uri_idx);
        httpd_register_uri_handler(server, &uri_str);
        httpd_register_uri_handler(server, &uri_pho);
        httpd_register_uri_handler(server, &uri_sav);
    }
}
