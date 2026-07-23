//-----------------------------------------------------------------------------
// Copyright 2018 Thiago Alves
// This file is part of the OpenPLC Runtime.
//
// OpenPLC is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// OpenPLC is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with OpenPLC.  If not, see <http://www.gnu.org/licenses/>.
//------
//
// This is the main file for the OpenPLC. It contains the initialization
// procedures for the hardware, network and the main loop
// Thiago Alves, Jun 2018
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <iostream>
#include <fstream>
#include <cstdio>
#ifdef __cplusplus
extern "C" {
#endif
#include <tee_client_api.h>
#include <cfi_ta.h>
#ifdef __cplusplus
}
#endif
#include "iec_types.h"
#include "ladder.h"
#ifdef _ethercat_src
#include "ethercat_src.h"
#endif

#include "oplc_snap7.h"

#include "openplc_cfi.h"

#define OPLC_CYCLE          50000000

#define CFI_PROTECT 1

extern int opterr;
IEC_BOOL __DEBUG;

extern cfi_runtime_state_t g_cfi_state;
extern TEEC_Session sess;
unsigned long __tick = 0;
pthread_mutex_t bufferLock; //mutex for the internal buffers
int run_openplc = 1000; //Variable to control OpenPLC Runtime execution
// int run_openplc = 100000
// pointers to IO *array[const][const] from cpp to c and back again don't work as expected, so instead callbacks
uint8_t *bool_input_call_back(int a, int b){ return bool_input[a][b]; }
uint8_t *bool_output_call_back(int a, int b){ return bool_output[a][b]; }
uint8_t *byte_input_call_back(int a){ return byte_input[a]; }
uint8_t *byte_output_call_back(int a){ return byte_output[a]; }
uint16_t *int_input_call_back(int a){ return int_input[a]; }
uint16_t *int_output_call_back(int a){ return int_output[a]; }
uint32_t *dint_input_call_back(int a){ return dint_input[a]; }
uint32_t *dint_output_call_back(int a){ return dint_output[a]; }
uint64_t *lint_input_call_back(int a){ return lint_input[a]; }
uint64_t *lint_output_call_back(int a){ return lint_output[a]; }
void logger_callback(char *msg){ openplc_log(msg);}
int main(int argc,char **argv)
{
    const char* filename = "my_logs/cycle_timing.csv"; // default
    if (argc >= 2) {
        filename = argv[1];
    }

    // Define the max/min/avg/total cycle and latency variables used in REAL-TIME computation(in nanoseconds)
    long cycle_avg, cycle_max, cycle_min, cycle_total;
    long latency_avg, latency_max, latency_min, latency_total;
    cycle_max = 0;
    cycle_min = LONG_MAX;
    cycle_total = 0;
    latency_max = 0;
    latency_min = LONG_MAX;
    latency_total = 0;

    #if CFI_PROTECT
    if (cfi_init() != 0) {
        fprintf(stderr, "CFI init failed\n");
        return 1;
    }

    if (cfi_got_init() != 0) {
        fprintf(stderr, "CFI GOT init failed\n");
    }

    /* Start Runtime Stage Layer 3: Operator Command Server */
    cfi_start_operator_server();

    #endif


    char log_msg[1000];
    sprintf(log_msg, "OpenPLC Runtime starting...\n");
    openplc_log(log_msg);

    //======================================================
    //                 PLC INITIALIZATION
    //======================================================
    tzset();
    time(&start_time);
    pthread_t interactive_thread;
    #if CFI_PROTECT
    /* CFI FWD: Register interactive server thread */
    CFI_PTHREAD_CREATE(&interactive_thread, NULL, interactiveServerThread, NULL);
    #else
    pthread_create(&interactive_thread, NULL, interactiveServerThread, NULL);
    #endif
    config_init__();
    glueVars();

    //======================================================
    //               MUTEX INITIALIZATION
    //======================================================
    if (pthread_mutex_init(&bufferLock, NULL) != 0)
    {
        printf("Mutex init failed\n");
        exit(1);
    }

    //======================================================
    //              HARDWARE INITIALIZATION
    //======================================================
#ifdef _ethercat_src
    type_logger_callback logger = logger_callback; 
    ethercat_configure("../utils/ethercat_src/build/ethercat.cfg", logger);
#endif
    initializeHardware();
    initializeMB();

    updateBuffersIn();
    updateBuffersOut();

    //======================================================
    //          PERSISTENT STORAGE INITIALIZATION
    //======================================================
    glueVars();
    mapUnusedIO();
    readPersistentStorage();
    //pthread_t persistentThread;
    //pthread_create(&persistentThread, NULL, persistentStorage, NULL);

    //======================================================
    //            S7 PROTOCOL INITIALIZATION
    //======================================================
    initializeSnap7();

#ifdef __linux__
    //======================================================
    //              REAL-TIME INITIALIZATION
    //======================================================
    // Set our thread to real time priority
    struct sched_param sp;
    sp.sched_priority = 30;
    printf("Setting main thread priority to RT\n");
    if(pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp))
    {
        printf("WARNING: Failed to set main thread to real-time priority\n");
    }

    // Lock memory to ensure no swapping is done.
    printf("Locking main thread memory\n");
    if(mlockall(MCL_FUTURE|MCL_CURRENT))
    {
        printf("WARNING: Failed to lock memory\n");
    }
