/* IBM_PROLOG_BEGIN_TAG                                                   */
/* This is an automatically generated prolog.                             */
/*                                                                        */
/* $Source: src/occ_405/pss/apss.c $                                      */
/*                                                                        */
/* OpenPOWER OnChipController Project                                     */
/*                                                                        */
/* Contributors Listed Below - COPYRIGHT 2011,2016                        */
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

#include "ssx.h"

#include <occhw_async.h>
#include <trac_interface.h>
#include <apss.h>
#include <occ_common.h>
#include <comp_ids.h>
#include <pss_service_codes.h>
#include <occ_service_codes.h>
#include <trac.h>
#include <state.h>
#include <occ_sys_config.h>
#include <dcom.h>
#include "pss_constants.h"

// Threshold for calling out the redundant APSS
#define MAX_BACKUP_FAILURES  8

// Configure both GPIOs (directoin/drive/interrupts): All Input, All 1's, No Interrupts
const apssGpioConfigStruct_t G_gpio_config[2] = { {0x00, 0xFF, 0x00}, {0x00, 0xFF, 0x00} };

// Configure streaming of: APSS Mode, 16 ADCs, 2 GPIOs
const apssModeConfigStruct_t G_apss_mode_config = { APSS_MODE_COMPOSITE, 16, 2 };

// Power Measurements (read from APSS every RealTime loop)
apssPwrMeasStruct_t G_apss_pwr_meas = { {0} };

GPE_BUFFER(initGpioArgs_t G_gpe_apss_initialize_gpio_args);
GPE_BUFFER(setApssModeArgs_t G_gpe_apss_set_mode_args);

uint64_t G_gpe_apss_time_start;
uint64_t G_gpe_apss_time_end;

// Flag for requesting APSS recovery when OCC detects all zeroes or data out of sync
bool G_apss_recovery_requested = FALSE;

GPE_BUFFER(apss_start_args_t    G_gpe_start_pwr_meas_read_args);
GPE_BUFFER(apss_continue_args_t G_gpe_continue_pwr_meas_read_args);
GPE_BUFFER(apss_complete_args_t G_gpe_complete_pwr_meas_read_args);

GpeRequest G_meas_start_request;
GpeRequest G_meas_cont_request;
GpeRequest G_meas_complete_request;

// Up / down counter for redundant apss failures
uint32_t G_backup_fail_count = 0;

#ifdef DEBUG_APSS_SEQ
uint32_t G_sequence_start = 0;
uint32_t G_sequence_cont = 0;
uint32_t G_sequence_complete = 0;
volatile uint32_t G_savedCompleteSeq = 0;
#endif

// Used to tell slave inbox that pwr meas is complete
volatile bool G_ApssPwrMeasCompleted = FALSE;

// Function Specification
//
// Name: dumpHexString
//
// Description: TODO Add description
//
// End Function Specification
#if ( (!defined(NO_TRAC_STRINGS)) && defined(TRAC_TO_SIMICS) )

// Utility function to dump hex data to screen
void dumpHexString(const void *i_data, const unsigned int len, const char *string)
{
  unsigned int i, j;
  char text[17];
  uint8_t *data = (uint8_t*)i_data;
  unsigned int l_len = len;

  text[16] = '\0';
  if (string != NULL)
  {
    printf("%s\n", string);
  }

  if (len > 0x0800) l_len = 0x800;
  for(i = 0; i < l_len; i++)
  {
    if (i % 16 == 0)
    {
      if (i > 0) printf("   \"%s\"\n", text);
      printf("   %04x:",i);
    }
    if (i % 4 == 0) printf(" ");

    printf("%02x",(int)data[i]);
    if (isprint(data[i])) text[i%16] = data[i];
    else text[i%16] = '.';
  }
  if ((i%16) != 0) {
    text[i%16] = '\0';
    for(j = (i % 16); j < 16; ++j) {
      printf("  ");
      if (j % 4 == 0) printf(" ");
    }
  }
  printf("   \"%s\"\n", text);
  return;
}
#endif

