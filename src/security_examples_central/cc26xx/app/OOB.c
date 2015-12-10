/*******************************************************************************
  Filename:       just_works.c
  Revised:        $Date$
  Revision:       $Revision$

  Description:    This file contains the basic sample central application to implement
                  legacy just works pairing for use with the CC2650 Bluetooth 
                  Low Energy Protocol Stack.

  Copyright 2013 - 2015 Texas Instruments Incorporated. All rights reserved.

  IMPORTANT: Your use of this Software is limited to those specific rights
  granted under the terms of a software license agreement between the user
  who downloaded the software, his/her employer (which must be your employer)
  and Texas Instruments Incorporated (the "License").  You may not use this
  Software unless you agree to abide by the terms of the License. The License
  limits your use, and you acknowledge, that the Software may not be modified,
  copied or distributed unless embedded on a Texas Instruments microcontroller
  or used solely and exclusively in conjunction with a Texas Instruments radio
  frequency transceiver, which is integrated into your product.  Other than for
  the foregoing purpose, you may not use, reproduce, copy, prepare derivative
  works of, modify, distribute, perform, display or sell this Software and/or
  its documentation for any purpose.

  YOU FURTHER ACKNOWLEDGE AND AGREE THAT THE SOFTWARE AND DOCUMENTATION ARE
  PROVIDED �AS IS� WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED,
  INCLUDING WITHOUT LIMITATION, ANY WARRANTY OF MERCHANTABILITY, TITLE,
  NON-INFRINGEMENT AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL
  TEXAS INSTRUMENTS OR ITS LICENSORS BE LIABLE OR OBLIGATED UNDER CONTRACT,
  NEGLIGENCE, STRICT LIABILITY, CONTRIBUTION, BREACH OF WARRANTY, OR OTHER
  LEGAL EQUITABLE THEORY ANY DIRECT OR INDIRECT DAMAGES OR EXPENSES
  INCLUDING BUT NOT LIMITED TO ANY INCIDENTAL, SPECIAL, INDIRECT, PUNITIVE
  OR CONSEQUENTIAL DAMAGES, LOST PROFITS OR LOST DATA, COST OF PROCUREMENT
  OF SUBSTITUTE GOODS, TECHNOLOGY, SERVICES, OR ANY CLAIMS BY THIRD PARTIES
  (INCLUDING BUT NOT LIMITED TO ANY DEFENSE THEREOF), OR OTHER SIMILAR COSTS.

  Should you have any questions regarding your right to use this Software,
  contact Texas Instruments Incorporated at www.TI.com.
*******************************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include <string.h>

#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/knl/Queue.h>

#include "bcomdef.h"

#include "gatt.h"
#include "gapgattserver.h"
#include "gattservapp.h"
#include "central.h"
#include "gapbondmgr.h"
#include "hci.h"

#include "osal_snv.h"
#include "icall_apimsg.h"

#include "util.h"
#include "board_key.h"
#include "board_lcd.h"
#include "board.h"

#include "security_examples_central.h"

#include "ble_user_config.h"

#include <ti/drivers/lcd/LCDDogm1286.h>

/*********************************************************************
 * MACROS
 */

/*********************************************************************
 * CONSTANTS
 */

// Simple BLE Central Task Events
#define SEC_PAIRING_STATE_EVT                 0x0001
#define SEC_KEY_CHANGE_EVT                    0x0002
#define SEC_STATE_CHANGE_EVT                  0x0004
#define SEC_PASSCODE_NEEDED_EVT               0x0008
#define SEC_SM_ECC_KEYS_EVT                   0x0010

// Maximum number of scan responses
#define DEFAULT_MAX_SCAN_RES                  8

// Scan duration in ms
#define DEFAULT_SCAN_DURATION                 4000

// Discovery mode (limited, general, all)
#define DEFAULT_DISCOVERY_MODE                DEVDISC_MODE_ALL

// TRUE to use active scan
#define DEFAULT_DISCOVERY_ACTIVE_SCAN         TRUE

// TRUE to use white list during discovery
#define DEFAULT_DISCOVERY_WHITE_LIST          FALSE

// TRUE to use high scan duty cycle when creating link
#define DEFAULT_LINK_HIGH_DUTY_CYCLE          FALSE

