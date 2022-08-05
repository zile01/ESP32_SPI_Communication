/* SPI Slave example, sender (uses SPI master driver)

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"

/*
SPI sender (master) example.

This example is supposed to work together with the SPI receiver. It uses the standard SPI pins (MISO, MOSI, SCLK, CS) to
transmit data over in a full-duplex fashion, that is, while the master puts data on the MOSI pin, the slave puts its own
data on the MISO pin.

This example uses one extra pin: GPIO_HANDSHAKE is used as a handshake pin. The slave makes this pin high as soon as it is
ready to receive/send data. This code connects this line to a GPIO interrupt which gives the rdySem semaphore. The main
task waits for this semaphore to be given before queueing a transmission.
*/


/*
Pins in use. The SPI Master can use the GPIO mux, so feel free to change these if needed.
*/

//TODO ja menjao

//SPI2
// #define GPIO_HANDSHAKE 2
// #define GPIO_CS 15
// #define GPIO_SCLK 14
// #define GPIO_MISO 12
// #define GPIO_MOSI 13

//SPI3
#define GPIO_HANDSHAKE 2
#define GPIO_CS 5
#define GPIO_SCLK 18
#define GPIO_MISO 19
#define GPIO_MOSI 23

// #if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
// #define GPIO_HANDSHAKE 2
// #define GPIO_MOSI 12
// #define GPIO_MISO 13
// #define GPIO_SCLK 15
// #define GPIO_CS 14

// #elif CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32H2
// #define GPIO_HANDSHAKE 3
// #define GPIO_MOSI 7
// #define GPIO_MISO 2
// #define GPIO_SCLK 6
// #define GPIO_CS 10

// #elif CONFIG_IDF_TARGET_ESP32S3
// #define GPIO_HANDSHAKE 2
// #define GPIO_MOSI 11
// #define GPIO_MISO 13
// #define GPIO_SCLK 12
// #define GPIO_CS 10

// #endif //CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2

#define SENDER_HOST SPI3_HOST

// #ifdef CONFIG_IDF_TARGET_ESP32
// #define SENDER_HOST HSPI_HOST

// #else
// #define SENDER_HOST SPI2_HOST

// #endif


//The semaphore indicating the slave is ready to receive stuff.
static QueueHandle_t rdySem;

/*
This ISR is called when the handshake line goes high.
*/
static void IRAM_ATTR gpio_handshake_isr_handler(void* arg)
{
    //Sometimes due to interference or ringing or something, we get two irqs after eachother. This is solved by
    //looking at the time between interrupts and refusing any interrupt too close to another one.
    static uint32_t lasthandshaketime_us;
    uint32_t currtime_us = esp_timer_get_time();
    uint32_t diff = currtime_us - lasthandshaketime_us;
    if (diff < 1000) {
        return; //ignore everything <1ms after an earlier irq
    }
    lasthandshaketime_us = currtime_us;

    //Give/release the semaphore.
    BaseType_t mustYield = false;
    xSemaphoreGiveFromISR(rdySem, &mustYield);
    if (mustYield) {
        portYIELD_FROM_ISR();
    }
}

char* sendbuf;
char* recvbuf;
char* recvbuf_temp;

typedef struct {
    uint8_t id;
    uint8_t len;
    uint8_t parity;
    char* message;
} sender;

typedef struct {
    uint8_t id;
    uint8_t len;
    uint8_t len_prim;
    uint8_t parity;
    uint8_t parity_prim;
    char* message;
} receiver;

sender s;
receiver r;

int formSendMessage(){
    s.parity = 0;

    memset(sendbuf, 0, 128);

    //Measuring parity
    s.parity ^= s.id;
    s.parity ^= s.len;

    //message[len] = 0

    for(int i = 0; i < s.len; ++i){
        s.parity ^= s.message[i];
    }
    
    int res = snprintf(sendbuf, 128, "%d:%d:%s:%d", s.id, s.len, s.message, s.parity); 

    return res;
}

int checkReceivedMessage(){
    char* pokazivac;
    char* pomocna;

    r.id = 0;
    r.len = 0;
    r.len_prim = 0;
    r.parity = 0;
    r.parity_prim = 0;

    //Taking informations from received message
    strcpy(recvbuf_temp, recvbuf);

    pokazivac = recvbuf_temp;

    int i = 0;

    while ((pomocna = strsep (&recvbuf_temp, ":")) != NULL){
        switch (i){
            case 0:
                r.id = atoi (pomocna);
                break;
            case 1:
                r.len = atoi (pomocna);
                break;
            case 2:
                r.message = pomocna;
                break;
            case 3:
                r.parity = atoi (pomocna);
                break;
            default:
                break;
        }
        ++i;
    }

    recvbuf_temp = pokazivac;

    //Len
    r.len_prim = strlen(r.message);

    //Measuring parity
    r.parity_prim ^= r.id;
    r.parity_prim ^= r.len;

    //message_s[len] = 0

    for(i = 0; i < r.len_prim; ++i){
        r.parity_prim ^= r.message[i];
    }

    //Checking parity matching
    if((r.parity == r.parity_prim) && (r.len_prim > 0)){
        return 1;
    }else{
        return 0;
    }
}