// Function Specification
//
// Name: do_apss_recovery
//
// Description: Collect FFDC and attempt recovery for APSS failures
//
// End Function Specification
void do_apss_recovery(void)
{
#define PSS_START_COMMAND 0x8000000000000000ull
#define PSS_RESET_COMMAND 0x4000000000000000ull
    int             l_scom_rc           = 0;
    uint32_t        l_scom_addr;
    uint64_t        l_spi_adc_ctrl0;
    uint64_t        l_spi_adc_ctrl1;
    uint64_t        l_spi_adc_ctrl2;
    uint64_t        l_spi_adc_status;
    uint64_t        l_spi_adc_reset;
    uint64_t        l_spi_adc_wdata;


    INTR_TRAC_ERR("detected invalid power data[%08x%08x]",
             (uint32_t)(G_gpe_continue_pwr_meas_read_args.meas_data[0] >> 32),
             (uint32_t)(G_gpe_continue_pwr_meas_read_args.meas_data[0] & 0x00000000ffffffffull));

    do
    {
        // Collect SPI ADC FFDC data
        l_scom_addr = SPIPSS_ADC_RESET_REG;
        l_scom_rc = _getscom(l_scom_addr, &l_spi_adc_reset, SCOM_TIMEOUT);
        if(l_scom_rc)
        {
            break;
        }
        l_scom_addr = SPIPSS_ADC_CTRL_REG0;
        l_scom_rc = _getscom(l_scom_addr, &l_spi_adc_ctrl0, SCOM_TIMEOUT);
        if(l_scom_rc)
        {
            break;
        }
        l_scom_addr = SPIPSS_ADC_CTRL_REG1;
        l_scom_rc = _getscom(l_scom_addr, &l_spi_adc_ctrl1, SCOM_TIMEOUT);
        if(l_scom_rc)
        {
            break;
        }
        l_scom_addr = SPIPSS_ADC_CTRL_REG2;
        l_scom_rc = _getscom(l_scom_addr, &l_spi_adc_ctrl2, SCOM_TIMEOUT);
        if(l_scom_rc)
        {
            break;
        }
        l_scom_addr = SPIPSS_ADC_STATUS_REG;
        l_scom_rc = _getscom(l_scom_addr, &l_spi_adc_status, SCOM_TIMEOUT);
        if(l_scom_rc)
        {
            break;
        }
        l_scom_addr = SPIPSS_ADC_WDATA_REG;
        l_scom_rc = _getscom(l_scom_addr, &l_spi_adc_wdata, SCOM_TIMEOUT);
        if(l_scom_rc)
        {
            break;
        }

        INTR_TRAC_ERR("70000[%08x] 70001[%08x] 70002[%08x] 70003|70005[%08x] 70010[%08x]",
                (uint32_t)(l_spi_adc_ctrl0 >> 32),
                (uint32_t)(l_spi_adc_ctrl1 >> 32),
                (uint32_t)(l_spi_adc_ctrl2 >> 32),
                (uint32_t)((l_spi_adc_status >> 32) | (l_spi_adc_reset >> 48)), // Stuff reset register in lower 16 bits
                (uint32_t)(l_spi_adc_wdata >> 32));

        // Special error handling on OCC backup. Keep an up/down counter of
        // fail/success and log predictive error when we reach the limit.
        if(G_occ_role == OCC_SLAVE)
        {
            if(G_backup_fail_count < MAX_BACKUP_FAILURES)
            {
                // Increment the up/down counter
                G_backup_fail_count++;
            }
            else
            {
                //We're logging the error so stop running apss tasks
                rtl_stop_task(TASK_ID_APSS_START);
                rtl_stop_task(TASK_ID_APSS_CONT);
                rtl_stop_task(TASK_ID_APSS_DONE);

                INTR_TRAC_INFO("Redundant APSS has exceeded failure threshold.  Logging Error");

                /*
                 * @errortype
                 * @moduleid    PSS_MID_DO_APSS_RECOVERY
                 * @reasoncode  REDUNDANT_APSS_GPE_FAILURE
                 * @userdata1   0
                 * @userdata2   0
                 * @userdata4   OCC_NO_EXTENDED_RC
                 * @devdesc     Redundant APSS failure.  Power Management Redundancy Lost.
                 */
                errlHndl_t l_err = createErrl(PSS_MID_DO_APSS_RECOVERY,
                                              REDUNDANT_APSS_GPE_FAILURE,
                                              OCC_NO_EXTENDED_RC,
                                              ERRL_SEV_PREDICTIVE,
                                              NULL,
                                              DEFAULT_TRACE_SIZE,
                                              0,
                                              0);

                // APSS callout
                addCalloutToErrl(l_err,
                                 ERRL_CALLOUT_TYPE_HUID,
                                 G_sysConfigData.apss_huid,
                                 ERRL_CALLOUT_PRIORITY_HIGH);
                // Processor callout
                addCalloutToErrl(l_err,
                                 ERRL_CALLOUT_TYPE_HUID,
                                 G_sysConfigData.proc_huid,
                                 ERRL_CALLOUT_PRIORITY_LOW);
                // Backplane callout
                addCalloutToErrl(l_err,
                                 ERRL_CALLOUT_TYPE_HUID,
                                 G_sysConfigData.backplane_huid,
                                 ERRL_CALLOUT_PRIORITY_LOW);
                // Firmware callout
                addCalloutToErrl(l_err,
                                 ERRL_CALLOUT_TYPE_COMPONENT_ID,
                                 ERRL_COMPONENT_ID_FIRMWARE,
                                 ERRL_CALLOUT_PRIORITY_MED);

                commitErrl(&l_err);

                break;
            }
        }

        INTR_TRAC_INFO("Starting APSS recovery.  fail_count=%d", G_backup_fail_count);

        // Reset the ADC engine
        l_scom_addr = SPIPSS_ADC_RESET_REG;
        l_scom_rc = _putscom(l_scom_addr, PSS_RESET_COMMAND, SCOM_TIMEOUT);
        if(l_scom_rc)
        {
            break;
        }

        // Zero out the reset register
        l_scom_rc = _putscom(l_scom_addr, 0, SCOM_TIMEOUT);
        if(l_scom_rc)
        {
            break;
        }

        // Attempt recovery by sending the apss
        // command that was set up earlier by initialization GPE
        l_scom_addr = SPIPSS_P2S_COMMAND_REG;
        l_scom_rc = _putscom(l_scom_addr, PSS_START_COMMAND, SCOM_TIMEOUT);
        if(l_scom_rc)
        {
            break;
        }
    }while(0);

    // Just trace it if we get a scom failure trying to collect FFDC
    if(l_scom_rc)
    {
        INTR_TRAC_ERR("apss recovery scom failure. addr=0x%08x, rc=0x%08x", l_scom_addr, l_scom_rc);
    }
}

// Note: The complete request must be global, since it must stick around until after the
//       GPE program has completed (in order to do the callback).

