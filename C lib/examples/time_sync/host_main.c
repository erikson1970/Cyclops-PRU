/*
 * Copyright (c) Regents of the University of California, 2017. All rights reserved.
 * See LICENSE and ATTRIB in the repository root.
 */

/*
 * Based on helloworld_capture.c, a simple C example showing how to register
 * to listen for capture events by Fatima Anwar and Andrew Symington.
 */

/*
 * Based on code from Derek Molloy for the book "Exploring BeagleBone:
 * Tools and Techniques for Building with Embedded Linux" by John Wiley & Sons,
 * 2014 ISBN 9781118935125. Please see the file ATTRIB in the repository root
 * directory for copyright and GNU GPLv3 license information.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <prussdrv.h>
#include <pruss_intc_mapping.h>

#include "shared_conf.h"
#include "nesl_pru_rbuffer.h"
#include "host_qot.h"

volatile int stop = 0;
volatile uint8_t *shared_mem;

struct rbuffer *send_to_pru_rbuffer;
struct rbuffer *receive_from_pru_rbuffer;

// Handles interrupts from the PRU
void *receive_pru_thread(void *value)
{
    while(!stop) {
        short status = 0;
        int64_t data;

        // This call will block until PRU interrupts the host
        prussdrv_pru_wait_event(PRU_EVTOUT_0);
        prussdrv_pru_clear_event(PRU_EVTOUT_0, PRU0_ARM_INTERRUPT);

        while(!status) {
            data = rbuf_read_uint64(receive_from_pru_rbuffer, &status);
            if (!status) {
                printf("PRU: %lld ns\n", data);
            }
        }
    }
}

// Handles sending QoT timestamp to the PRU
void *send_pru_thread(void *value)
{
    int err = 0;
    uint64_t nano_ts;

    while (!stop) {
        // This call will block until a QoT input capture event is triggered
        nano_ts = qot_read_event_ts(&err);
        rbuf_write_uint64(send_to_pru_rbuffer, nano_ts);
    }
}

int main (void)
{
    int err;

    if(getuid()!=0){
        printf("You must run this program as root. Exiting.\n");
        exit(EXIT_FAILURE);
    }

    pthread_t thread;
    pthread_t thread2;

    /* Initialize structure used by prussdrv_pruintc_intc   */
    /* PRUSS_INTC_INITDATA is found in pruss_intc_mapping.h */
    tpruss_intc_initdata pruss_intc_initdata = PRUSS_INTC_INITDATA;

    /* Initialize PRUSS (PRU Sub-System Driver) */
    prussdrv_init();

    // PRU_EVTOUT_0 - interrupt generated by PRU to indicate that it has
    // sent messages to the host. Pending messages are in the
    // receive_from_pru_rbuffer.
    err = prussdrv_open (PRU_EVTOUT_0);
    if(err){
        printf("Failed to open the PRU-ICSS, have you loaded the overlay?");
        exit(EXIT_FAILURE);
    }

    // PRU_EVTOUT_1 - interrupt generated by PRU to indicate that is is
    // going to halt.
    err = prussdrv_open (PRU_EVTOUT_1);
    if(err){
        printf("Failed to open the PRU-ICSS, have you loaded the overlay?");
        exit(EXIT_FAILURE);
    }

    /* Map PRU's INTC */
    prussdrv_pruintc_init(&pruss_intc_initdata);

    // Map PRU's shared memory into user-space
    if (prussdrv_map_prumem(PRUSS0_SHARED_DATARAM, (void **) &shared_mem)) {
        printf("map shared memory failed\n");
        exit(EXIT_FAILURE);
    }

    // Clear 12Kb of shared memory
    memset((void*) shared_mem, 0, 0x3000);

    // Setup buffers for communication between host and PRU

    // Only the sender initializes the rbuffer.
    // PRU will initialize this buffer. Host should NOT initalize this.
    receive_from_pru_rbuffer = (struct rbuffer *) (shared_mem + RBUF_ADDR);

    send_to_pru_rbuffer = (struct rbuffer *) (shared_mem + RBUF_ADDR + sizeof(struct rbuffer));
    init_rbuffer(send_to_pru_rbuffer);

    // Setup QoT
    if (init_qot("/dev/ptp1", 0)) {
        printf("Initialize QoT time sync failed\n");
        exit(EXIT_FAILURE);
    }

    /* Load the memory data file */
    prussdrv_load_datafile(PRU_NUM, "./data.bin");

    /* Load and execute binary on PRU */
    prussdrv_exec_program (PRU_NUM, "./text.bin");

    /* Thread for handling message from the PRU */
    if (pthread_create(&thread, NULL, &receive_pru_thread, NULL)){
        printf("Failed to create thread!\n");
        exit(EXIT_FAILURE);
    }

    /* Thread for sending messages to the PRU */
    if (pthread_create(&thread2, NULL, &send_pru_thread, NULL)){
        printf("Failed to create thread!\n");
        exit(EXIT_FAILURE);
    }

    /* Wait for event completion from PRU */
    // This call will block until PRU interrupts the host.
    prussdrv_pru_wait_event(PRU_EVTOUT_1);
    stop = 1;

    // Teardown QoT
    if (deinit_qot()) {
        printf("Deinitialize QoT time sync failed\n");
    }

    /* Disable PRU and close memory mappings */
    prussdrv_pru_disable(PRU_NUM);
    prussdrv_exit ();
    return(EXIT_SUCCESS);
}