// TRUE to use white list when creating link
#define DEFAULT_LINK_WHITE_LIST               FALSE

// Whether to enable automatic parameter update request when a connection is 
// formed
#define DEFAULT_ENABLE_UPDATE_REQUEST         FALSE

// Minimum connection interval (units of 1.25ms) if automatic parameter update
  // request is enabled
#define DEFAULT_UPDATE_MIN_CONN_INTERVAL      400

// Maximum connection interval (units of 1.25ms) if automatic parameter update
// request is enabled
#define DEFAULT_UPDATE_MAX_CONN_INTERVAL      800

// Slave latency to use if automatic parameter update request is enabled
#define DEFAULT_UPDATE_SLAVE_LATENCY          0

// Supervision timeout value (units of 10ms) if automatic parameter update 
// request is enabled
#define DEFAULT_UPDATE_CONN_TIMEOUT           600

// Length of bd addr as a string
#define B_ADDR_STR_LEN                        15

// Task configuration
#define SEC_TASK_PRIORITY                     1

#ifndef SEC_TASK_STACK_SIZE
#define SEC_TASK_STACK_SIZE                   864
#endif

// Application states
enum
{
  BLE_STATE_IDLE,
  BLE_STATE_CONNECTING,
  BLE_STATE_CONNECTED,
  BLE_STATE_DISCONNECTING
};

/*********************************************************************
 * TYPEDEFS
 */

// App event passed from profiles.
typedef struct 
{
  appEvtHdr_t hdr; // event header
  uint8_t *pData;  // event data 
} sbcEvt_t;

// Passcode/Numeric Comparison display data structure
typedef struct
{
  uint8_t uiOutputs;
  uint32_t numComparison;
} pairDisplay_t;

/*********************************************************************
 * GLOBAL VARIABLES
 */

/*********************************************************************
 * EXTERNAL VARIABLES
 */

/*********************************************************************
 * LOCAL VARIABLES
 */

// Entity ID globally used to check for source and/or destination of messages
static ICall_EntityID selfEntity;

// Semaphore globally used to post events to the application thread
static ICall_Semaphore sem;

// Task pending events
static uint16_t events = 0;

// Queue object used for app messages
static Queue_Struct appMsg;
static Queue_Handle appMsgQueue;

// Task configuration
Task_Struct sbcTask;
Char sbcTaskStack[SEC_TASK_STACK_SIZE];

// GAP GATT Attributes
static const uint8_t attDeviceName[GAP_DEVICE_NAME_LEN] = "Security Ex Centr";

// Number of scan results and scan result index
static uint8_t scanRes;
static uint8_t scanIdx;

// Scan result list
static gapDevRec_t devList[DEFAULT_MAX_SCAN_RES];

// Scanning state
static bool scanningStarted = FALSE;

// Connection handle of current connection 
static uint16_t connHandle = GAP_CONNHANDLE_INIT;

// Application state
static uint8_t state = BLE_STATE_IDLE;

// Used for triggering keypresses to pass or fail a numeric comparison during
// pairing.
static uint8 judgeNumericComparison = FALSE;

// LOCAL OOB DATA
uint8 oobLocal[16] = { 0xA3, 0xDE, 0xBB, 0x31, 0xE6, 0x42, 0x4E, 0x2F,
                  0x39, 0x7F, 0xF2, 0xD2, 0xC4, 0x89, 0xC6, 0xA7 };

// REMOTE DEVICE'S OOB DATA
gapBondOobSC_t oobData =
{
  .addr = {0x0C, 0xA3, 0x08, 0x0B, 0xC9, 0x68},
  .confirm = {0x0e, 0x3f, 0x3a, 0x75, 0x63, 0x9a, 0xac, 0x9a, 0x56, 0x4f, 0x1d, 
              0x65, 0x95, 0x39, 0xe1, 0x56},
  .oob = {0xAD, 0xAE, 0x2A, 0x71, 0xEC, 0xB2, 0x4F, 0xFF, 0x33, 0x73, 0x72, 
          0xD1, 0x84, 0x81, 0xCB, 0xD7}
};