// Function Specification
//
// Name:  task_apss_start_pwr_meas
//
// Description: Start the GPE program to request power measurement data from APSS
//              If previous call had failed, commit the error and request reset
//
// Task Flags: RTL_FLAG_MSTR, RTL_FLAG_OBS, RTL_FLAG_ACTIVE, RTL_FLAG_APSS_PRES
//
// End Function Specification
void task_apss_start_pwr_meas(struct task *i_self)
{
    int             l_rc                = 0;
    static bool     L_scheduled         = FALSE;
    static bool     L_idle_traced       = FALSE;
    static bool     L_ffdc_collected    = FALSE;

    // Create/schedule GPE_start_pwr_meas_read (non-blocking)
    APSS_DBG("GPE_start_pwr_meas_read started");

    do
    {
        if (!async_request_is_idle(&G_meas_start_request.request))
        {
            if (!L_idle_traced)
            {
                INTR_TRAC_INFO("E>task_apss_start_pwr_meas: request is not idle.");
                L_idle_traced = TRUE;
            }
            break;
        }
        else
        {
            L_idle_traced = FALSE;
        }

        // Check if we need to try recovering the apss
        if(G_apss_recovery_requested)
        {
            // Do recovery then wait until next tick to do anything more.
            do_apss_recovery();
            break;
        }

        if (L_scheduled)
        {

            if ((ASYNC_REQUEST_STATE_COMPLETE != G_meas_start_request.request.completion_state) ||
                (0 != G_gpe_start_pwr_meas_read_args.error.error))
            {
                //error should only be non-zero in the case where the GPE timed out waiting for
                //the APSS master to complete the last operation.  Just keep retrying until
                //DCOM resets us due to not having valid power data.
                INTR_TRAC_ERR("task_apss_start_pwr_meas: request is not complete or failed with an error(rc:0x%08X, ffdc:0x%08X%08X). " \
                        "CompletionState:0x%X.",
                         G_gpe_start_pwr_meas_read_args.error.rc,
                         (uint32_t) (G_gpe_start_pwr_meas_read_args.error.ffdc >> 32),
                         (uint32_t) G_gpe_start_pwr_meas_read_args.error.ffdc,
                         G_meas_start_request.request.completion_state);

                // Collect FFDC and log error once.
                if (!L_ffdc_collected)
                {

                    errlHndl_t l_err = NULL;

                    /*
                     * @errortype
                     * @moduleid    PSS_MID_APSS_START_MEAS
                     * @reasoncode  APSS_GPE_FAILURE
                     * @userdata1   GPE returned rc code
                     * @userdata4   ERC_APSS_COMPLETE_FAILURE
                     * @devdesc     Failure getting power measurement data from APSS
                     */
                    l_err = createErrl(PSS_MID_APSS_START_MEAS,   // i_modId
                                       APSS_GPE_FAILURE,          // i_reasonCode
                                       ERC_APSS_COMPLETE_FAILURE,
                                       ERRL_SEV_INFORMATIONAL,
                                       NULL,
                                       DEFAULT_TRACE_SIZE,
                                       G_gpe_start_pwr_meas_read_args.error.rc,
                                       0);

                    addUsrDtlsToErrl(l_err,
                                     (uint8_t*)&G_meas_start_request.ffdc,
                                     sizeof(G_meas_start_request.ffdc),
                                     ERRL_STRUCT_VERSION_1,
                                     ERRL_USR_DTL_BINARY_DATA);

                    // Commit Error
                    commitErrl(&l_err);
                      // Set to true so that we don't log this error again.
                    L_ffdc_collected = TRUE;
                }
            }

        }

        // Clear these out prior to starting the GPE (GPE only sets them)
        G_gpe_start_pwr_meas_read_args.error.error = 0;
        G_gpe_start_pwr_meas_read_args.error.ffdc = 0;

#ifdef DEBUG_APSS_SEQ
        // DEBUG: Only allow start if the previous complete was scheduled
        static bool L_throttle = FALSE;
        if (G_sequence_complete == G_sequence_start)
        {
            L_throttle = FALSE;
            ++G_sequence_start;
            TRAC_INFO("task_apss_start_pwr_meas: scheduling start (seq %d)", G_sequence_start);
#endif

            // Submit the next request
            l_rc = gpe_request_schedule(&G_meas_start_request);
            if (0 != l_rc)
            {
                errlHndl_t l_err = NULL;

                INTR_TRAC_ERR("task_apss_start_pwr_meas: schedule failed w/rc=0x%08X (%d us)", l_rc,
                              (int) ((ssx_timebase_get())/(SSX_TIMEBASE_FREQUENCY_HZ/1000000)));

                /*
                 * @errortype
                 * @moduleid    PSS_MID_APSS_START_MEAS
                 * @reasoncode  SSX_GENERIC_FAILURE
                 * @userdata1   GPE shedule returned rc code
                 * @userdata2   0
                 * @userdata4   ERC_APSS_SCHEDULE_FAILURE
                 * @devdesc     task_apss_start_pwr_meas schedule failed
                 */
                l_err = createErrl(PSS_MID_APSS_START_MEAS,
                                   SSX_GENERIC_FAILURE,
                                   ERC_APSS_SCHEDULE_FAILURE,
                                   ERRL_SEV_PREDICTIVE,
                                   NULL,
                                   DEFAULT_TRACE_SIZE,
                                   l_rc,
                                   0);

                // Request reset since this should never happen.
                REQUEST_RESET(l_err);
                L_scheduled = FALSE;
                break;
            }

            L_scheduled = TRUE;
#ifdef DEBUG_APSS_SEQ
        }
        else
        {
            if (!L_throttle)
            {
                TRAC_INFO("task_apss_start_pwr_meas: start seq(%d) != complete seq(%d) so skipping start",
                          G_sequence_start, G_sequence_complete);
                L_throttle = TRUE;
            }
        }
#endif


    }while (0);


    APSS_DBG("GPE_start_pwr_meas_read finished w/rc=0x%08X", G_gpe_start_pwr_meas_read_args.error.rc);
    APSS_DBG_HEXDUMP(&G_gpe_start_pwr_meas_read_args, sizeof(G_gpe_start_pwr_meas_read_args), "G_gpe_start_pwr_meas_read_args");
    G_ApssPwrMeasCompleted = FALSE;  // Will complete when 3rd task is complete.
    G_gpe_apss_time_start = ssx_timebase_get();


} // end task_apss_start_pwr_meas()