#endif

    // Define the start, end, cycle time and latency time variables
    struct timespec cycle_start, cycle_end, cycle_time;
    struct timespec timer_start, timer_end, sleep_latency;

    //gets the starting point for the clock
    printf("Getting current time\n");
    clock_gettime(CLOCK_MONOTONIC, &timer_start);

    //======================================================
    //                    MAIN LOOP
    //======================================================
    int cur_cycle = 0;
    printf("run_openplc is %d\n", run_openplc);
#define MAX_CYCLE_RECORDS 1000 
//#define MAX_CYCLE_RECORDS 100000
    long per_cycle_us[MAX_CYCLE_RECORDS];      // Execution time per cycle (us)
    //long per_latency_us[MAX_CYCLE_RECORDS];    // Sleep latency per cycle (us)
    int total_recorded = 0;
    while(run_openplc)
    {

	run_openplc -= 1;
	if (run_openplc % 100 == 0) {
	    printf("run_openplc is %d\n", run_openplc);
	}
	#if CFI_PROTECT
        /* Update global cycle counter for Runtime Stage */
        const cfi_runtime_state_t* cfi_st = cfi_get_runtime_state();
        g_cfi_state.cycle_count = cur_cycle;
	if (cur_cycle == 100) {
	    //printf("cur_cycle is %d\n", cur_cycle);
	    cfi_freeze();	
	}
	cur_cycle += 1;
    	/* Adaptive integrity check (Runtime Stage Layer 2) */
    	static int last_check_cycle = 0;
    	int period = cfi_get_current_integrity_period();
    	if ((cur_cycle - last_check_cycle) >= period) {
        	int result = cfi_verify_integrity_safe();
        	if (result == 1) {
            		fprintf(stderr, "[PLC] CFI Integrity Check FAILED!\n");
            		/* Auto-adapt: tighten period on failure */
            		cfi_adapt_integrity_period(CFI_RUNTIME_STATUS_DEGRADED);
        	}
		int got_result = cfi_got_verify();
        	if (got_result == 1) {
            		fprintf(stderr, "[PLC] CFI GOT Check FAILED!\n");
        	}
        	last_check_cycle = cur_cycle;
    	}

        /* Check operator safety stop request (Runtime Stage Layer 3) */
        if (cfi_safety_stop_requested) {
            fprintf(stderr, "[PLC] Safety stop requested by operator. Stopping...\n");
            run_openplc = 0;
        }
	if (cfi_get_protection_mode() == CFI_MODE_SURVIVAL && cur_cycle % 100 == 0) {
	    struct timespec TEE_integrity_start, TEE_integrity_end;
	    clock_gettime(CLOCK_MONOTONIC, &TEE_integrity_start);
	    uint64_t TEE_start, TEE_end;
	    TEE_start = (uint64_t)TEE_integrity_start.tv_sec * 1000000000ULL + TEE_integrity_start.tv_nsec;
	    TEEC_Operation op = {0};
	    uint32_t err;
	    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_OUTPUT, TEEC_NONE, TEEC_NONE, TEEC_NONE);
    	    TEEC_Result res = TEEC_InvokeCommand(&sess, TA_CFI_CMD_VERIFY_GOLDEN_TABLE_INTERNAL, &op, &err);
    	    clock_gettime(CLOCK_MONOTONIC, &TEE_integrity_end);
    	    TEE_end = (uint64_t)TEE_integrity_end.tv_sec * 1000000000ULL + TEE_integrity_end.tv_nsec;
    	    printf("TEE internel integrity check in microsecond is %ld\n", (TEE_end-TEE_start)/1000);
            if (res== TEEC_SUCCESS && op.params[0].value.a == CFI_INTEGRITY_FAIL) {
               fprintf(stderr, "TEE internal golden table tamperd!\n");
	    }
	}
    	#endif

        // Get the start time for the running cycle
        clock_gettime(CLOCK_MONOTONIC, &cycle_start);

        //make sure the buffer pointers are correct and
        //attached to the user variables
        glueVars();
        
