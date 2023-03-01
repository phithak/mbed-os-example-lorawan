/**
 * Copyright (c) 2017, Arm Limited and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdio.h>
#include "mbed.h"

#include "lorawan/LoRaWANInterface.h"
#include "lorawan/system/lorawan_data_structures.h"
#include "events/EventQueue.h"

// Application helpers
#include "DummySensor.h"
#include "trace_helper.h"
#include "lora_radio_helper.h"

static BufferedSerial pc(USBTX, USBRX);

using namespace events;

// Max payload size can be LORAMAC_PHY_MAXPAYLOAD.
// This example only communicates with much shorter messages (<30 bytes).
// If longer messages are used, these buffers must be changed accordingly.
uint8_t tx_buffer[222];
uint8_t rx_buffer[30];

/*
 * Sets up an application dependent transmission timer in ms. Used only when Duty Cycling is off for testing
 */
#define TX_TIMER                        10000

/**
 * Maximum number of events for the event queue.
 * 10 is the safe number for the stack events, however, if application
 * also uses the queue for whatever purposes, this number should be increased.
 */
#define MAX_NUMBER_OF_EVENTS            10

/**
 * Maximum number of retries for CONFIRMED messages before giving up
 */
#define CONFIRMED_MSG_RETRY_COUNTER     3

/**
 * Dummy pin for dummy sensor
 */
#define PC_9                            0

/**
 * Dummy sensor class object
 */
DS1820  ds1820(PC_9);

/**
* This event queue is the global event queue for both the
* application and stack. To conserve memory, the stack is designed to run
* in the same thread as the application and the application is responsible for
* providing an event queue to the stack that will be used for ISR deferment as
* well as application information event queuing.
*/
static EventQueue ev_queue(MAX_NUMBER_OF_EVENTS *EVENTS_EVENT_SIZE);

/**
 * Event handler.
 *
 * This will be passed to the LoRaWAN stack to queue events for the
 * application which in turn drive the application.
 */
static void lora_event_handler(lorawan_event_t event);

/**
 * Constructing Mbed LoRaWANInterface and passing it the radio object from lora_radio_helper.
 */
static LoRaWANInterface lorawan(radio);

/**
 * Application specific callbacks
 */
static lorawan_app_callbacks_t callbacks;

/**
 * Global variables for experiments
 */
int key_size;
int exp_func;
int payload_min;
int payload_max;
int payload_inc;
int round_per_payload;
int all_round;
int exp_round = 0;
int payload_size;
int msg_sent_count = 0;

#define IS_EXP_COMPUTE_MIC 1
#define IS_EXP_ENCRYPT_PAYLOAD 2
#define IS_EXP_KEYSIZE_128 1
#define IS_EXP_KEYSIZE_192 2
#define IS_EXP_KEYSIZE_256 3

/**
 * Entry point for application
 */
int main(void)
{
    char c[2];
    uint8_t buff[6];

    // Start program
    printf("\n\n\n\n\n");
    wait_us(5000000);   // wait for 5 seconds
    printf("\n\n\n\nSTART\n");
    wait_us(5000000);   // wait for 5 seconds
    printf("\n\n\n\nmain()\n");

    // Input from USB serial with format 
    // '...000[exp_func][key_size][payload_min]
    // [payload_max][payload_inc][round_per_payload]'
    while(1) {
        pc.read(c, 1);
        // Check the 1st '\0' char
        if (c[0] == '\0') {
            pc.read(c, 1);
            // Check the 2nd '\0' char
            if (c[0] == '\0') {
                pc.read(c, 1);
                // Check the 3rd '\0' char
                if (c[0] == '\0') {
                    break;
                } else {
                    continue;
                }
            } else {
                continue;
            }
        } else {
            continue;
        }
    }

    wait_us(200000);
    memset(buff, '\0', sizeof(buff));
    pc.read(buff, sizeof(buff));
    switch (buff[0]) {
        case IS_EXP_COMPUTE_MIC:
            exp_func = 'c';
            break;
        case IS_EXP_ENCRYPT_PAYLOAD: 
            exp_func = 'e';
            break;
        default:
            exp_func = 'c';
    }
    switch (buff[1]) {
        case IS_EXP_KEYSIZE_128:
            key_size = 128;
            break;
        case IS_EXP_KEYSIZE_192:
            key_size = 192;
            break;
        case IS_EXP_KEYSIZE_256:
            key_size = 256;
            break;
        default:
            key_size = 128;
    }
    payload_min = (int) buff[2];
    payload_max = (int) buff[3];
    payload_inc = (int) buff[4];
    round_per_payload = (int) buff[5];
    printf("exp_func = %c\n", exp_func);
    printf("key_size = %d\n", key_size);
    printf("payload_min = %d\n", payload_min);
    printf("payload_max = %d\n", payload_max);
    printf("payload_inc = %d\n", payload_inc);
    printf("round_per_payload = %d\n", round_per_payload);

    // Compute all_round
    if (payload_min == payload_max) {
        all_round = payload_min * round_per_payload;
    } else {
        all_round = (payload_max - payload_min) / payload_inc;
        all_round += ((payload_max - payload_min) % payload_inc) ? 1 : 0;
        ++all_round;
        all_round *= round_per_payload;
    }
    printf("all_round = %d\n", all_round);

    // setup tracing
    setup_trace();

    // stores the status of a call to LoRaWAN protocol
    lorawan_status_t retcode;

    // Initialize LoRaWAN stack
    if (lorawan.initialize(&ev_queue) != LORAWAN_STATUS_OK) {
        printf("LoRa initialization failed!\n");
        return -1;
    }

    printf("Mbed LoRaWANStack initialized\n");

    // prepare application callbacks
    callbacks.events = mbed::callback(lora_event_handler);
    lorawan.add_app_callbacks(&callbacks);

    // Set number of retries in case of CONFIRMED messages
    if (lorawan.set_confirmed_msg_retries(CONFIRMED_MSG_RETRY_COUNTER)
            != LORAWAN_STATUS_OK) {
        printf("set_confirmed_msg_retries failed!\n\n");
        return -1;
    }

    printf("CONFIRMED message retries : %d\n",
           CONFIRMED_MSG_RETRY_COUNTER);

    // Enable adaptive data rate
    if (lorawan.enable_adaptive_datarate() != LORAWAN_STATUS_OK) {
        printf("enable_adaptive_datarate failed!\n");
        return -1;
    }

    printf("Adaptive data  rate (ADR) - Enabled\n");

    retcode = lorawan.connect();

    if (retcode == LORAWAN_STATUS_OK ||
            retcode == LORAWAN_STATUS_CONNECT_IN_PROGRESS) {
    } else {
        printf("Connection error, code = %d\n", retcode);
        return -1;
    }

    printf("Connection - In Progress ...\n");

    // make your event queue dispatching events forever
    ev_queue.dispatch_forever();

    return 0;
}