// Note: The complete request must be global, since it must stick around until after the
//       GPE program has completed (in order to do the callback).

// Function Specification
//
// Name:  task_apss_continue_pwr_meas
//
// Description: Start GPE to collect 1st block of power measurement data and request
//              the 2nd block
//              If previous call had failed, commit the error and request reset
//
// Task Flags: RTL_FLAG_MSTR, RTL_FLAG_OBS, RTL_FLAG_ACTIVE, RTL_FLAG_APSS_PRES
//
// End Function Specification
void task_apss_continue_pwr_meas(struct task *i_self)
{
    int         l_rc                = 0;
    static bool L_scheduled         = FALSE;
    static bool L_idle_traced       = FALSE;
    static bool L_ffdc_collected    = FALSE;

    // Create/schedule GPE_apss_continue_pwr_meas_read (non-blocking)
    APSS_DBG("Calling task_apss_continue_pwr_meas.");

    do
    {
        if (!async_request_is_idle(&G_meas_cont_request.request))
        {
            if (!L_idle_traced)
            {
                INTR_TRAC_INFO("E>task_apss_continue_pwr_meas: request is not idle.");
                L_idle_traced = TRUE;
            }
            break;
        }
        else
        {
            L_idle_traced = FALSE;
        }

        //Don't run anything if apss recovery is in progress
        if(G_apss_recovery_requested)
        {
            break;
        }

        if (L_scheduled)
        {
            if ((ASYNC_REQUEST_STATE_COMPLETE != G_meas_cont_request.request.completion_state) ||
                (0 != G_gpe_continue_pwr_meas_read_args.error.error))
            {
                //error should only be non-zero in the case where the GPE timed out waiting for
                //the APSS master to complete the last operation.  Just keep retrying until
                //DCOM resets us due to not having valid power data.
                INTR_TRAC_ERR("task_apss_continue_pwr_meas: request is not complete or failed with an error(rc:0x%08X, ffdc:0x%08X%08X). " \
                        "CompletionState:0x%X.",
                         G_gpe_continue_pwr_meas_read_args.error.rc,
                         (uint32_t) (G_gpe_continue_pwr_meas_read_args.error.ffdc >> 32),
                         (uint32_t) G_gpe_continue_pwr_meas_read_args.error.ffdc,
                         G_meas_cont_request.request.completion_state);


                // Collect FFDC and log error once.
                if (!L_ffdc_collected)
                {
                    errlHndl_t l_err = NULL;

                    /*
                     * @errortype
                     * @moduleid    PSS_MID_APSS_CONT_MEAS
                     * @reasoncode  APSS_GPE_FAILURE
                     * @userdata1   GPE returned rc code
                     * @userdata2   0
                     * @userdata4   ERC_APSS_COMPLETE_FAILURE
                     * @devdesc     Failure getting power measurement data from APSS
                     */
                    l_err = createErrl(PSS_MID_APSS_CONT_MEAS,   // i_modId
                                       APSS_GPE_FAILURE,          // i_reasonCode
                                       ERC_APSS_COMPLETE_FAILURE,
                                       ERRL_SEV_INFORMATIONAL,
                                       NULL,
                                       DEFAULT_TRACE_SIZE,
                                       G_gpe_continue_pwr_meas_read_args.error.rc,
                                       0);

                    addUsrDtlsToErrl(l_err,
                                     (uint8_t*)&G_meas_cont_request.ffdc,
                                     sizeof(G_meas_cont_request.ffdc),
                                     ERRL_STRUCT_VERSION_1,
                                     ERRL_USR_DTL_BINARY_DATA);

                    // Commit Error
                    commitErrl(&l_err);
                      // Set to true so that we don't log this error again.
                    L_ffdc_collected = TRUE;
                }
            }
        }

        // Clear these out prior to starting the GPE (GPE only sets them)
        G_gpe_continue_pwr_meas_read_args.error.error = 0;
        G_gpe_continue_pwr_meas_read_args.error.ffdc = 0;

#ifdef DEBUG_APSS_SEQ
        // DEBUG: Only allow new continue if the start was scheduled
        static bool L_throttle = FALSE;
        if (G_sequence_start == (G_sequence_cont+1))
        {
            L_throttle = FALSE;
            ++G_sequence_cont;
            TRAC_INFO("task_apss_cont_pwr_meas: scheduling cont (seq %d)", G_sequence_cont);
#endif

            // Submit the next request
            l_rc = gpe_request_schedule(&G_meas_cont_request);
            if (0 != l_rc)
            {
                errlHndl_t l_err = NULL;

                INTR_TRAC_ERR("task_apss_cont_pwr_meas: schedule failed w/rc=0x%08X (%d us)", l_rc,
                              (int) ((ssx_timebase_get())/(SSX_TIMEBASE_FREQUENCY_HZ/1000000)));

                /*
                 * @errortype
                 * @moduleid    PSS_MID_APSS_CONT_MEAS
                 * @reasoncode  SSX_GENERIC_FAILURE
                 * @userdata1   GPE shedule returned rc code
                 * @userdata2   0
                 * @userdata4   ERC_APSS_SCHEDULE_FAILURE
                 * @devdesc     task_apss_continue_pwr_meas schedule failed
                 */
                l_err = createErrl(PSS_MID_APSS_CONT_MEAS,
                                   SSX_GENERIC_FAILURE,
                                   ERC_APSS_SCHEDULE_FAILURE,
                                   ERRL_SEV_PREDICTIVE,
                                   NULL,
                                   DEFAULT_TRACE_SIZE,
                                   l_rc,
                                   0);

                // Request reset since this should never happen.
                REQUEST_RESET(l_err);
                L_scheduled = FALSE;
                break;
            }

            L_scheduled = TRUE;

#ifdef DEBUG_APSS_SEQ
        }
        else
        {
            if (!L_throttle)
            {
                TRAC_INFO("task_apss_cont_pwr_meas: start seq(%d) != cont seq(%d+1) so skipping cont",
                          G_sequence_start, G_sequence_cont);
                L_throttle = TRUE;
            }
        }
#endif

    }while (0);

    APSS_DBG("task_apss_continue_pwr_meas: finished w/rc=0x%08X", G_gpe_continue_pwr_meas_read_args.error.rc);
    APSS_DBG_HEXDUMP(&G_gpe_continue_pwr_meas_read_args, sizeof(G_gpe_continue_pwr_meas_read_args), "G_gpe_continue_pwr_meas_read_args");

} // end task_apss_continue_pwr_meas