//LOCAL KEYS OOB
gapBondEccKeys_t eccKeys =
{
  .privateKey = {0x15, 0x99, 0x87, 0x83, 0xc7, 0x84, 0x05, 0x92, 0x35, 0x9e, 
                 0x54, 0x2c, 0x77, 0x61, 0xb5, 0xd6, 0x0a, 0x80, 0x67, 0x5d, 
                 0xe8, 0x62, 0xd5, 0xe0, 0xeb, 0xce, 0x76, 0xc7, 0x7b, 0xc2, 
                 0xfb, 0x43},
  .publicKeyX = {0xca, 0x42, 0x2f, 0xc3, 0x4c, 0xe5, 0x03, 0x9a, 0x94, 0x06, 
                 0x26, 0x6d, 0xd8, 0x22, 0x51, 0x30, 0xe6, 0x04, 0xd7, 0x4b, 
                 0x9b, 0xc3, 0x1e, 0x45, 0xde, 0x5e, 0x3d, 0x5d, 0xb0, 0x1a, 
                 0xe4, 0xaa},
  .publicKeyY = {0x03, 0xc8, 0xbf, 0xd1, 0x00, 0xc6, 0x10, 0xb5, 0xec, 0x33, 
                 0x0c, 0x39, 0x8d, 0xa9, 0xcf, 0x87, 0x36, 0x27, 0xe9, 0x02, 
                 0x27, 0x28, 0xad, 0xc1, 0xb0, 0x40, 0xae, 0x97, 0x47, 0x66, 
                 0x8f, 0xb4}
};

/*********************************************************************
 * LOCAL FUNCTIONS
 */
static void security_examples_central_init(void);
static void security_examples_central_taskFxn(UArg a0, UArg a1);

static void security_examples_central_handleKeys(uint8_t shift, uint8_t keys);
static void security_examples_central_processStackMsg(ICall_Hdr *pMsg);
static void security_examples_central_processAppMsg(sbcEvt_t *pMsg);
static void security_examples_central_processRoleEvent(gapCentralRoleEvent_t *pEvent);
static void security_examples_central_processPasscode(uint16_t connectionHandle,
                                             uint8_t uiOutputs, 
                                             uint32_t numComparison);

static void security_examples_central_addDeviceInfo(uint8_t *pAddr, uint8_t addrType);
static void security_examples_central_processPairState(uint8_t state, uint8_t status);

static uint8_t security_examples_central_eventCB(gapCentralRoleEvent_t *pEvent);
static void security_examples_central_passcodeCB(uint8_t *deviceAddr, uint16_t connHandle,
                                        uint8_t uiInputs, uint8_t uiOutputs,
                                        uint32_t numComparison);
static void security_examples_central_pairStateCB(uint16_t connHandle, uint8_t state, 
                                         uint8_t status);

void security_examples_central_keyChangeHandler(uint8 keys);

static uint8_t security_examples_central_enqueueMsg(uint8_t event, uint8_t status, 
                                           uint8_t *pData);

/*********************************************************************
 * PROFILE CALLBACKS
 */

// GAP Role Callbacks
static gapCentralRoleCB_t security_examples_central_roleCB =
{
  security_examples_central_eventCB     // Event callback
};

// Bond Manager Callbacks
static gapBondCBs_t security_examples_central_bondCB =
{
  (pfnPasscodeCB_t)security_examples_central_passcodeCB, // Passcode callback
  security_examples_central_pairStateCB                  // Pairing state callback
};

/*********************************************************************
 * PUBLIC FUNCTIONS
 */

/*********************************************************************
 * @fn      SimpleBLEPeripheral_createTask
 *
 * @brief   Task creation function for the Simple BLE Peripheral.
 *
 * @param   none
 *
 * @return  none
 */
void security_examples_central_createTask(void)
{
  Task_Params taskParams;
    
  // Configure task
  Task_Params_init(&taskParams);
  taskParams.stack = sbcTaskStack;
  taskParams.stackSize = SEC_TASK_STACK_SIZE;
  taskParams.priority = SEC_TASK_PRIORITY;
  
  Task_construct(&sbcTask, security_examples_central_taskFxn, &taskParams, NULL);
}