//Main application
void app_main(void)
{
    esp_err_t ret;
    spi_device_handle_t handle;

    //Configuration for the SPI bus
    spi_bus_config_t buscfg={
        .mosi_io_num=GPIO_MOSI,
        .miso_io_num=GPIO_MISO,
        .sclk_io_num=GPIO_SCLK,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1
    };

    //Configuration for the SPI device on the other side of the bus
    spi_device_interface_config_t devcfg={
        .command_bits=0,
        .address_bits=0,
        .dummy_bits=0,
        .clock_speed_hz=5000000,
        .duty_cycle_pos=128,        //50% duty cycle
        .mode=0,
        .spics_io_num=GPIO_CS,
        .cs_ena_posttrans=3,        //Keep the CS low 3 cycles after transaction, to stop slave from missing the last bit when CS has less propagation delay than CLK
        .queue_size=3
    };

    //GPIO config for the handshake line.
    gpio_config_t io_conf={
        .intr_type=GPIO_INTR_POSEDGE,
        .mode=GPIO_MODE_INPUT,
        .pull_up_en=1,
        .pin_bit_mask=(1<<GPIO_HANDSHAKE)
    };

    //Set up variables
    //char message[128] = {0};
    //char sendbuf[128] = {0};
    //char recvbuf[128] = {0};

    sendbuf = malloc (128 * sizeof (char));
    recvbuf = malloc (128 * sizeof (char));
    recvbuf_temp = malloc (128 * sizeof (char));
    r.message = malloc (128 * sizeof (char));
    s.message = malloc (128 * sizeof (char));

    memset(r.message, 0, 128);
    memset(s.message, 0, 128);
    memset(sendbuf, 0, 128);
    memset(recvbuf, 0, 128);
    memset(recvbuf_temp, 0, 128);

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));

    //Create the semaphore.
    rdySem=xSemaphoreCreateBinary();

    //Set up handshake line interrupt.
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_set_intr_type(GPIO_HANDSHAKE, GPIO_INTR_POSEDGE);
    gpio_isr_handler_add(GPIO_HANDSHAKE, gpio_handshake_isr_handler, NULL);

    //Initialize the SPI bus and add the device we want to send stuff to.
    ret=spi_bus_initialize(SENDER_HOST, &buscfg, SPI_DMA_CH_AUTO);
    assert(ret==ESP_OK);
    ret=spi_bus_add_device(SENDER_HOST, &devcfg, &handle);
    assert(ret==ESP_OK);

    //Assume the slave is ready for the first transmission: if the slave started up before us, we will not detect positive edge on the handshake line.
    xSemaphoreGive(rdySem);

    //Initialising the basic parameters
    s.id = 0;
    s.len = strlen("Hello from Master!");
    snprintf(s.message, s.len + 1, "Hello from Master!"); //vraca mi pravu duzinu, bez \0

    while(1) {
        memset(recvbuf, 0, 128);
        memset(recvbuf_temp, 0, 128);

        int i;
        
        int res = formSendMessage();
        
        if (res >= 128) {
            printf("Data truncated\n");
        }

        t.length=128*8;
        t.tx_buffer=sendbuf;
        t.rx_buffer=recvbuf;

        //Wait for slave to be ready for next byte before sending
        xSemaphoreTake(rdySem, portMAX_DELAY); //Wait until slave is ready
        ret=spi_device_transmit(handle, &t);    //Send a SPI transaction, wait for it to complete, and return the result
        
        //TODO proveri primljenu poruku
        
        int ret = checkReceivedMessage();

         if(ret == 1){
            printf("Master Received: %s\n", r.message);
        }

        //Sleep, If there is no sleep included, SPI communication will stop after first exchange

        vTaskDelay(10);
        //vTaskDelay(1000 / portTICK_PERIOD_MS);
        //usleep(2000);
        //vTaskDelay(portTICK_PERIOD_MS);

        ++s.id;
    }

    //Never reached.
    ret=spi_bus_remove_device(handle);
    assert(ret==ESP_OK);
}