// Function Specification
//
// Name:  reformat_meas_data
//
// Description: Extract measurement from GPE programs into G_apss_pwr_meas structure
//              This function is called when the GPE completes the final measurement
//              collection for this loop.
//
// End Function Specification
#define APSS_ADC_SEQ_MASK  0xf000f000f000f000ull
#define APSS_ADC_SEQ_CHECK 0x0000100020003000ull
void reformat_meas_data()
{
    // NO TRACING ALLOWED IN CRITICAL INTERRUPT (any IPC callback functions)
    do
    {
        // Make sure complete was successful
        if (G_gpe_complete_pwr_meas_read_args.error.error)
        {
            break;
        }

        // Check that the first 4 sequence nibbles are 0, 1, 2, 3 in the ADC data
        if (((G_gpe_continue_pwr_meas_read_args.meas_data[0] & APSS_ADC_SEQ_MASK) != APSS_ADC_SEQ_CHECK) ||
             !(G_gpe_continue_pwr_meas_read_args.meas_data[0] & ~APSS_ADC_SEQ_MASK))
        {
            // Recovery will begin on the next tick
            G_apss_recovery_requested = TRUE;
            break;
        }

        // Decrement up/down fail counter for backup on success.
        if(G_backup_fail_count)
        {
            G_backup_fail_count--;
        }

        // Don't do the copy unless this is the master OCC
        if(G_occ_role == OCC_MASTER)
        {
            // Fail every 16 seconds
            // Merge continue/complete data into a single buffer
            const uint16_t l_continue_meas_length = sizeof(G_gpe_continue_pwr_meas_read_args.meas_data);
            const uint16_t l_complete_meas_length = sizeof(G_gpe_complete_pwr_meas_read_args.meas_data);
            uint8_t l_buffer[l_continue_meas_length+l_complete_meas_length];
            memcpy(&l_buffer[                     0], G_gpe_continue_pwr_meas_read_args.meas_data, l_continue_meas_length);
            memcpy(&l_buffer[l_continue_meas_length], G_gpe_complete_pwr_meas_read_args.meas_data, l_complete_meas_length);

            // Copy measurements into correct struction locations (based on composite config)
            uint16_t l_index = 0;
            memcpy(G_apss_pwr_meas.adc, &l_buffer[l_index], (G_apss_mode_config.numAdcChannelsToRead * 2));
            l_index += (G_apss_mode_config.numAdcChannelsToRead * 2);
            memcpy(G_apss_pwr_meas.gpio, &l_buffer[l_index], (G_apss_mode_config.numGpioPortsToRead * 2));
            // TOD is always located at same offset
            memcpy(&G_apss_pwr_meas.tod, &l_buffer[l_continue_meas_length+l_complete_meas_length-8], 8);
        }

        // Mark apss pwr meas completed and valid
        G_ApssPwrMeasCompleted = TRUE;
#ifdef DEBUG_APSS_SEQ
        // Save the complete sequence number from the GPE
        G_savedCompleteSeq = G_gpe_complete_pwr_meas_read_args.error.ffdc;
#endif
        G_gpe_apss_time_end = ssx_timebase_get();
  }while(0);
} // end reformat_meas_data()


// Note: The complete request must be global, since it must stick around until after the
//       GPE program has completed (in order to do the callback).