/*********************************************************************
 * @fn      security_examples_central_Init
 *
 * @brief   Initialization function for the Simple BLE Central App Task.
 *          This is called during initialization and should contain
 *          any application specific initialization (ie. hardware
 *          initialization/setup, table initialization, power up
 *          notification).
 *
 * @param   none
 *
 * @return  none
 */
static void security_examples_central_init(void)
{
  // ******************************************************************
  // N0 STACK API CALLS CAN OCCUR BEFORE THIS CALL TO ICall_registerApp
  // ******************************************************************
  // Register the current thread as an ICall dispatcher application
  // so that the application can send and receive messages.
  ICall_registerApp(&selfEntity, &sem);

  uint8 bdAddr[] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  HCI_EXT_SetBDADDRCmd(bdAddr);
  
  // Create an RTOS queue for message from profile to be sent to app.
  appMsgQueue = Util_constructQueue(&appMsg);
     
  Board_initKeys(security_examples_central_keyChangeHandler);
  
  Board_openLCD();
  
  // Setup Central Profile
  {
    uint8_t scanRes = DEFAULT_MAX_SCAN_RES;
    GAPCentralRole_SetParameter(GAPCENTRALROLE_MAX_SCAN_RES, sizeof(uint8_t), 
                                &scanRes);
  }
  
  // Setup GAP
  GAP_SetParamValue(TGAP_GEN_DISC_SCAN, DEFAULT_SCAN_DURATION);
  GAP_SetParamValue(TGAP_LIM_DISC_SCAN, DEFAULT_SCAN_DURATION);
  GGS_SetParameter(GGS_DEVICE_NAME_ATT, GAP_DEVICE_NAME_LEN, 
                   (void *)attDeviceName);

  // Setup the GAP Bond Manager
  {
    uint8_t pairMode = GAPBOND_PAIRING_MODE_INITIATE;
    uint8_t mitm = TRUE;
    uint8_t ioCap = GAPBOND_IO_CAP_KEYBOARD_DISPLAY;
    uint8_t bonding = FALSE;
    uint8_t scMode = GAPBOND_SECURE_CONNECTION_ONLY;
    uint8_t oobEnabled = TRUE;
    
    GAPBondMgr_SetParameter(GAPBOND_PAIRING_MODE, sizeof(uint8_t), &pairMode);
    GAPBondMgr_SetParameter(GAPBOND_MITM_PROTECTION, sizeof(uint8_t), &mitm);
    GAPBondMgr_SetParameter(GAPBOND_IO_CAPABILITIES, sizeof(uint8_t), &ioCap);
    GAPBondMgr_SetParameter(GAPBOND_BONDING_ENABLED, sizeof(uint8_t), &bonding);
    GAPBondMgr_SetParameter(GAPBOND_SECURE_CONNECTION, sizeof(uint8_t), &scMode);
    GAPBondMgr_SetParameter(GAPBOND_LOCAL_OOB_SC_ENABLED, sizeof(uint8_t), &oobEnabled );
    GAPBondMgr_SetParameter(GAPBOND_LOCAL_OOB_SC_DATA, sizeof(uint8_t) * 16, oobLocal);    
    GAPBondMgr_SetParameter(GAPBOND_ECC_KEYS, sizeof(gapBondEccKeys_t), &eccKeys);    
  }  

  // Initialize GATT Client
  VOID GATT_InitClient();

  // Initialize GATT attributes
  GGS_AddService(GATT_ALL_SERVICES);         // GAP
  GATTServApp_AddService(GATT_ALL_SERVICES); // GATT attributes
  
  // Start the Device
  VOID GAPCentralRole_StartDevice(&security_examples_central_roleCB);

  // Register with bond manager after starting device
  GAPBondMgr_Register(&security_examples_central_bondCB);
  
  //TODO SC OOB SM
  SM_RegisterTask(selfEntity);  

  LCD_WRITE_STRING("Security Ex Centr", LCD_PAGE0);
}

/*********************************************************************
 * @fn      security_examples_central_taskFxn
 *
 * @brief   Application task entry point for the Simple BLE Central.
 *
 * @param   none
 *
 * @return  events not processed
 */