#ifdef _ethercat_src
        #if CFI_PROTECT
        /* CFI FWD: EtherCAT callback registration */
        CFI_CALLBACK_ASSIGN(bool_input_callback, bool_input_call_back, 0xE1000001U);
        CFI_CALLBACK_ASSIGN(bool_output_callback, bool_output_call_back, 0xE1000002U);
        CFI_CALLBACK_ASSIGN(byte_input_callback, byte_input_call_back, 0xE1000003U);
        CFI_CALLBACK_ASSIGN(byte_output_callback, byte_output_call_back, 0xE1000004U);
        CFI_CALLBACK_ASSIGN(int_input_callback, int_input_call_back, 0xE1000005U);
        CFI_CALLBACK_ASSIGN(int_output_callback, int_output_call_back, 0xE1000006U);
        CFI_CALLBACK_ASSIGN(dint_input_callback, dint_input_call_back, 0xE1000007U);
        CFI_CALLBACK_ASSIGN(dint_output_callback, dint_output_call_back, 0xE1000008U);
        CFI_CALLBACK_ASSIGN(lint_input_callback, lint_input_call_back, 0xE1000009U);
        CFI_CALLBACK_ASSIGN(lint_output_callback, lint_output_call_back, 0xE100000AU);
        #else
        boolvar_call_back bool_input_callback = bool_input_call_back;
        boolvar_call_back bool_output_callback = bool_output_call_back;
        int8var_call_back byte_input_callback = byte_input_call_back;
        int8var_call_back byte_output_callback = byte_output_call_back;
        int16var_call_back int_input_callback = int_input_call_back;
        int16var_call_back int_output_callback = int_output_call_back;
        int32var_call_back dint_input_callback = dint_input_call_back;
        int32var_call_back dint_output_callback = dint_output_call_back;
        int64var_call_back lint_input_callback = lint_input_call_back;
        int64var_call_back lint_output_callback = lint_output_call_back;
        #endif
#endif
        
        updateBuffersIn(); //read input image

        pthread_mutex_lock(&bufferLock); //lock mutex


#ifdef _ethercat_src
        if(ethercat_callcyclic(BUFFER_SIZE, 
                bool_input_callback, 
                bool_output_callback, 
                byte_input_callback, 
                byte_output_callback, 
                int_input_callback, 
                int_output_callback, 
                dint_input_call_back, 
                dint_output_call_back, 
                lint_input_call_back, 
                lint_output_call_back)){
            printf("EtherCAT cyclic failed\n");
            break;
        }