// Function Specification
//
// Name:  task_apss_complete_pwr_meas
//
// Description: Start GPE to collect 2nd block of power measurement data and TOD.
//              If previous call had failed, commit the error and request reset
//
// Task Flags: RTL_FLAG_MSTR, RTL_FLAG_OBS, RTL_FLAG_ACTIVE, RTL_FLAG_APSS_PRES
//
// End Function Specification
void task_apss_complete_pwr_meas(struct task *i_self)
{
    int         l_rc                = 0;
    static bool L_scheduled         = FALSE;
    static bool L_idle_traced       = FALSE;
    static bool L_ffdc_collected    = FALSE;

    // Create/schedule GPE_apss_complete_pwr_meas_read (non-blocking)
    APSS_DBG("Calling task_apss_complete_pwr_meas.\n");

    do
    {
        if (!async_request_is_idle(&G_meas_complete_request.request))
        {
            if (!L_idle_traced)
            {
                INTR_TRAC_INFO("E>task_apss_complete_pwr_meas: request is not idle.");
                L_idle_traced = TRUE;
            }
            break;
        }
        else
        {
            L_idle_traced = FALSE;
        }
        if(G_apss_recovery_requested)
        {
            // Allow apss measurement to proceed on next tick
            G_apss_recovery_requested = FALSE;
            break;
        }


        if (L_scheduled)
        {
            if ((ASYNC_REQUEST_STATE_COMPLETE != G_meas_complete_request.request.completion_state) ||
                (0 != G_gpe_complete_pwr_meas_read_args.error.error))
            {
                // Error should only be non-zero in the case where the GPE timed out waiting for
                // the APSS master to complete the last operation. Just keep retrying until
                // DCOM resets us due to not having valid power data.
                INTR_TRAC_ERR("task_apss_complete_pwr_meas: request is not complete or failed with an error(rc:0x%08X, ffdc:0x%08X%08X). " \
                        "CompletionState:0x%X.",
                         G_gpe_complete_pwr_meas_read_args.error.rc,
                         (uint32_t) (G_gpe_complete_pwr_meas_read_args.error.ffdc >> 32),
                         (uint32_t) G_gpe_complete_pwr_meas_read_args.error.ffdc,
                         G_meas_complete_request.request.completion_state);

                // Collect FFDC and log error once.
                if (!L_ffdc_collected)
                {
                    errlHndl_t l_err = NULL;

                    /*
                     * @errortype
                     * @moduleid    PSS_MID_APSS_COMPLETE_MEAS
                     * @reasoncode  APSS_GPE_FAILURE
                     * @userdata1   GPE returned rc code
                     * @userdata2   0
                     * @userdata4   ERC_APSS_COMPLETE_FAILURE
                     * @devdesc     Failure getting power measurement data from APSS
                     */
                    l_err = createErrl(PSS_MID_APSS_COMPLETE_MEAS,   // i_modId
                                       APSS_GPE_FAILURE,          // i_reasonCode
                                       ERC_APSS_COMPLETE_FAILURE,
                                       ERRL_SEV_INFORMATIONAL,
                                       NULL,
                                       DEFAULT_TRACE_SIZE,
                                       G_gpe_complete_pwr_meas_read_args.error.rc,
                                       0);

                    addUsrDtlsToErrl(l_err,
                                     (uint8_t*)&G_meas_complete_request.ffdc,
                                     sizeof(G_meas_complete_request.ffdc),
                                     ERRL_STRUCT_VERSION_1,
                                     ERRL_USR_DTL_BINARY_DATA);

                    // Commit Error
                    commitErrl(&l_err);
                    // Set to true so that we don't log this error again.
                    L_ffdc_collected = TRUE;
                }
            }
        }

#ifdef DEBUG_APSS_SEQ
        // DEBUG: Only allow new complete if the continue was scheduled
        static bool L_throttle = FALSE;
        if (G_sequence_cont == (G_sequence_complete+1))
        {
            L_throttle = FALSE;
            ++G_sequence_complete;

            // Clear these out prior to starting the GPE (GPE only sets them)
            G_gpe_complete_pwr_meas_read_args.error.error = 0;
            // Set the FFDC field to the complete sequence number
            G_gpe_complete_pwr_meas_read_args.error.ffdc = G_sequence_complete;
            TRAC_INFO("task_apss_complete_pwr_meas: scheduling complete (seq %d, prior seq was %d)",
                      G_sequence_complete, G_savedCompleteSeq);
#else
            // Clear these out prior to starting the GPE (GPE only sets them)
            G_gpe_complete_pwr_meas_read_args.error.error = 0;
            G_gpe_complete_pwr_meas_read_args.error.ffdc = 0;
#endif

            // Submit the next request

            l_rc = gpe_request_schedule(&G_meas_complete_request);
            if (0 != l_rc)
            {

                errlHndl_t l_err = NULL;

                INTR_TRAC_ERR("task_apss_complete_pwr_meas: schedule failed w/rc=0x%08X (%d us)", l_rc,
                              (int) ((ssx_timebase_get())/(SSX_TIMEBASE_FREQUENCY_HZ/1000000)));
                /*
                 * @errortype
                 * @moduleid    PSS_MID_APSS_COMPLETE_MEAS
                 * @reasoncode  SSX_GENERIC_FAILURE
                 * @userdata1   GPE shedule returned rc code
                 * @userdata2   0
                 * @userdata4   ERC_APSS_SCHEDULE_FAILURE
                 * @devdesc     task_apss_complete_pwr_meas schedule failed
                 */
                l_err = createErrl(PSS_MID_APSS_COMPLETE_MEAS,
                                   SSX_GENERIC_FAILURE,
                                   ERC_APSS_SCHEDULE_FAILURE,
                                   ERRL_SEV_PREDICTIVE,
                                   NULL,
                                   DEFAULT_TRACE_SIZE,
                                   l_rc,
                                   0);

                // Request reset since this should never happen.
                REQUEST_RESET(l_err);
                L_scheduled = FALSE;
                break;
            }
            L_scheduled = TRUE;
#ifdef DEBUG_APSS_SEQ
        }
        else
        {
            if (!L_throttle)
            {
                TRAC_INFO("task_apss_complete_pwr_meas: cont seq(%d) != complete seq(%d+1) so skipping complete",
                          G_sequence_cont, G_sequence_complete);
                L_throttle = TRUE;
            }
        }
#endif


    }while (0);

    APSS_DBG("task_apss_complete_pwr_meas: finished w/rc=0x%08X\n", G_gpe_complete_pwr_meas_read_args.error.rc);
    APSS_DBG_HEXDUMP(&G_gpe_complete_pwr_meas_read_args, sizeof(G_gpe_complete_pwr_meas_read_args), "G_gpe_complete_pwr_meas_read_args");


} // end task_apss_complete_pwr_meas