static void security_examples_central_taskFxn(UArg a0, UArg a1)
{
  // Initialize application
  security_examples_central_init();
  
  // Application main loop
  for (;;)
  {
    // Waits for a signal to the semaphore associated with the calling thread.
    // Note that the semaphore associated with a thread is signaled when a
    // message is queued to the message receive queue of the thread or when
    // ICall_signal() function is called onto the semaphore.
    ICall_Errno errno = ICall_wait(ICALL_TIMEOUT_FOREVER);

    if (errno == ICALL_ERRNO_SUCCESS)
    {
      ICall_EntityID dest;
      ICall_ServiceEnum src;
      ICall_HciExtEvt *pMsg = NULL;
      
      if (ICall_fetchServiceMsg(&src, &dest, 
                                (void **)&pMsg) == ICALL_ERRNO_SUCCESS)
      {
        if ((src == ICALL_SERVICE_CLASS_BLE) && (dest == selfEntity))
        {
          // Process inter-task message
          security_examples_central_processStackMsg((ICall_Hdr *)pMsg);
        }

        if (pMsg)
        {
          ICall_freeMsg(pMsg);
        }
      }
    }

    // If RTOS queue is not empty, process app message
    while (!Queue_empty(appMsgQueue))
    {
      sbcEvt_t *pMsg = (sbcEvt_t *)Util_dequeueMsg(appMsgQueue);
      if (pMsg)
      {
        // Process message
        security_examples_central_processAppMsg(pMsg);
        
        // Free the space from the message
        ICall_free(pMsg);
      }
    }
    if (events & SEC_SM_ECC_KEYS_EVT)
    {
      events &= ~SEC_SM_ECC_KEYS_EVT;
    }
  }
}

/*********************************************************************
 * @fn      security_examples_central_processStackMsg
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static void security_examples_central_processStackMsg(ICall_Hdr *pMsg)
{
  switch (pMsg->event)
  {
    case GAP_MSG_EVENT:
      security_examples_central_processRoleEvent((gapCentralRoleEvent_t *)pMsg);
      break;
      
    default:
      break;
  }
}

/*********************************************************************
 * @fn      security_examples_central_processAppMsg
 *
 * @brief   Central application event processing function.
 *
 * @param   pMsg - pointer to event structure
 *
 * @return  none
 */
static void security_examples_central_processAppMsg(sbcEvt_t *pMsg)
{
  switch (pMsg->hdr.event)
  {
    case SEC_STATE_CHANGE_EVT:
      security_examples_central_processStackMsg((ICall_Hdr *)pMsg->pData);
      
      // Free the stack message
      ICall_freeMsg(pMsg->pData);
      break;
      
    case SEC_KEY_CHANGE_EVT:
      security_examples_central_handleKeys(0, pMsg->hdr.state); 
      break;
      
    // Pairing event  
    case SEC_PAIRING_STATE_EVT:
      {
        security_examples_central_processPairState(pMsg->hdr.state, *pMsg->pData);
        
        ICall_free(pMsg->pData);
        break;
      }
      
    // Passcode event    
    case SEC_PASSCODE_NEEDED_EVT:
      {     
        pairDisplay_t *pPair = (pairDisplay_t *)pMsg->pData;
        
        judgeNumericComparison = TRUE;
        
        security_examples_central_processPasscode(connHandle, pPair->uiOutputs, 
                                         pPair->numComparison);
        
        ICall_free(pMsg->pData);
      }
      
    default:
      // Do nothing.
      break;
  }
}

/*********************************************************************
 * @fn      security_examples_central_processRoleEvent
 *
 * @brief   Central role event processing function.
 *
 * @param   pEvent - pointer to event structure
 *
 * @return  none
 */
