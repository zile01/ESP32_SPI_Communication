/* SPI Slave example, receiver (uses SPI Slave driver to communicate with sender)

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
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"


/*
SPI receiver (slave) example.

This example is supposed to work together with the SPI sender. It uses the standard SPI pins (MISO, MOSI, SCLK, CS) to
transmit data over in a full-duplex fashion, that is, while the master puts data on the MOSI pin, the slave puts its own
data on the MISO pin.

This example uses one extra pin: GPIO_HANDSHAKE is used as a handshake pin. After a transmission has been set up and we're
ready to send/receive data, this code uses a callback to set the handshake pin high. The sender will detect this and start
sending a transaction. As soon as the transaction is done, the line gets set low again.
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

#define RCV_HOST    SPI3_HOST

// #ifdef CONFIG_IDF_TARGET_ESP32
// #define RCV_HOST    HSPI_HOST

// #else
// #define RCV_HOST    SPI2_HOST

// #endif

//Called after a transaction is queued and ready for pickup by master. We use this to set the handshake line high.
void my_post_setup_cb(spi_slave_transaction_t *trans) {
    gpio_set_level(GPIO_HANDSHAKE, 1);
}

//Called after transaction is sent/received. We use this to set the handshake line low.
void my_post_trans_cb(spi_slave_transaction_t *trans) {
    gpio_set_level(GPIO_HANDSHAKE, 0);
}

char* sendbuf;
char* recvbuf;
char* recvbuf_temp;

typedef struct {
    uint8_t id;
    uint8_t len;
    uint8_t len_prim;
    uint8_t parity;
    uint8_t parity_prim;
    char* message;
} sender;

typedef struct {
    uint8_t id;
    uint8_t len;
    uint8_t parity;
    char* message;
} receiver;

sender s;
receiver r;

int formSendMessage(){
    r.parity = 0;

    memset(sendbuf, 0, 128);

    //Measuring parity
    r.parity ^= r.id;
    r.parity ^= r.len;

    //message[len] = 0

    for(int i = 0; i < r.len; ++i){
        r.parity ^= r.message[i];
    }
    
    int res = snprintf(sendbuf, 128, "%d:%d:%s:%d", r.id, r.len, r.message, r.parity); 

    return res;
}

int checkReceivedMessage(){
    char* pokazivac;
    char* pomocna;

    s.id = 0;
    s.len = 0;
    s.len_prim = 0;
    s.parity = 0;
    s.parity_prim = 0;

    //Taking informations from received message
    strcpy(recvbuf_temp, recvbuf);

    pokazivac = recvbuf_temp;

    int i = 0;

    while ((pomocna = strsep (&recvbuf_temp, ":")) != NULL){
        switch (i){
            case 0:
                s.id = atoi (pomocna);
                break;
            case 1:
                s.len = atoi (pomocna);
                break;
            case 2:
                s.message = pomocna;
                break;
            case 3:
                s.parity = atoi (pomocna);
                break;
            default:
                break;
        }
        ++i;
    }

    recvbuf_temp = pokazivac;

    //Len
    s.len_prim = strlen(s.message);

    //Measuring parity
    s.parity_prim ^= s.id;
    s.parity_prim ^= s.len;

    //message[len] = 0

    for(i = 0; i < s.len_prim; ++i){
        s.parity_prim ^= s.message[i];
    }

    //Checking parity matching
    if((s.parity == s.parity_prim) && (s.len_prim > 0)){
        return 1;
    }else{
        return 0;
    }
}

//Main application
void app_main(void)
{
    esp_err_t ret;

    //Configuration for the SPI bus
    spi_bus_config_t buscfg={
        .mosi_io_num=GPIO_MOSI,
        .miso_io_num=GPIO_MISO,
        .sclk_io_num=GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    //Configuration for the SPI slave interface
    spi_slave_interface_config_t slvcfg={
        .mode=0,
        .spics_io_num=GPIO_CS,
        .queue_size=3,
        .flags=0,
        .post_setup_cb=my_post_setup_cb,
        .post_trans_cb=my_post_trans_cb
    };

    //Configuration for the handshake line
    gpio_config_t io_conf={
        .intr_type=GPIO_INTR_DISABLE,
        .mode=GPIO_MODE_OUTPUT,
        .pin_bit_mask=(1<<GPIO_HANDSHAKE)
    };

    //Configure handshake line as output
    gpio_config(&io_conf);
    //Enable pull-ups on SPI lines so we don't detect rogue pulses when no master is connected.
    gpio_set_pull_mode(GPIO_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_SCLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_CS, GPIO_PULLUP_ONLY);

    //Initialize SPI slave interface
    ret=spi_slave_initialize(RCV_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
    assert(ret==ESP_OK);

    //bilo 129
    //WORD_ALIGNED_ATTR char sendbuf[128]="";
    //WORD_ALIGNED_ATTR char recvbuf[128]="";
    sendbuf = malloc (128 * sizeof (char));
    recvbuf = malloc (128 * sizeof (char));
    recvbuf_temp = malloc (128 * sizeof (char));
    r.message = malloc (128 * sizeof (char));
    s.message = malloc (128 * sizeof (char));

    memset(sendbuf, 0, 128);
    memset(recvbuf, 0, 128);
    memset(recvbuf_temp, 0, 128);
    memset(r.message, 0, 128);
    memset(s.message, 0, 128);

    //bilo 33 umesto 129
    //memset(recvbuf, 0, 128);
    int res = snprintf(sendbuf, 128, "Hello from Slave!");
    spi_slave_transaction_t t;
    memset(&t, 0, sizeof(t));

    r.id = 0;
    r.len = strlen("Hello from Slave!");
    snprintf(r.message, r.len + 1, "Hello from Slave!");

    while(1) {
        memset(recvbuf, 0, 128);
        memset(recvbuf_temp, 0, 128);
        
        //TODO form sendbuf

        int res = formSendMessage();

        if (res >= 128) {
            printf("Data truncated\n");
        }

        //Set up a transaction of 128 bytes to send/receive
        t.length=128*8;
        t.tx_buffer=sendbuf;
        t.rx_buffer=recvbuf;
        /* This call enables the SPI slave interface to send/receive to the sendbuf and recvbuf. The transaction is
        initialized by the SPI master, however, so it will not actually happen until the master starts a hardware transaction
        by pulling CS low and pulsing the clock etc. In this specific example, we use the handshake line, pulled up by the
        .post_setup_cb callback that is called as soon as a transaction is ready, to let the master know it is free to transfer
        data.
        */
        ret=spi_slave_transmit(RCV_HOST, &t, portMAX_DELAY);
        //spi_slave_transmit does not return until the master has done a transmission, so by here we have sent our data and
        //received data from the master. Print it.

        int ret = checkReceivedMessage();

        if(ret == 1){
            printf("Slave Received: %s\n", s.message);
        }

        ++r.id;
    }
}