bool apss_gpio_get(uint8_t i_pin_number, uint8_t *o_pin_value)
{
    bool l_valid = FALSE;

    if( (i_pin_number != SYSCFG_INVALID_PIN) && (o_pin_value != NULL) )
    {
        // Check if G_dcom_slv_inbox_rx is valid.
        // The default value is all 0, so check if it's no-zero
        bool l_dcom_data_valid = FALSE;
// TEMP -- NO DCOM IN PHASE1
/*
        int i=0;
        for(;i<sizeof(G_dcom_slv_inbox_rx);i++)
        {
            if( ((char*)&G_dcom_slv_inbox_rx)[i] != 0 )
            {
              l_dcom_data_valid = TRUE;
              break;
            }
        }
*/
        if( l_dcom_data_valid == TRUE)
        {
            uint8_t l_gpio_port = i_pin_number/NUM_OF_APSS_PINS_PER_GPIO_PORT;
            uint8_t l_gpio_mask = 0x1 << i_pin_number % NUM_OF_APSS_PINS_PER_GPIO_PORT;
            l_valid = TRUE;
            if( G_dcom_slv_inbox_rx.gpio[l_gpio_port] & l_gpio_mask )
            {
                *o_pin_value = 1;
            }
            else
            {
                *o_pin_value = 0;
            }
        }
    }
    return l_valid;
}