static void security_examples_central_processRoleEvent(gapCentralRoleEvent_t *pEvent)
{
  switch (pEvent->gap.opcode)
  {
    case GAP_DEVICE_INIT_DONE_EVENT:  
      {        
        LCD_WRITE_STRING(Util_convertBdAddr2Str(pEvent->initDone.devAddr),
                         LCD_PAGE1);
        LCD_WRITE_STRING("Initialized", LCD_PAGE2);
      }
      break;

    case GAP_DEVICE_INFO_EVENT:
      {
        security_examples_central_addDeviceInfo(pEvent->deviceInfo.addr, 
                                       pEvent->deviceInfo.addrType);
      }
      break;
      
    case GAP_DEVICE_DISCOVERY_EVENT:
      {
        // discovery complete
        scanningStarted = FALSE;

        // Copy results
        scanRes = pEvent->discCmpl.numDevs;
        memcpy(devList, pEvent->discCmpl.pDevList,
               (sizeof(gapDevRec_t) * scanRes));
        
        LCD_WRITE_STRING_VALUE("Devices Found", scanRes, 10, LCD_PAGE2);
        
        if (scanRes > 0)
        {
          LCD_WRITE_STRING("<- To Select", LCD_PAGE3);
        }

        // initialize scan index to last device
        scanIdx = scanRes;
      }
      break;
      
    case GAP_LINK_ESTABLISHED_EVENT:
      {
        if (pEvent->gap.hdr.status == SUCCESS)
        {
          state = BLE_STATE_CONNECTED;
          connHandle = pEvent->linkCmpl.connectionHandle;

          LCD_WRITE_STRING("Connected", LCD_PAGE2);
          LCD_WRITE_STRING(Util_convertBdAddr2Str(pEvent->linkCmpl.devAddr),
                           LCD_PAGE3);   
        }
        else
        {
          state = BLE_STATE_IDLE;
          connHandle = GAP_CONNHANDLE_INIT;
          
          LCD_WRITE_STRING("Connect Failed", LCD_PAGE2);
          LCD_WRITE_STRING_VALUE("Reason:", pEvent->gap.hdr.status, 10, 
                                 LCD_PAGE3);
        }
      }
      break;

    case GAP_LINK_TERMINATED_EVENT:
      {
        state = BLE_STATE_IDLE;
        connHandle = GAP_CONNHANDLE_INIT;
        
        LCD_WRITE_STRING("Disconnected", LCD_PAGE2);
        LCD_WRITE_STRING_VALUE("Reason:", pEvent->linkTerminate.reason,
                                10, LCD_PAGE3);
        LCD_WRITE_STRING("", LCD_PAGE4);
      }
      break;

    case GAP_LINK_PARAM_UPDATE_EVENT:
      {
        LCD_WRITE_STRING_VALUE("Param Update:", pEvent->linkUpdate.status,
                                10, LCD_PAGE2);
      }
      break;
      
    default:
      break;
  }
}

/*********************************************************************
 * @fn      security_examples_central_handleKeys
 *
 * @brief   Handles all key events for this device.
 *
 * @param   shift - true if in shift/alt.
 * @param   keys - bit field for key events. Valid entries:
 *                 HAL_KEY_SW_2
 *                 HAL_KEY_SW_1
 *
 * @return  none
 */
