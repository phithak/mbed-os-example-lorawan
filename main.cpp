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
uint8_t tx_buffer[30];
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
int all_round;
int current_round;
int increment;
int payload_start;
int payload_stop;
int key_size;
int round_per_payload;

/**
 * Entry point for application
 */
int main(void)
{
    char c[2];
    char buff[4];

    // Start program
    printf("\n\n\n\nSTART\n");

    // Input from USB serial with format '...000aaabbbcccdddeee'
    // aaa = number of bits (128, 192, 256)
    // bbb = number of beginning payload size (1 to 222)
    // ccc = number of ending pay load size (1 to 222)
    // ddd = number of increment
    // eee = number of rounds
    while(1) {
        pc.read(c, 1);
        // Check the 1st '0' char
        if (c[0] == '0') {
            pc.read(c, 1);
            // Check the 2nd '0' char
            if (c[0] == '0') {
                pc.read(c, 1);
                // Check the 3rd '0' char
                if (c[0] == '0') {
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

    // Cannot read too long USB serial input
    // Extract USB serial input to variables
    // Print i for debugging if needed
    int i;

    // key_size
    rtos::ThisThread::sleep_for(200);
    memset(buff, '\0', sizeof(buff));
    i = pc.read(buff, 3);
    key_size = atoi(buff);

    // payload_start
    rtos::ThisThread::sleep_for(200);
    memset(buff, '\0', sizeof(buff));
    i = pc.read(buff, 3);
    payload_start = atoi(buff);
    
    // payload_stop
    rtos::ThisThread::sleep_for(200);
    memset(buff, '\0', sizeof(buff));
    i = pc.read(buff, 3);
    payload_stop = atoi(buff);
    
    // increment
    rtos::ThisThread::sleep_for(200);
    memset(buff, '\0', sizeof(buff));
    i = pc.read(buff, 3);
    increment = atoi(buff);
    
    // round_per_payload
    rtos::ThisThread::sleep_for(200);
    memset(buff, '\0', sizeof(buff));
    i = pc.read(buff, 3);
    printf("\ni = %d\n", i);
    round_per_payload = atoi(buff);

    printf("key_size = %d\n", key_size);
    printf("payload_start = %d\n", payload_start);
    printf("payload_stop = %d\n", payload_stop);
    printf("increment = %d\n", increment);
    printf("round_per_payload = %d\n", round_per_payload);

    return 0;

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

    if (ds1820.begin()) {
        ds1820.startConversion();
        sensor_value = ds1820.read(10);
        printf("Dummy Sensor Value = %d\n", sensor_value);
        ds1820.startConversion();
    } else {
        printf("No sensor found\n");
        return;
    }

    packet_len = sprintf((char *) tx_buffer, "Dummy Sensor Value is %d",
                         sensor_value);

    retcode = lorawan.send(MBED_CONF_LORA_APP_PORT, tx_buffer, packet_len,
                           MSG_UNCONFIRMED_FLAG);

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

    printf("%d bytes scheduled for transmission\n", retcode);
    memset(tx_buffer, 0, sizeof(tx_buffer));
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
            printf("Message Sent to Network Server\n");
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