// Function Specification
//
// Name:  initialize_apss
//
// Description: Configure the APSS GPIOs, set the mode, and create the
//              GPE requests for the APSS data collection tasks.
//
// End Function Specification
errlHndl_t initialize_apss(void)
{
    errlHndl_t l_err = NULL;
    GpeRequest l_request;   //Used once here to initialize apss.
    uint8_t    l_retryCount = 0;
    // Initialize APSS

    while ( l_retryCount < APSS_MAX_NUM_INIT_RETRIES )
    {
        // Setup the GPIO init structure to pass to the PPE program
        G_gpe_apss_initialize_gpio_args.error.error = 0;
        G_gpe_apss_initialize_gpio_args.error.ffdc = 0;
        G_gpe_apss_initialize_gpio_args.config0.direction
                                    = G_gpio_config[0].direction;
        G_gpe_apss_initialize_gpio_args.config0.drive
                                = G_gpio_config[0].drive;
        G_gpe_apss_initialize_gpio_args.config0.interrupt
                                    = G_gpio_config[0].interrupt;
        G_gpe_apss_initialize_gpio_args.config1.direction
                                    = G_gpio_config[1].direction;
        G_gpe_apss_initialize_gpio_args.config1.drive
                                    = G_gpio_config[1].drive;
        G_gpe_apss_initialize_gpio_args.config1.interrupt
                                    = G_gpio_config[1].interrupt;

        // Create/schedule IPC_ST_APSS_INIT_GPIO_FUNCID and wait for it to complete (BLOCKING)
        TRAC_INFO("initialize_apss: Creating request for GPE_apss_initialize_gpio");
        gpe_request_create(&l_request,                                // request
                           &G_async_gpe_queue0,                       // queue
                           IPC_ST_APSS_INIT_GPIO_FUNCID,              // Function ID
                           &G_gpe_apss_initialize_gpio_args,          // GPE argument_ptr
                           SSX_SECONDS(5),                            // timeout
                           NULL,                                      // callback
                           NULL,                                      // callback arg
                           ASYNC_REQUEST_BLOCKING);                   // options

        // Schedule the request to be executed
        TRAC_INFO("initialize_apss: Scheduling request for IPC_ST_APSS_INIT_GPIO_FUNCID");
        gpe_request_schedule(&l_request);

        // Check for a timeout only; will create the error below.
        if(ASYNC_REQUEST_STATE_TIMED_OUT == l_request.request.completion_state)
        {
            // For whatever reason, we hit a timeout.  It could be either
            // that the HW did not work, or the request didn't ever make
            // it to the front of the queue.
            // Let's log an error, and include the FFDC data if it was
            // generated.
            TRAC_ERR("initialize_apss: Timeout communicating with PPE for APSS Init.");
        }


        TRAC_INFO("initialize_apss: GPE_apss_initialize_gpio completed w/rc=0x%08x",
                  l_request.request.completion_state);

        //TODO: The ipc command will return "SUCCESS" even if an internal PPE failure
        //      occurs.  Make sure that checking for rc as seen below, is good enough.

        // Only continue if initializaton completed without any errors.
        if ((ASYNC_REQUEST_STATE_COMPLETE == l_request.request.completion_state) &&
            (G_gpe_apss_initialize_gpio_args.error.rc == ERRL_RC_SUCCESS))
        {
            // Setup the mode structure to pass to the GPE program
            G_gpe_apss_set_mode_args.error.error = 0;
            G_gpe_apss_set_mode_args.error.ffdc = 0;
            G_gpe_apss_set_mode_args.config.mode
                                    = G_apss_mode_config.mode;
            G_gpe_apss_set_mode_args.config.numAdcChannelsToRead
                                    = G_apss_mode_config.numAdcChannelsToRead;
            G_gpe_apss_set_mode_args.config.numGpioPortsToRead
                                    = G_apss_mode_config.numGpioPortsToRead;

            // Create/schedule GPE_apss_set_mode and wait for it to complete (BLOCKING)
            TRAC_INFO("initialize_apss: Creating request for GPE_apss_set_mode");
            gpe_request_create(&l_request,                              // request
                               &G_async_gpe_queue0,                     // queue
                               IPC_ST_APSS_INIT_MODE_FUNCID,            // Function ID
                               &G_gpe_apss_set_mode_args,               // GPE argument_ptr
                               SSX_SECONDS(5),                          // timeout
                               NULL,                                    // callback
                               NULL,                                    // callback arg
                               ASYNC_REQUEST_BLOCKING);                 // options
            //Schedule set_mode
            gpe_request_schedule(&l_request);

            // Check for a timeout, will create the error log later
            if(ASYNC_REQUEST_STATE_TIMED_OUT == l_request.request.completion_state)
            {
                // For whatever reason, we hit a timeout.  It could be either
                // that the HW did not work, or the request didn't ever make
                // it to the front of the queue.
                // Let's log an error, and include the FFDC data if it was
                // generated.
                TRAC_ERR("initialize_apss: Timeout communicating with PPE for APSS Init");
            }

            TRAC_INFO("initialize_apss: GPE_apss_set_mode completed w/rc=0x%08x",
                          l_request.request.completion_state);

            //Continue only if mode set was successful.
            if ((ASYNC_REQUEST_STATE_COMPLETE != l_request.request.completion_state) ||
                (G_gpe_apss_set_mode_args.error.rc != ERRL_RC_SUCCESS))
            {
                /*
                 * @errortype
                 * @moduleid    PSS_MID_APSS_INIT
                 * @reasoncode  INTERNAL_FAILURE
                 * @userdata1   GPE returned rc code
                 * @userdata2   GPE returned abort code
                 * @userdata4   ERC_PSS_COMPOSITE_MODE_FAIL
                 * @devdesc     Failure from GPE for setting composite mode on
                 *              APSS
                 */
                l_err = createErrl(PSS_MID_APSS_INIT,                   // i_modId,
                                   INTERNAL_FAILURE,                    // i_reasonCode,
                                   ERC_PSS_COMPOSITE_MODE_FAIL,         // extended reason code
                                   ERRL_SEV_UNRECOVERABLE,              // i_severity
                                   NULL,                                // i_trace,
                                   0x0000,                              // i_traceSz,
                                   l_request.request.completion_state,  // i_userData1,
                                   l_request.request.abort_state);      // i_userData2
                addUsrDtlsToErrl(l_err,
                                 (uint8_t*)&G_gpe_apss_set_mode_args,
                                 sizeof(G_gpe_apss_set_mode_args),
                                 ERRL_STRUCT_VERSION_1,
                                 ERRL_USR_DTL_TRACE_DATA);

                // Returning an error log will cause us to go to safe
                // state so we can report error to FSP
            }
        }
        else
        {
            /*
             * @errortype
             * @moduleid    PSS_MID_APSS_INIT
             * @reasoncode  INTERNAL_FAILURE
             * @userdata1   GPE returned rc code
             * @userdata2   GPE returned abort code
             * @userdata4   ERC_PSS_GPIO_INIT_FAIL
             * @devdesc     Failure from GPE for gpio initialization on APSS
             */
            l_err = createErrl(PSS_MID_APSS_INIT,                   // i_modId,
                               INTERNAL_FAILURE,                    // i_reasonCode,
                               ERC_PSS_GPIO_INIT_FAIL,              // extended reason code
                               ERRL_SEV_UNRECOVERABLE,              // i_severity
                               NULL,                                // tracDesc_t i_trace,
                               0x0000,                              // i_traceSz,
                               l_request.request.completion_state,  // i_userData1,
                               l_request.request.abort_state);      // i_userData2

            addUsrDtlsToErrl(l_err,
                             (uint8_t*)&G_gpe_apss_initialize_gpio_args,
                             sizeof(G_gpe_apss_initialize_gpio_args),
                             ERRL_STRUCT_VERSION_1,
                             ERRL_USR_DTL_TRACE_DATA);

            // Returning an error log will cause us to go to safe
            // state so we can report error to FSP
        }

        if ( NULL == l_err )
        {
            TRAC_INFO("initialize_apss: Creating request G_meas_start_request.");
            //Create the request for measure start. Scheduling will happen in apss.c
            gpe_request_create(&G_meas_start_request,
                               &G_async_gpe_queue0,                          // queue
                               IPC_ST_APSS_START_PWR_MEAS_READ_FUNCID,   // entry_point
                               &G_gpe_start_pwr_meas_read_args,              // entry_point arg
                               SSX_WAIT_FOREVER,                             // no timeout
                               NULL,                                         // callback
                               NULL,                                         // callback arg
                               ASYNC_CALLBACK_IMMEDIATE);                    // options

            TRAC_INFO("initialize_apss: Creating request G_meas_cont_request.");
            //Create the request for measure continue. Scheduling will happen in apss.c
            gpe_request_create(&G_meas_cont_request,
                               &G_async_gpe_queue0,                          // request
                               IPC_ST_APSS_CONTINUE_PWR_MEAS_READ_FUNCID,    // entry_point
                               &G_gpe_continue_pwr_meas_read_args,           // entry_point arg
                               SSX_WAIT_FOREVER,                             // no timeout
                               NULL,                                         // callback
                               NULL,                                         // callback arg
                               ASYNC_CALLBACK_IMMEDIATE);                    // options

            TRAC_INFO("initialize_apss: Creating request G_meas_complete_request.");
            //Create the request for measure complete. Scheduling will happen in apss.c
            gpe_request_create(&G_meas_complete_request,
                               &G_async_gpe_queue0,                          // queue
                               IPC_ST_APSS_COMPLETE_PWR_MEAS_READ_FUNCID,    // entry_point
                               &G_gpe_complete_pwr_meas_read_args,           // entry_point arg
                               SSX_WAIT_FOREVER,                             // no timeout
                               (AsyncRequestCallback)reformat_meas_data,     // callback
                               NULL,                                         // callback arg
                               ASYNC_CALLBACK_IMMEDIATE);                    // options

            // Successfully initialized APSS, no need to go through again. Let's leave.
            break;
        }
        else
        {
            l_retryCount++;
            if ( l_retryCount < APSS_MAX_NUM_INIT_RETRIES )
            {
                TRAC_ERR("initialize_apss: APSS init failed! (retrying)");
                setErrlSevToInfo(l_err);
                commitErrl(&l_err); // commit & delete
            }
            else
            {
                TRAC_ERR("initialize_apss: APSS init failed again!");
            }
        }
    }

    return l_err;
}