static void security_examples_central_handleKeys(uint8_t shift, uint8_t keys)
{
  (void)shift;  // Intentionally unreferenced parameter

  if (keys & KEY_LEFT)
  {
    // Display discovery results
    if (!scanningStarted && scanRes > 0)
    {
      // Increment index of current result (with wraparound)
      scanIdx++;
      if (scanIdx >= scanRes)
      {
        scanIdx = 0;
      }

      LCD_WRITE_STRING_VALUE("Device", (scanIdx + 1), 10, LCD_PAGE2);
      LCD_WRITE_STRING(Util_convertBdAddr2Str(devList[scanIdx].addr), LCD_PAGE3);
    }

    return;
  }

  if (keys & KEY_UP)
  {
    // Start or stop discovery
    if (state != BLE_STATE_CONNECTED)
    {
      if (!scanningStarted)
      {
        scanningStarted = TRUE;
        scanRes = 0;
        
        LCD_WRITE_STRING("Discovering...", LCD_PAGE2);
        LCD_WRITE_STRING("", LCD_PAGE3);
        LCD_WRITE_STRING("", LCD_PAGE4);
        
        GAPCentralRole_StartDiscovery(DEFAULT_DISCOVERY_MODE,
                                      DEFAULT_DISCOVERY_ACTIVE_SCAN,
                                      DEFAULT_DISCOVERY_WHITE_LIST);      
      }
      else
      {
        GAPCentralRole_CancelDiscovery();
      }
    }
    return;
  }

  if (keys & KEY_RIGHT)
  {
    // Numeric Comparisons Success
    if (judgeNumericComparison)
    {
      judgeNumericComparison = FALSE;
      
      // overload 3rd parameter as TRUE when instead of the passcode when
      // numeric comparisons is used.
      GAPBondMgr_PasscodeRsp(connHandle, SUCCESS, TRUE);
      
      LCD_WRITE_STRING("Codes Match!", LCD_PAGE5);

      return;
    }
  }

  if (keys & KEY_SELECT)
  {
    uint8_t addrType;
    uint8_t *peerAddr;
    
    // Connect or disconnect
    if (state == BLE_STATE_IDLE)
    {
      // if there is a scan result
      if (scanRes > 0)
      {
        // connect to current device in scan result
        peerAddr = devList[scanIdx].addr;
        addrType = devList[scanIdx].addrType;
      
        state = BLE_STATE_CONNECTING;
        
        GAPCentralRole_EstablishLink(DEFAULT_LINK_HIGH_DUTY_CYCLE,
                                     DEFAULT_LINK_WHITE_LIST,
                                     addrType, peerAddr);
  
        LCD_WRITE_STRING("Connecting", LCD_PAGE2);
        LCD_WRITE_STRING(Util_convertBdAddr2Str(peerAddr), LCD_PAGE3);
        LCD_WRITE_STRING("", LCD_PAGE4);
      }
    }
    else if (state == BLE_STATE_CONNECTING ||
              state == BLE_STATE_CONNECTED)
    {
      // disconnect
      state = BLE_STATE_DISCONNECTING;

      GAPCentralRole_TerminateLink(connHandle);
      
      LCD_WRITE_STRING("Disconnecting", LCD_PAGE2);
      LCD_WRITE_STRING("", LCD_PAGE4);
    }

    return;
  }

  if (keys & KEY_DOWN)
  {
    // Numeric Comparisons Failed
    if (judgeNumericComparison)
    {
      judgeNumericComparison = FALSE;
      
      // overload 3rd parameter as FALSE when instead of the passcode when
      // numeric comparisons is used.
      GAPBondMgr_PasscodeRsp(connHandle, SUCCESS, FALSE);
      
      LCD_WRITE_STRING("Codes Don't Match :(", LCD_PAGE5);
      
      return;
    }
  }
}

/*********************************************************************
 * @fn      security_examples_central_processPairState
 *
 * @brief   Process the new paring state.
 *
 * @return  none
 */
static void security_examples_central_processPairState(uint8_t state, uint8_t status)
{
  if (state == GAPBOND_PAIRING_STATE_STARTED)
  {
    LCD_WRITE_STRING("Pairing started", LCD_PAGE2);
  }
  else if (state == GAPBOND_PAIRING_STATE_COMPLETE)
  {
    if (status == SUCCESS)
    {
      LCD_WRITE_STRING("Pairing success", LCD_PAGE2);
    }
    else
    {
      LCD_WRITE_STRING_VALUE("Pairing fail:", status, 10, LCD_PAGE2);
    }
  }
  else if (state == GAPBOND_PAIRING_STATE_BONDED)
  {
    if (status == SUCCESS)
    {
      LCD_WRITE_STRING("Bonding success", LCD_PAGE2);
    }
  }
  else if (state == GAPBOND_PAIRING_STATE_BOND_SAVED)
  {
    if (status == SUCCESS)
    {
      LCD_WRITE_STRING("Bond save success", LCD_PAGE2);
    }
    else
    {
      LCD_WRITE_STRING_VALUE("Bond save failed:", status, 10, LCD_PAGE2);
    }
  }
}

/*********************************************************************
 * @fn      security_examples_central_processPasscode
 *
 * @brief   Process the Passcode request.
 *
 * @return  none
 */
static void security_examples_central_processPasscode(uint16_t connectionHandle,
                                             uint8_t uiOutputs, 
                                             uint32_t numComparison)
{ 
  if (numComparison) //this sample only accepts numeric comparison
  {
    LCD_WRITE_STRING_VALUE("Num Cmp:", numComparison, 10, LCD_PAGE4);
  }
}

