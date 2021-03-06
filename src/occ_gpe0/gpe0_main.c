/* IBM_PROLOG_BEGIN_TAG                                                   */
/* This is an automatically generated prolog.                             */
/*                                                                        */
/* $Source: src/occ_gpe0/gpe0_main.c $                                    */
/*                                                                        */
/* OpenPOWER OnChipController Project                                     */
/*                                                                        */
/* Contributors Listed Below - COPYRIGHT 2015,2016                        */
/* [+] International Business Machines Corp.                              */
/*                                                                        */
/*                                                                        */
/* Licensed under the Apache License, Version 2.0 (the "License");        */
/* you may not use this file except in compliance with the License.       */
/* You may obtain a copy of the License at                                */
/*                                                                        */
/*     http://www.apache.org/licenses/LICENSE-2.0                         */
/*                                                                        */
/* Unless required by applicable law or agreed to in writing, software    */
/* distributed under the License is distributed on an "AS IS" BASIS,      */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or        */
/* implied. See the License for the specific language governing           */
/* permissions and limitations under the License.                         */
/*                                                                        */
/* IBM_PROLOG_END_TAG                                                     */

/// \file gpe0_main.c
/// \brief Sample program that creates and starts a thread
///
/// This file demonstrates how to create a thread and run it.  It also provides
/// an example of how to add traces to the code.

#include "pk.h"
#include "ipc_api.h"
#include "ipc_ping.h"

#define KERNEL_STACK_SIZE  256
#define MAIN_THREAD_STACK_SIZE 512

uint8_t        G_kernel_stack[KERNEL_STACK_SIZE];
uint8_t        G_main_thread_stack[MAIN_THREAD_STACK_SIZE];
PkThread       G_main_thread;

//statically initialize a ping command message
IPC_PING_CMD_CREATE(G_ping_cmd);

//statically initialize an IPC message queue
IPC_MSGQ_CREATE(G_gpe0_test_msgq0);

//statically initialize an IPC message queue message.  Responses to
//this message will automatically be placed on the message queue.
IPC_MSGQ_MSG_CREATE(G_test_msg, IPC_ST_TEST_FUNC0, &G_gpe0_test_msgq0);


// Main thread of execution. Currently does nothing
void main_thread(void* arg)
{
    volatile uint64_t t_ticks = 0;

    while(1)
    {
        pk_sleep(PK_MICROSECONDS(250));
        t_ticks++;
    }
}

// The main function is called by the boot code (after initializing some
// registers)
int main(int argc, char **argv)
{
    int rc;

    // initializes kernel data (stack, threads, timebase, timers, etc.)
    pk_initialize((PkAddress)G_kernel_stack,
                  KERNEL_STACK_SIZE,
                  PK_TIMEBASE_CONTINUES,
                  PPE_TIMEBASE_HZ);

    PK_TRACE("Kernel init completed");

    // Disable IPC's and register the IPC interrupt handler 
    rc = ipc_init();
    if(rc)
    {
        PK_TRACE("ipc_init failed with rc = 0x%08x", rc);
        pk_halt();
    }

    // enable IPC's
    rc = ipc_enable();
    if(rc)
    {
        PK_TRACE("ipc_enable failed with rc = 0x%08x", rc);
        pk_halt();
    }


    //Initialize the thread control block for G_main_thread
      pk_thread_create(&G_main_thread,
                      (PkThreadRoutine)main_thread,
                      (void*)NULL,
                      (PkAddress)G_main_thread_stack,
                      (size_t)MAIN_THREAD_STACK_SIZE,
                      (PkThreadPriority)1);

    //PK_TRACE_BIN("G_main_thread", &G_main_thread, sizeof(G_main_thread));

    //Make G_main_thread runnable
    pk_thread_resume(&G_main_thread);

    PK_TRACE("Starting thread(s)");

    // Start running the highest priority thread.
    // This function never returns
    pk_start_threads();

    return 0;
}