/**
 * Sends a message to the Network Server
 */
static void send_message()
{
    uint16_t packet_len;
    int16_t retcode;
    int32_t sensor_value;

    char buffer_printf_format[7];

    if (exp_round == 0) {
        printf("START, round = %d\n", exp_round + 1);
        payload_size = payload_min;
    } else if (exp_round >= all_round) {
        printf("FINISH, round = %d\n", exp_round);
        //exp_round = 0;
        return;
    }

    printf("payload_size = %d, round = %d\n", payload_size, exp_round + 1);
    sprintf(buffer_printf_format, "%%0%dd", payload_size);

    if (ds1820.begin()) {
        ds1820.startConversion();
        sensor_value = ds1820.read(payload_size);
        printf("data = ");
        printf(buffer_printf_format, sensor_value);
        printf("\n");
        ds1820.startConversion();
    } else {
        printf("No sensor found\n");
        return;
    }

    packet_len = sprintf((char *) tx_buffer, buffer_printf_format,
                         sensor_value);

    retcode = lorawan.send(MBED_CONF_LORA_APP_PORT, tx_buffer, packet_len,
                           MSG_UNCONFIRMED_FLAG);

    printf("retcode = %d, payload_size = %d, round = %d\n", retcode, payload_size, exp_round + 1);

    if (retcode < 0) {
        retcode == LORAWAN_STATUS_WOULD_BLOCK ? printf("send - WOULD BLOCK\n")
        : printf("send() - Error code %d\n", retcode);

        if (retcode == LORAWAN_STATUS_WOULD_BLOCK) {
            //retry in 3 seconds
            if (MBED_CONF_LORA_DUTY_CYCLE_ON) {
                ev_queue.call_in(3000, send_message);
            }
        }
        return;
    }

    printf("%d bytes scheduled for transmission, payload_size = %d, round = %d\n", retcode, payload_size, exp_round + 1);
    memset(tx_buffer, 0, sizeof(tx_buffer));

    ++exp_round;

    if ((exp_round % round_per_payload) == 0) {
        ; // trigger here
        payload_size += payload_inc;
    }

    if (payload_size > payload_max) {
        payload_size = payload_max;
    }

    /*
    if (exp_round == all_round) {
        printf("FINISH, round = %d\n", exp_round);
    }
    */

}

/**
 * Receive a message from the Network Server
 */
static void receive_message()
{
    uint8_t port;
    int flags;
    int16_t retcode = lorawan.receive(rx_buffer, sizeof(rx_buffer), port, flags);

    if (retcode < 0) {
        printf("receive() - Error code %d\n", retcode);
        return;
    }

    printf(" RX Data on port %u (%d bytes): ", port, retcode);
    for (uint8_t i = 0; i < retcode; i++) {
        printf("%02x ", rx_buffer[i]);
    }
    printf("\n");
    
    memset(rx_buffer, 0, sizeof(rx_buffer));
}

/**
 * Event handler
 */
static void lora_event_handler(lorawan_event_t event)
{
    switch (event) {
        case CONNECTED:
            printf("Connection - Successful\n");
            if (MBED_CONF_LORA_DUTY_CYCLE_ON) {
                send_message();
            } else {
                ev_queue.call_every(TX_TIMER, send_message);
            }

            break;
        case DISCONNECTED:
            ev_queue.break_dispatch();
            printf("Disconnected Successfully\n");
            break;
        case TX_DONE:
            printf("Message Sent to Network Server, msg_sent_count = %d\n", ++msg_sent_count);
            if (MBED_CONF_LORA_DUTY_CYCLE_ON) {
                send_message();
            }
            break;
        case TX_TIMEOUT:
        case TX_ERROR:
        case TX_CRYPTO_ERROR:
        case TX_SCHEDULING_ERROR:
            printf("Transmission Error - EventCode = %d\n", event);
            // try again
            if (MBED_CONF_LORA_DUTY_CYCLE_ON) {
                send_message();
            }
            break;
        case RX_DONE:
            printf("Received message from Network Server\n");
            receive_message();
            break;
        case RX_TIMEOUT:
        case RX_ERROR:
            printf("Error in reception - Code = %d\n", event);
            break;
        case JOIN_FAILURE:
            printf("OTAA Failed - Check Keys\n");
            break;
        case UPLINK_REQUIRED:
            printf("Uplink required by NS\n");
            if (MBED_CONF_LORA_DUTY_CYCLE_ON) {
                send_message();
            }
            break;
        default:
            MBED_ASSERT("Unknown Event");
    }
}

// EOF