/**********************************************************************
 * @fn      security_examples_central_addDeviceInfo
 *
 * @brief   Add a device to the device discovery result list
 *
 * @return  none
 */
static void security_examples_central_addDeviceInfo(uint8_t *pAddr, uint8_t addrType)
{
  uint8_t i;
  
  // If result count not at max
  if (scanRes < DEFAULT_MAX_SCAN_RES)
  {
    // Check if device is already in scan results
    for (i = 0; i < scanRes; i++)
    {
      if (memcmp(pAddr, devList[i].addr , B_ADDR_LEN) == 0)
      {
        return;
      }
    }
    
    // Add addr to scan result list
    memcpy(devList[scanRes].addr, pAddr, B_ADDR_LEN);
    devList[scanRes].addrType = addrType;
    
    // Increment scan result count
    scanRes++;
  }
}

/*********************************************************************
 * @fn      security_examples_central_eventCB
 *
 * @brief   Central event callback function.
 *
 * @param   pEvent - pointer to event structure
 *
 * @return  TRUE if safe to deallocate event message, FALSE otherwise.
 */
static uint8_t security_examples_central_eventCB(gapCentralRoleEvent_t *pEvent)
{
  // Forward the role event to the application
  if (security_examples_central_enqueueMsg(SEC_STATE_CHANGE_EVT, 
                                  SUCCESS, (uint8_t *)pEvent))
  {
    // App will process and free the event
    return FALSE;
  }
  
  // Caller should free the event
  return TRUE;
}

/*********************************************************************
 * @fn      security_examples_central_pairStateCB
 *
 * @brief   Pairing state callback.
 *
 * @return  none
 */
static void security_examples_central_pairStateCB(uint16_t connHandle, uint8_t state,
                                         uint8_t status)
{
  uint8_t *pData;
  
  // Allocate space for the event data.
  if ((pData = ICall_malloc(sizeof(uint8_t))))
  {
    *pData = status;  
  
    // Queue the event.
    security_examples_central_enqueueMsg(SEC_PAIRING_STATE_EVT, state, pData);
  }
}

/*********************************************************************
 * @fn      security_examples_central_passcodeCB
 *
 * @brief   Passcode callback.
 *
 * @return  none
 */
static void security_examples_central_passcodeCB(uint8_t *deviceAddr, uint16_t connHandle,
                                        uint8_t uiInputs, uint8_t uiOutputs,
                                        uint32_t numComparison)
{
  pairDisplay_t *pData;
  
  // Allocate space for the passcode event.
  if ((pData = ICall_malloc(sizeof(pairDisplay_t))))
  {
    pData->uiOutputs = uiOutputs;
    pData->numComparison = (uint32_t)numComparison;
    
    // Enqueue the event.
    security_examples_central_enqueueMsg(SEC_PASSCODE_NEEDED_EVT, 0, (uint8 *)pData);
  }
}

/***********************************************************************
 * @fn      security_examples_central_keyChangeHandler
 *
 * @brief   Key event handler function
 *
 * @param   a0 - ignored
 *
 * @return  none
 */
void security_examples_central_keyChangeHandler(uint8 keys)
{
  security_examples_central_enqueueMsg(SEC_KEY_CHANGE_EVT, keys, NULL);
}

/*********************************************************************
 * @fn      security_examples_central_enqueueMsg
 *
 * @brief   Creates a message and puts the message in RTOS queue.
 *
 * @param   event - message event.
 * @param   state - message state.
 * @param   pData - message data pointer.
 *
 * @return  TRUE or FALSE
 */
static uint8_t security_examples_central_enqueueMsg(uint8_t event, uint8_t state, 
                                           uint8_t *pData)
{
  sbcEvt_t *pMsg = ICall_malloc(sizeof(sbcEvt_t));
  
  // Create dynamic pointer to message.
  if (pMsg)
  {
    pMsg->hdr.event = event;
    pMsg->hdr.state = state;
    pMsg->pData = pData;
    
    // Enqueue the message.
    return Util_enqueueMsg(appMsgQueue, sem, (uint8_t *)pMsg);
  }
  
  return FALSE;
}

/*********************************************************************
*********************************************************************/