#endif
        updateBuffersIn_MB(); //update input image table with data from slave devices
        handleSpecialFunctions();
	
        config_run__(__tick++); // execute plc program logic
	
        updateBuffersOut_MB(); //update slave devices with data from the output image table
        pthread_mutex_unlock(&bufferLock); //unlock mutex

        updateBuffersOut(); //write output image
        
        updateTime();

        // Get the end time for the running cycle
        clock_gettime(CLOCK_MONOTONIC, &cycle_end);
        // Compute the time usage in one cycle and do max/min/total comparison/recording
        timespec_diff(&cycle_end, &cycle_start, &cycle_time);
/* ====== Record the execution time (in microseconds) for each cycle ====== */
        if (total_recorded < MAX_CYCLE_RECORDS) {
            per_cycle_us[total_recorded] = (long)(cycle_time.tv_sec * 1000000LL 
                                                  + cycle_time.tv_nsec / 1000);
        total_recorded++; 
	}
        if (cycle_time.tv_nsec > cycle_max)
            cycle_max = cycle_time.tv_nsec;
        if (cycle_time.tv_nsec < cycle_min)
            cycle_min = cycle_time.tv_nsec;
        cycle_total = cycle_total + cycle_time.tv_nsec;

        sleep_until(&timer_start, common_ticktime__);

        // Get the sleep end point which is also the start time/point of the next cycle
        clock_gettime(CLOCK_MONOTONIC, &timer_end);
        // Compute the time latency of the next cycle(caused by sleep) and do max/min/total comparison/recording
        timespec_diff(&timer_end, &timer_start, &sleep_latency);
        if (sleep_latency.tv_nsec > latency_max)
            latency_max = sleep_latency.tv_nsec;
        if (sleep_latency.tv_nsec < latency_min)
            latency_min = sleep_latency.tv_nsec;
        latency_total = latency_total + sleep_latency.tv_nsec;

        // Store the cycle_time/sleep_latency in microsecond, so it can be displayed in the webpage
        RecordCycletimeLatency((long)cycle_time.tv_nsec / 1000, (long)sleep_latency.tv_nsec / 1000);
    
   }

    #if CFI_PROTECT
    cfi_fini();
    #endif

    #if CFI_TIMING
    cfi_print_timing_stats();
    #endif

    /* Runtime Stage Layer 1: Print final status banner and event log */
    #if CFI_PROTECT
    cfi_print_status_banner();
    cfi_print_event_log();
    cfi_stop_operator_server();
    #endif
    /* ====== Export to CSV file for easy Python/Matlab analysis ====== */
    FILE *cycle_fp = fopen(filename, "w");
    if (cycle_fp) {
        fprintf(cycle_fp, "cycle,execution_us,latency_us\n");
        for (int i = 0; i < total_recorded; i++) {
            fprintf(cycle_fp, "%d,%ld\n", i, per_cycle_us[i]);
        }
        fclose(cycle_fp);
        printf("[PER-CYCLE] Details saved to cycle_timing.csv\n");
    } else {
        perror("[PER-CYCLE] Failed to write cycle_timing.csv");
    }

    // Compute/print the max/min/avg cycle time and latency
    cycle_avg = (long)cycle_total / __tick;
    latency_avg = (long)latency_total / __tick;
    printf("###Summary: The maximum/minimum/average cycle time in microsecond is %ld/%ld/%ld\n",
    cycle_max / 1000, cycle_min / 1000, cycle_avg / 1000);
    printf("###Summary: The maximum/minimum/average latency in microsecond is %ld/%ld/%ld\n",
    latency_max / 1000, latency_min / 1000, latency_avg / 1000);

    //======================================================
    //             SHUTTING DOWN OPENPLC RUNTIME  
    //======================================================
    pthread_join(interactive_thread, NULL);
#ifdef _ethercat_src
    ethercat_terminate_src();
#endif

    finalizeSnap7();
    printf("Disabling outputs\n");
    disableOutputs();
    updateBuffersOut();
    finalizeHardware();
    printf("Shutting down OpenPLC Runtime...\n");

   
    exit(0);
}
