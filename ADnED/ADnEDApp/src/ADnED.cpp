
/**
 * @brief areaDetector driver that is a V4 neutron data client for nED.
 *
 * @author Matt Pearson
 * @date Sept 2014
 */

#include <iostream>
#include <string>
#include <stdexcept>
#include "dirent.h"
#include <sys/types.h>
#include <syscall.h> 
#include <stdexcept>

#include <stdio.h>
#include <time.h>
#include <stdlib.h>

#include <epicsString.h>

#include <tinyxml.h>

#include <asynDriver.h>

#include <epicsExport.h>
//Epics headers
#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsExport.h>
#include <epicsString.h>
#include <iocsh.h>
#include <drvSup.h>
#include <registryFunction.h>

//#include <mysql.h>

// nexus 
#include <napi.h>
#include "napiconfig.h" 

//<mine 
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
 //mine>

#include "asynNDArrayDriver.h"

//ADnED
#include "ADnED.h"
#include "nEDChannel.h"
#include "ADnEDFile.h"
#include "ADnEDTransform.h"
#include <pv/pvData.h>

using std::cout;
using std::cerr;
using std::endl;
using std::hex;

using std::tr1::shared_ptr;
using nEDChannel::nEDChannelRequester;
using nEDChannel::nEDMonitorRequester;

//Definitions of static class data members
const epicsInt32 ADnED::s_ADNED_MAX_STRING_SIZE = ADNED_MAX_STRING_SIZE;
const epicsInt32 ADnED::s_ADNED_MAX_DETS = ADNED_MAX_DETS;
const epicsInt32 ADnED::s_ADNED_MAX_CHANNELS = ADNED_MAX_CHANNELS;
const epicsUInt32 ADnED::s_ADNED_ALLOC_STATUS_OK = 0;
const epicsUInt32 ADnED::s_ADNED_ALLOC_STATUS_REQ = 1;
const epicsUInt32 ADnED::s_ADNED_ALLOC_STATUS_FAIL = 2;
//These 2D plot options need to match the mbbo record that uses ADNED_DET_2D_TYPE parameter.
const epicsUInt32 ADnED::s_ADNED_2D_PLOT_XY = 0;
const epicsUInt32 ADnED::s_ADNED_2D_PLOT_XTOF = 1;
const epicsUInt32 ADnED::s_ADNED_2D_PLOT_YTOF = 2;
const epicsUInt32 ADnED::s_ADNED_2D_PLOT_PIXELIDTOF = 3;

//C Function prototypes to tie in with EPICS
static void ADnEDEventTaskC(void *drvPvt);
static void ADnEDFrameTaskC(void *drvPvt);

/**
 * Constructor. 
 * This must be called in the Epics IOC startup file.
 * @param portName The Asyn port name to use
 * @param maxBuffers Used by asynPortDriver (set to -1 for unlimited)
 * @param maxMemory Used by asynPortDriver (set to -1 for unlimited)
 * @param debug This debug flag for the driver. 
 */
ADnED::ADnED(const char *portName, int maxBuffers, size_t maxMemory, int debug)
  : ADDriver(portName,
             s_ADNED_MAX_DETS+1, /* maxAddr (different detectors use different asyn address*/ 
             NUM_DRIVER_PARAMS,
             maxBuffers,
             maxMemory,
             asynInt32Mask | asynInt32ArrayMask | asynFloat64Mask | asynFloat32ArrayMask | asynFloat64ArrayMask | asynDrvUserMask | asynOctetMask | asynGenericPointerMask, /* Interface mask */
             asynInt32Mask | asynInt32ArrayMask | asynFloat64Mask | asynFloat32ArrayMask | asynFloat64ArrayMask | asynOctetMask | asynGenericPointerMask,  /* Interrupt mask */
             ASYN_CANBLOCK | ASYN_MULTIDEVICE, /* asynFlags.*/
             1, /* Autoconnect */
             0, /* default priority */
             0), /* Default stack size*/
    m_debug(debug),single_file_num(0),capture_file_num(0),pulse_id_dim(1), capture_group_num(0),storageFlag(1),stream_group_num(0),start_every_slap(0),slab_start{0}, slab_size{0},unlimited_dims{0},common_aChar_len(256),pulse_num_per_file(45000)
{

  int status = asynSuccess;
  const char *functionName = "ADnED::ADnED";
  //Create the epicsEvent for signaling the threads.
  //This will cause it to do a poll immediately, rather than wait for the poll time period.
  m_startEvent = epicsEventMustCreate(epicsEventEmpty);
  if (!m_startEvent) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s epicsEventCreate failure for start event.\n", functionName);
    return;
  }
  m_stopEvent = epicsEventMustCreate(epicsEventEmpty);
  if (!m_stopEvent) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s epicsEventCreate failure for stop event.\n", functionName);
    return;
  }
  m_startFrame = epicsEventMustCreate(epicsEventEmpty);
  if (!m_startFrame) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s epicsEventCreate failure for start frame.\n", functionName);
    return;
  }
  m_stopFrame = epicsEventMustCreate(epicsEventEmpty);
  if (!m_stopFrame) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s epicsEventCreate failure for stop frame.\n", functionName);
    return;
  }

  //Add the params to the paramLib 
  //createParam adds the parameters to all param lists automatically (using maxAddr).
  createParam(ADnEDFirstParamString,              asynParamInt32,    &ADnEDFirstParam);
  createParam(ADnEDResetParamString,              asynParamInt32,    &ADnEDResetParam);
  createParam(ADnEDStartParamString,              asynParamInt32,    &ADnEDStartParam);
  createParam(ADnEDStopParamString,               asynParamInt32,    &ADnEDStopParam);
  createParam(ADnEDPauseParamString,              asynParamInt32,    &ADnEDPauseParam);
  createParam(ADnEDEventDebugParamString,         asynParamInt32,    &ADnEDEventDebugParam);
  createParam(ADnEDSeqCounterParamString,         asynParamInt32,    &ADnEDSeqCounterParam);
  createParam(ADnEDPulseCounterParamString,       asynParamInt32,    &ADnEDPulseCounterParam);
  
  createParam(ADnEDEventRateParamString,          asynParamInt32,    &ADnEDEventRateParam);
  createParam(ADnEDSeqIDParamString,              asynParamInt32,    &ADnEDSeqIDParam);
  createParam(ADnEDSeqIDMissingParamString,       asynParamInt32,    &ADnEDSeqIDMissingParam);
  createParam(ADnEDSeqIDNumMissingParamString,    asynParamInt32,    &ADnEDSeqIDNumMissingParam);
  createParam(ADnEDBadTimeStampParamString,       asynParamInt32,    &ADnEDBadTimeStampParam);
  createParam(ADnEDPChargeParamString,            asynParamFloat64,  &ADnEDPChargeParam);
  createParam(ADnEDPChargeIntParamString,         asynParamFloat64,  &ADnEDPChargeIntParam);
  createParam(ADnEDEventUpdatePeriodParamString,  asynParamFloat64,  &ADnEDEventUpdatePeriodParam);
  createParam(ADnEDFrameUpdatePeriodParamString,  asynParamFloat64,  &ADnEDFrameUpdatePeriodParam);
  createParam(ADnEDNumChannelsParamString,        asynParamInt32,    &ADnEDNumChannelsParam);
  createParam(ADnEDPVNameParamString,             asynParamOctet,    &ADnEDPVNameParam);
  createParam(ADnEDNumDetParamString,             asynParamInt32,    &ADnEDNumDetParam);
  createParam(ADnEDDetPixelNumStartParamString,   asynParamInt32,    &ADnEDDetPixelNumStartParam);
  createParam(ADnEDDetPixelNumEndParamString,     asynParamInt32,    &ADnEDDetPixelNumEndParam);
  createParam(ADnEDDetPixelNumSizeParamString,    asynParamInt32,    &ADnEDDetPixelNumSizeParam);
  createParam(ADnEDDetTOFNumBinsParamString,      asynParamInt32,    &ADnEDDetTOFNumBinsParam);
  createParam(ADnEDDet2DTypeParamString,          asynParamInt32,    &ADnEDDet2DTypeParam);
  createParam(ADnEDDetNDArrayStartParamString,    asynParamInt32,    &ADnEDDetNDArrayStartParam);
  createParam(ADnEDDetNDArrayEndParamString,      asynParamInt32,    &ADnEDDetNDArrayEndParam);
  createParam(ADnEDDetNDArraySizeParamString,     asynParamInt32,    &ADnEDDetNDArraySizeParam);
  createParam(ADnEDDetNDArrayTOFStartParamString, asynParamInt32,    &ADnEDDetNDArrayTOFStartParam);
  createParam(ADnEDDetNDArrayTOFEndParamString,   asynParamInt32,    &ADnEDDetNDArrayTOFEndParam);
  createParam(ADnEDDetEventRateParamString,       asynParamInt32,    &ADnEDDetEventRateParam);
  createParam(ADnEDDetEventTotalParamString,      asynParamFloat64,  &ADnEDDetEventTotalParam);
  createParam(ADnEDDetTOFROIStartParamString,     asynParamInt32,    &ADnEDDetTOFROIStartParam);
  createParam(ADnEDDetTOFROISizeParamString,      asynParamInt32,    &ADnEDDetTOFROISizeParam);
  createParam(ADnEDDetTOFROIEnableParamString,    asynParamInt32,    &ADnEDDetTOFROIEnableParam);
  createParam(ADnEDDetTOFArrayResetParamString,   asynParamInt32,    &ADnEDDetTOFArrayResetParam);
  //Params to use with ADnEDTransform
  createParam(ADnEDDetTOFTransFile0ParamString,   asynParamOctet,    &ADnEDDetTOFTransFile0Param);
  createParam(ADnEDDetTOFTransFile1ParamString,   asynParamOctet,    &ADnEDDetTOFTransFile1Param);
  createParam(ADnEDDetTOFTransFile2ParamString,   asynParamOctet,    &ADnEDDetTOFTransFile2Param);
  createParam(ADnEDDetTOFTransFile3ParamString,   asynParamOctet,    &ADnEDDetTOFTransFile3Param);
  createParam(ADnEDDetTOFTransFile4ParamString,   asynParamOctet,    &ADnEDDetTOFTransFile4Param);
  createParam(ADnEDDetTOFTransFile5ParamString,   asynParamOctet,    &ADnEDDetTOFTransFile5Param);
  createParam(ADnEDDetTOFTransInt0ParamString,    asynParamInt32,    &ADnEDDetTOFTransInt0Param);
  createParam(ADnEDDetTOFTransInt1ParamString,    asynParamInt32,    &ADnEDDetTOFTransInt1Param);
  createParam(ADnEDDetTOFTransInt2ParamString,    asynParamInt32,    &ADnEDDetTOFTransInt2Param);
  createParam(ADnEDDetTOFTransInt3ParamString,    asynParamInt32,    &ADnEDDetTOFTransInt3Param);
  createParam(ADnEDDetTOFTransInt4ParamString,    asynParamInt32,    &ADnEDDetTOFTransInt4Param);
  createParam(ADnEDDetTOFTransInt5ParamString,    asynParamInt32,    &ADnEDDetTOFTransInt5Param);
  createParam(ADnEDDetTOFTransFloat0ParamString,  asynParamFloat64,  &ADnEDDetTOFTransFloat0Param);  
  createParam(ADnEDDetTOFTransFloat1ParamString,  asynParamFloat64,  &ADnEDDetTOFTransFloat1Param);
  createParam(ADnEDDetTOFTransFloat2ParamString,  asynParamFloat64,  &ADnEDDetTOFTransFloat2Param);  
  createParam(ADnEDDetTOFTransFloat3ParamString,  asynParamFloat64,  &ADnEDDetTOFTransFloat3Param);  
  createParam(ADnEDDetTOFTransFloat4ParamString,  asynParamFloat64,  &ADnEDDetTOFTransFloat4Param);  
  createParam(ADnEDDetTOFTransFloat5ParamString,  asynParamFloat64,  &ADnEDDetTOFTransFloat5Param);  
  createParam(ADnEDDetTOFTransPrintParamString,   asynParamInt32,    &ADnEDDetTOFTransPrintParam);
  createParam(ADnEDDetTOFTransDebugParamString,   asynParamInt32,    &ADnEDDetTOFTransDebugParam);
  createParam(ADnEDDetTOFTransTypeParamString,    asynParamInt32,    &ADnEDDetTOFTransTypeParam);
  createParam(ADnEDDetTOFTransOffsetParamString,  asynParamFloat64,  &ADnEDDetTOFTransOffsetParam);
  createParam(ADnEDDetTOFTransScaleParamString,   asynParamFloat64,  &ADnEDDetTOFTransScaleParam);
  //
  createParam(ADnEDDetPixelMapFileParamString,    asynParamOctet,    &ADnEDDetPixelMapFileParam);
  createParam(ADnEDDetPixelMapPrintParamString,   asynParamInt32,    &ADnEDDetPixelMapPrintParam);
  createParam(ADnEDDetPixelMapEnableParamString,  asynParamInt32,    &ADnEDDetPixelMapEnableParam);
  createParam(ADnEDDetPixelROIStartXParamString,  asynParamInt32,    &ADnEDDetPixelROIStartXParam);
  createParam(ADnEDDetPixelROISizeXParamString,   asynParamInt32,    &ADnEDDetPixelROISizeXParam);
  createParam(ADnEDDetPixelROIStartYParamString,  asynParamInt32,    &ADnEDDetPixelROIStartYParam);
  createParam(ADnEDDetPixelROISizeYParamString,   asynParamInt32,    &ADnEDDetPixelROISizeYParam);
  createParam(ADnEDDetPixelSizeXParamString,      asynParamInt32,    &ADnEDDetPixelSizeXParam);
  createParam(ADnEDDetPixelROIEnableParamString,  asynParamInt32,    &ADnEDDetPixelROIEnableParam);
  createParam(ADnEDTOFMaxParamString,             asynParamInt32,    &ADnEDTOFMaxParam);
  createParam(ADnEDAllocSpaceParamString,         asynParamInt32,    &ADnEDAllocSpaceParam);
  createParam(ADnEDAllocSpaceStatusParamString,   asynParamInt32,    &ADnEDAllocSpaceStatusParam);

  //mine
  createParam(ADnEDSingleHdfFileNumberParamString,      asynParamInt32,    &ADnEDSingleHdfFileNumberParam);
  createParam(ADnEDCaptureHdfFileNumberParamString,      asynParamInt32,    &ADnEDCaptureHdfFileNumberParam);
  createParam(ADnEDHdfFullFileNameParamString,    asynParamOctet,    &ADnEDHdfFullFileNameParam);
  createParam(ADnEDHdfWriteModeParamString,       asynParamInt32,    &ADnEDHdfWriteModeParam);
  createParam(ADnEDHdfFileTemplateParamString,    asynParamOctet,    &ADnEDHdfFileTemplateParam);
  createParam(ADnEDHdfFilePathParamString,        asynParamOctet,    &ADnEDHdfFilePathParam);

  createParam(ADnEDHdfPauseParamString,       asynParamInt32,    &ADnEDHdfPauseParam);
  createParam(ADnEDHdfStatusMessageParamString,        asynParamOctet,    &ADnEDHdfStatusMessageParam);
  
  createParam(ADnEDHdfHV1MessageParamString,        asynParamOctet,    &ADnEDHdfHV1MessageParam);
  createParam(ADnEDHdfHV2MessageParamString,        asynParamOctet,    &ADnEDHdfHV2MessageParam);
  createParam(ADnEDHdfGasContentMessageParamString,        asynParamOctet,    &ADnEDHdfGasContentMessageParam);

  createParam(ADnEDHdfNumPulsePerFileParamString,        asynParamInt32,    &ADnEDHdfNumPulsePerFileParam);

  // warning  don't access
  createParam(ADnEDLastParamString,               asynParamInt32,    &ADnEDLastParam);  
  



  //Initialize non static, non const, data members
  m_acquiring = 0;
  for (int chan=0; chan<s_ADNED_MAX_CHANNELS; ++chan) {
    m_seqCounter[chan] = 0;
    m_seqID[chan] = 0;
    m_lastSeqID[chan] = -1; //Init to -1 to catch packet trains stuck at zero
  }
  m_pulseCounter = 0;
  m_pChargeInt = 0.0;
  m_nowTimeSecs = 0.0;
  m_lastTimeSecs = 0.0;
  p_Data = NULL;
  m_dataAlloc = true;
  m_dataMaxSize = 0;
  m_bufferMaxSize = 0;
  m_tofMax = 0;
  for (int chan=0; chan<s_ADNED_MAX_CHANNELS; ++chan) {
    m_TimeStamp[chan].put(0,0);
    m_TimeStampLast[chan].put(0,0);
  }

  for (int i=0; i<=s_ADNED_MAX_DETS; ++i) {
    p_PixelMap[i] = NULL;
    m_PixelMapSize[i] = 0;
    
    m_detStartValues[i] = 0;
    m_detEndValues[i] = 0;
    m_detSizeValues[i] = 0;
    m_NDArrayStartValues[i] = 0;
    m_NDArrayTOFStartValues[i] = 0;
    m_detTOFROIStartValues[i] = 0;
    m_detTOFROISizeValues[i] = 0;
    m_detTOFROIEnabled[i] = 0;
    m_detPixelMappingEnabled[i] = 0;
    m_detTOFTransType[i] = 0;
    m_detTOFTransOffset[i] = 0;
    m_detTOFTransScale[i] = 0;

    m_detPixelROIStartX[i] = 0;
    m_detPixelROISizeX[i] = 0;
    m_detPixelROIStartY[i] = 0;
    m_detPixelROISizeY[i] = 0;
    m_detPixelSizeX[i] = 0;
    m_detPixelROIEnable[i] = 0;
    m_detTotalEvents[i] = 0.0;
  }
  
  //Create the thread that reads the data 
  status = (epicsThreadCreate("ADnEDEventTask",
                            epicsThreadPriorityHigh,
                            epicsThreadGetStackSize(epicsThreadStackMedium),
                            (EPICSTHREADFUNC)ADnEDEventTaskC,
                            this) == NULL);
  if (status) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s epicsThreadCreate failure for ADnEDEventTask.\n", functionName);
    return;
  }

   //Create the thread that copies the frames for areaDetector plugins 
  status = (epicsThreadCreate("ADnEDFrameTask",
                            epicsThreadPriorityMedium,
                            epicsThreadGetStackSize(epicsThreadStackMedium),
                            (EPICSTHREADFUNC)ADnEDFrameTaskC,
                            this) == NULL);
  if (status) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s epicsThreadCreate failure for ADnEDFrameTask.\n", functionName);
    return;
  }

  std::string channelStr("ADnED Channel");
  std::string monitorStr("ADnED Monitor");
  p_ChannelRequester = (shared_ptr<nEDChannelRequester>)(new nEDChannelRequester(channelStr)); 
  if (!p_ChannelRequester) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s ERROR: Failed to create nEDChannelRequester.\n", functionName);
    return;
  }
  //Create a different monitor requestor for each PVAccess channel that we want to connect to. This is
  //so we can distinguish which channel caused a monitor, which is necessary in the eventHandler function.
  for (int channel=0; channel<s_ADNED_MAX_CHANNELS; ++channel) {
    p_MonitorRequester[channel] = (shared_ptr<nEDMonitorRequester>)(new nEDMonitorRequester(monitorStr, this, channel));  
    if (!p_MonitorRequester[channel]) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s ERROR: Failed to create nEDMonitorRequester. channel: %d\n", functionName, channel);
      return;
    }
  }

  bool paramStatus = true;
  //Initialise any paramLib parameters that need passing up to device support
  paramStatus = ((setIntegerParam(ADnEDResetParam, 0) == asynSuccess) && paramStatus);
  paramStatus = ((setIntegerParam(ADnEDStartParam, 0) == asynSuccess) && paramStatus);
  paramStatus = ((setIntegerParam(ADnEDStopParam, 0) == asynSuccess) && paramStatus);
  paramStatus = ((setIntegerParam(ADnEDPauseParam, 0) == asynSuccess) && paramStatus);
  paramStatus = ((setIntegerParam(ADnEDEventDebugParam, 0) == asynSuccess) && paramStatus);
  paramStatus = ((setIntegerParam(ADnEDPulseCounterParam, 0) == asynSuccess) && paramStatus);


  paramStatus = ((setIntegerParam(ADnEDEventRateParam, 0) == asynSuccess) && paramStatus);
  paramStatus = ((setDoubleParam(ADnEDPChargeParam, 0.0) == asynSuccess) && paramStatus);
  paramStatus = ((setDoubleParam(ADnEDPChargeIntParam, 0.0) == asynSuccess) && paramStatus);
  paramStatus = ((setIntegerParam(ADnEDNumDetParam, 0) == asynSuccess) && paramStatus);
  paramStatus = ((setIntegerParam(ADnEDNumChannelsParam, 0) == asynSuccess) && paramStatus);
  //Loop over asyn addresses for detector and channel specific params. We create both here because
  //we are using the Asyn address to handle both the detector and channel params. 
  for (int det=0; det<=s_ADNED_MAX_DETS; det++) {

    //Channel params (0-based)
    paramStatus = ((setIntegerParam(det, ADnEDSeqCounterParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDSeqIDParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setStringParam(det, ADnEDPVNameParam, " ") == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDSeqIDMissingParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDSeqIDNumMissingParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDBadTimeStampParam, 0) == asynSuccess) && paramStatus);

    //Detector params (1-based) - we just don't use the addr=0 params.
    paramStatus = ((setIntegerParam(det, ADnEDDetPixelNumStartParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetPixelNumEndParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetPixelNumSizeParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetTOFNumBinsParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDet2DTypeParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetNDArrayStartParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetNDArrayEndParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetNDArraySizeParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetNDArrayTOFStartParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetNDArrayTOFEndParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetEventRateParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setDoubleParam(det, ADnEDDetEventTotalParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetTOFROIStartParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetTOFROISizeParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetTOFROIEnableParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetTOFArrayResetParam, 0) == asynSuccess) && paramStatus);
    //Params to use with ADnEDTransform
    paramStatus = ((setStringParam(det, ADnEDDetTOFTransFile0Param, " ") == asynSuccess) && paramStatus);
    //paramStatus = ((setStringParam(det, NDHdfFileName, " ") == asynSuccess) && paramStatus);
    paramStatus = ((setStringParam(det, ADnEDDetTOFTransFile1Param, " ") == asynSuccess) && paramStatus);
    paramStatus = ((setStringParam(det, ADnEDDetTOFTransFile2Param, " ") == asynSuccess) && paramStatus);
    paramStatus = ((setStringParam(det, ADnEDDetTOFTransFile3Param, " ") == asynSuccess) && paramStatus);
    paramStatus = ((setStringParam(det, ADnEDDetTOFTransFile4Param, " ") == asynSuccess) && paramStatus);
    paramStatus = ((setStringParam(det, ADnEDDetTOFTransFile5Param, " ") == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetTOFTransInt0Param, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetTOFTransInt1Param, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetTOFTransInt2Param, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetTOFTransInt3Param, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetTOFTransInt4Param, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetTOFTransInt5Param, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setDoubleParam(det, ADnEDDetTOFTransFloat0Param, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setDoubleParam(det, ADnEDDetTOFTransFloat1Param, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setDoubleParam(det, ADnEDDetTOFTransFloat2Param, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setDoubleParam(det, ADnEDDetTOFTransFloat3Param, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setDoubleParam(det, ADnEDDetTOFTransFloat4Param, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setDoubleParam(det, ADnEDDetTOFTransFloat5Param, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetTOFTransDebugParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetTOFTransTypeParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setDoubleParam(det, ADnEDDetTOFTransOffsetParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setDoubleParam(det, ADnEDDetTOFTransScaleParam, 1) == asynSuccess) && paramStatus);


    // mine  
    paramStatus = ((setStringParam(det, ADnEDHdfFullFileNameParam, " ") == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDHdfWriteModeParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setStringParam(det, ADnEDHdfFileTemplateParam, "") == asynSuccess) && paramStatus);
    paramStatus = ((setStringParam(det, ADnEDHdfFilePathParam, "") == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDSingleHdfFileNumberParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDCaptureHdfFileNumberParam, 0) == asynSuccess) && paramStatus);

    paramStatus = ((setIntegerParam(det, ADnEDHdfPauseParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setStringParam(det, ADnEDHdfStatusMessageParam, "") == asynSuccess) && paramStatus);
    
    paramStatus = ((setStringParam(det, ADnEDHdfHV1MessageParam, "-400V") == asynSuccess) && paramStatus);
    paramStatus = ((setStringParam(det, ADnEDHdfHV2MessageParam, "-1100") == asynSuccess) && paramStatus);
    paramStatus = ((setStringParam(det, ADnEDHdfGasContentMessageParam, "90%Co2:10%Ne") == asynSuccess) && paramStatus);

    paramStatus = ((setIntegerParam(det, ADnEDHdfNumPulsePerFileParam, 90000) == asynSuccess) && paramStatus);

    paramStatus = ((setStringParam(det, ADnEDDetPixelMapFileParam, " ") == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetPixelMapEnableParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetPixelROIStartXParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetPixelROISizeXParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetPixelROIStartYParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetPixelROISizeYParam, 0) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetPixelSizeXParam, 1) == asynSuccess) && paramStatus);
    paramStatus = ((setIntegerParam(det, ADnEDDetPixelROIEnableParam, 0) == asynSuccess) && paramStatus);
    callParamCallbacks(det);
  }
  paramStatus = ((setIntegerParam(ADnEDTOFMaxParam, 0) == asynSuccess) && paramStatus);
  paramStatus = ((setIntegerParam(ADnEDAllocSpaceParam, 0) == asynSuccess) && paramStatus);
  paramStatus = ((setIntegerParam(ADnEDAllocSpaceStatusParam, s_ADNED_ALLOC_STATUS_OK) == asynSuccess) && paramStatus);

  paramStatus = ((setStringParam (ADManufacturer, "CSNS") == asynSuccess) && paramStatus);
  paramStatus = ((setStringParam (ADModel, "nED areaDetector") == asynSuccess) && paramStatus);



  callParamCallbacks();

  if (!paramStatus) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s Unable To Set Driver Parameters In Constructor.\n", functionName);
  }

  for (int det=1; det<=s_ADNED_MAX_DETS; det++) {
    p_Transform[det] = new ADnEDTransform();
  }

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s End Of Constructor.\n", functionName);

  epicsThreadSleep(1);

}

/**
 * Destructor. Should never get here.
 */
ADnED::~ADnED() 
{
  NXclose (&capture_file_id);
  NXclosegroup (stream_file_id);   
  NXclose (&stream_file_id); 
  asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "ADnED::~ADnED Called.\n");
 

}

/**
 * Class function to create a PVAccess client factory
 */
asynStatus ADnED::createFactory()
{
  try {
    printf("Starting ClientFactory::start()\n");
    epics::pvAccess::ClientFactory::start();
  } catch (std::exception &e)  {
    fprintf(stderr, "ERROR: Exception for ClientFactory::start(). Exception: %s\n", e.what());
    return asynError;
  }

  epicsThreadSleep(1);

  return asynSuccess;

}


/** Report status of the driver.
  * Prints details about the detector in us if details>0.
  * It then calls the ADDriver::report() method.
  * \param[in] fp File pointed passed by caller where the output is written to.
  * \param[in] details Controls the level of detail in the report. */
void ADnED::report(FILE *fp, int details)
{
 
  fprintf(fp, "ADnED::report.\n");

  fprintf(fp, "ADnED port=%s\n", this->portName);
  if (details > 0) { 
    fprintf(fp, "ADnED driver details...\n");
  }

  fprintf(fp, "ADnED finished.\n");
  
  //Call the base class method
  asynNDArrayDriver::report(fp, details);

}


/**
 * Reimplementing this function from ADDriver to deal with integer values.
 */ 
asynStatus ADnED::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
   getStringParam(ADnEDHdfFilePathParam, sizeof(hdfFilePath), hdfFilePath); 

  asynStatus status = asynSuccess;
  int function = pasynUser->reason;
  int addr = 0;
  int adStatus = 0;
  const char *functionName = "ADnED::writeInt32";
  
  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Entry.\n", functionName);

  //Read address (ie. det number).
  status = getAddress(pasynUser, &addr); 
  if (status!=asynSuccess) {
    return(status);
  }

  getIntegerParam(ADStatus, &adStatus);

  if (function == ADnEDResetParam) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Reset.\n", functionName);
    cout << "Reset some params (total counts, PCharge, seq numbers, etc, without doing a start." << endl;
    if (clearParams() != asynSuccess) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s: ERROR: Failed to run clearParams on Reset.\n", functionName);
    }
  } else if (function == ADnEDStartParam) {
    if (value) {
      if ((adStatus == ADStatusIdle) || (adStatus == ADStatusError) || (adStatus == ADStatusAborted)) {
        cout << "Start acqusition." << endl;
        if (clearParams() != asynSuccess) {
          asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s: ERROR: Failed to run clearParams on start.\n", functionName);
        }
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Start Reading Events.\n", functionName);
        cout << "Sending start event" << endl;
        setIntegerParam(ADnEDPauseParam, 0);
        epicsEventSignal(this->m_startEvent);
      } else {
        //If we have tried to Start while still acquiring, or some other state, we need
        //to clear the busy record anyway. Print an error so we know about it.
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                  "%s: Tried to Start from an invalid state (ADStatus=%d).\n", functionName, adStatus);
        //Note: this does not work. I need to clear the busy from outside this function.
        setIntegerParam(ADnEDStartParam, 0);
        callParamCallbacks();
        return asynError;
      }
    } 
  } else if (function == ADnEDStopParam) {
    if (value) {
      if (adStatus == ADStatusAcquire) {
        cout << "Stop acqusition." << endl;
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Stop Reading Events.\n", functionName);
        cout << "Sending stop event" << endl;
        setIntegerParam(ADnEDPauseParam, 0);
        epicsEventSignal(this->m_stopEvent);
      } else {
        //If we have tried to Stop while not acquiring, or some other state, we need
        //to clear the busy record anyway. Print an error so we know about it.
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                  "%s: Tried to Stop from an invalid state (ADStatus=%d).\n", functionName, adStatus);
        //Note: this does not work. I need to clear the busy from outside this function.
        setIntegerParam(ADnEDStopParam, 0);
        callParamCallbacks();
        return asynError;
      }
    }
  } else if (function == ADnEDDetPixelNumStartParam) {
    if (adStatus != ADStatusAcquire) {
      m_dataAlloc = true;
    } else {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s. Cannot configure during acqusition.\n", functionName);
      return asynError;
    }
  } else if (function == ADnEDDetPixelNumEndParam) {
    if (adStatus != ADStatusAcquire) {
      m_dataAlloc = true;
    } else {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s. Cannot configure during acqusition.\n", functionName);
      return asynError;
    }
  } else if (function == ADnEDDetPixelNumSizeParam) {
    if (adStatus != ADStatusAcquire) {
      m_dataAlloc = true;
    } else {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s. Cannot configure during acqusition.\n", functionName);
      return asynError;
    }
  } else if (function == ADnEDTOFMaxParam) {
    if (adStatus != ADStatusAcquire) {
      m_dataAlloc = true;
    } else {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s. Cannot configure during acqusition.\n", functionName);
      return asynError;
    }
  } else if (function == ADnEDAllocSpaceParam) {
    if (adStatus != ADStatusAcquire) {
      if (allocArray() != asynSuccess) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s: ERROR: Failed to allocate array.\n", functionName);
        setIntegerParam(ADnEDAllocSpaceStatusParam, s_ADNED_ALLOC_STATUS_FAIL);
        setStringParam(ADStatusMessage, "allocArray Error");
        setIntegerParam(ADStatus, ADStatusError);
        callParamCallbacks();
        return asynError;
      } 
    } else {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s. Cannot configure during acqusition.\n", functionName);
      return asynError;
    }
  } else if (function == ADnEDNumDetParam) {
    if (adStatus != ADStatusAcquire) {
      if (value > s_ADNED_MAX_DETS) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                  "%s Error Setting Number Of Detectors. Max: %d\n", 
                  functionName, s_ADNED_MAX_DETS);
        return asynError;
      }
    } else {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s. Cannot configure during acqusition.\n", functionName);
      return asynError;
    } 
  } else if (function == ADnEDNumChannelsParam) {
      if (adStatus != ADStatusAcquire) {
      if (value > s_ADNED_MAX_CHANNELS) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                  "%s Error Setting Number Of PVAccess channels. Max: %d\n", 
                  functionName, s_ADNED_MAX_CHANNELS);
        return asynError;
      }
    } else {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s. Cannot configure during acqusition.\n", functionName);
      return asynError;
    }
  } else if (function == ADnEDDetTOFTransPrintParam) {
    printTofTrans(addr);
  } else if (function == ADnEDDetTOFTransDebugParam) {
    if (value != 0) {
      p_Transform[addr]->setDebug(true);
    } else {
      p_Transform[addr]->setDebug(false);
    }
  } else if (function == ADnEDDetPixelMapPrintParam) {
    printPixelMap(addr);
  } else if (function == ADnEDDetPixelROISizeXParam) {
    if (value <= 0) {
      value = 1;
    }
  } else if (function == ADnEDDetPixelROIEnableParam) {
    //If we enable Pixel ROI Filter, disable the TOF ROI Filter
    if (value == 1) {
      setIntegerParam(addr, ADnEDDetTOFROIEnableParam, 0);
    }
  } else if (function == ADnEDDetTOFROIEnableParam) {
    //If we enable TOF ROI Filter, disable the Pixel ROI Filter
    if (value == 1) {
      setIntegerParam(addr, ADnEDDetPixelROIEnableParam, 0);
    }
  } else if (function == ADnEDDetTOFArrayResetParam) {
    //Clear the TOF Array for this detector
    resetTOFArray(addr);
  }

  epicsUInt32 transIndex = 0;
  if (matchTransInt(function, transIndex)) {
    if (p_Transform[addr]->setIntParam(transIndex, value) != ADNED_TRANSFORM_OK) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s Error loading int32 into p_Transform[%d]. transIndex: %d, value: %d\n", functionName, addr, transIndex, value);
      status = asynError;
    }
  }

  if (m_dataAlloc) {
    setIntegerParam(ADnEDAllocSpaceStatusParam, s_ADNED_ALLOC_STATUS_REQ);
    callParamCallbacks();
  }

  if (status != asynSuccess) {
    callParamCallbacks(addr);
    return asynError;
  }

  status = (asynStatus) setIntegerParam(addr, function, value);
  if (status!=asynSuccess) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
              "%s Error Setting Parameter. Asyn addr: %d, asynUser->reason: %d, value: %d\n", 
              functionName, addr, function, value);
    return(status);
  }

  //Do callbacks so higher layers see any changes 
  callParamCallbacks(addr);

  return status;
}


/**
 * Reimplementing this function from ADDriver to deal with floating point values.
 */ 
asynStatus ADnED::writeFloat64(asynUser *pasynUser, epicsFloat64 value)
{
  getStringParam(ADnEDHdfFilePathParam, sizeof(hdfFilePath), hdfFilePath); 

  int function = pasynUser->reason;
  int addr = 0;
  asynStatus status = asynSuccess;
  const char *functionName = "ADnED::writeFloat64";
  
  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Entry.\n", functionName);
 
  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s asynUser->reason: %d, value: %f, addr: %d\n", functionName, function, value, addr);
  //Read address (ie. det number).
  status = getAddress(pasynUser, &addr); 
  if (status!=asynSuccess) {
    return(status);
  }

  epicsUInt32 transIndex = 0;
  if (matchTransFloat(function, transIndex)) {
    if (p_Transform[addr]->setDoubleParam(transIndex, value) != ADNED_TRANSFORM_OK) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s Error loading float64 into p_Transform[%d]. transIndex: %d, value: %f\n", functionName, addr, transIndex, value);
      status = asynError;
    }
  }

  if (function == ADnEDFrameUpdatePeriodParam) {
    printf("Setting ADnEDFrameUpdatePeriodParam to %f\n", value);
  }

  if (status != asynSuccess) {
    callParamCallbacks(addr);
    return asynError;
  }

  //Set in param lib so the user sees a readback straight away. We might overwrite this in the 
  //status task, depending on the parameter.
  status = (asynStatus) setDoubleParam(addr, function, value);
  if (status!=asynSuccess) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
              "%s Error Setting Parameter. Asyn addr: %d, asynUser->reason: %d, value: %f\n", 
              functionName, addr, function, value);
    return(status);
  }
  
  //Do callbacks so higher layers see any changes 
  callParamCallbacks(addr);
  
  return status;
}



/**
 * Reimplementing this function from asynNDArrayDriver to deal with strings.
 */
asynStatus ADnED::writeOctet(asynUser *pasynUser, const char *value, 
                                    size_t nChars, size_t *nActual)
{
     getStringParam(ADnEDHdfFilePathParam, sizeof(hdfFilePath), hdfFilePath); 


  int function = pasynUser->reason;
  int addr = 0;
  asynStatus status = asynSuccess;
  const char *functionName = "ADnED::writeOctet";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Entry.\n", functionName);
   
  //Read address (ie. det number).
  status = getAddress(pasynUser, &addr); 
  if (status!=asynSuccess) {
    return(status);
  }
 
  epicsUInt32 transIndex = 0;
  if (matchTransFile(function, transIndex)) {
        
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
              "%s Set Det %d TOF Transformation (Index %d) File: %s.\n", functionName, addr, transIndex, value);
       
    epicsUInt32 arraySize = 0;
    epicsFloat64 *pArray = NULL;

    try {
      ADnEDFile file = ADnEDFile(value);
      if (file.getSize() != 0) { 
        arraySize = file.getSize();
        if (pArray == NULL) {
          pArray = static_cast<epicsFloat64 *>(calloc(arraySize, sizeof(epicsFloat64)));
        }
        file.readDataIntoDoubleArray(&pArray);
        if (p_Transform[addr]->setDoubleArray(transIndex, pArray, arraySize) != ADNED_TRANSFORM_OK) {
          asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s Error loading array into p_Transform[%d]\n", functionName, addr);
          status = asynError;
        }
      }
    } catch (std::exception &e) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s Error Parsing TOF Transformation File. Det: %d. %s\n", functionName, addr, e.what());
      status = asynError;
    }

    if (pArray != NULL) {
      free(pArray);
    }
    
  } else if (function == ADnEDDetPixelMapFileParam) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
              "%s Set Det %d Pixel Map File: %s.\n", functionName, addr, value);
    
    if (p_PixelMap[addr]) {
        free(p_PixelMap[addr]);
        p_PixelMap[addr] = NULL;
        m_PixelMapSize[addr] = 0;
      }

    try {
      ADnEDFile file = ADnEDFile(value);
      if (file.getSize() != 0) {
        m_PixelMapSize[addr] = file.getSize();
        if (p_PixelMap[addr] == NULL) {
          p_PixelMap[addr] = static_cast<epicsUInt32 *>(calloc(m_PixelMapSize[addr], sizeof(epicsUInt32)));
        }
        file.readDataIntoIntArray(&p_PixelMap[addr]);
        if ((status = checkPixelMap(addr)) == asynError) {
          free(p_PixelMap[addr]);
          p_PixelMap[addr] = NULL;
        }
      }
    } catch (std::exception &e) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s Error parsing pixel mapping file. Det: %d. %s\n", functionName, addr, e.what());
      free(p_PixelMap[addr]);
      p_PixelMap[addr] = NULL;
    }
  } else {
    // If this parameter belongs to a base class call its method 
    if (function < ADNED_FIRST_DRIVER_COMMAND) {
      status = asynNDArrayDriver::writeOctet(pasynUser, value, nChars, nActual);
    }
  }
  
  if (status != asynSuccess) {
    callParamCallbacks(addr);
    return asynError;
  }
  
  // Set the parameter in the parameter library. 
  status = (asynStatus)setStringParam(addr, function, (char *)value);
  // Do callbacks so higher layers see any changes 
  status = (asynStatus)callParamCallbacks(addr);
  
  if (status) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
              "%s Error Setting Parameter. asynUser->reason: %d\n", 
              functionName, function);
  }
  
  *nActual = nChars;
  return status;
}

/**
 * For debug purposes, print pixel map array to stdout.
 * @param det The detector number (1 based)
 */

void ADnED::print_data (const char *prefix, void *data, int type, int num)
{
  int i;
  printf ("%s", prefix);
  for (i = 0; i < num; i++) {
      switch (type) {
        case NX_CHAR:
           printf ("%c", ((char *) data)[i]);
           break;
        case NX_INT8:
           printf (" %d", ((unsigned char *) data)[i]);
           break;
        case NX_INT16:
           printf (" %d", ((short *) data)[i]);
           break;
        case NX_INT32:
           printf (" %d", ((int *) data)[i]);
           break;
        case NX_INT64:
           printf (" %lld", (long long)((int64_t *) data)[i]);
           break;
        case NX_UINT64:
           printf (" %llu", (unsigned long long)((uint64_t *) data)[i]);
           break;
        case NX_FLOAT32:
           printf (" %f", ((float *) data)[i]);
           break;
        case NX_FLOAT64:
           printf (" %f", ((double *) data)[i]);
           break;
        default:
           printf ("print_data: invalid type");
           break;
        }
     }
  }


void ADnED::printPixelMap(epicsUInt32 det)
{ 

//    // <mine 
 /* int i, j, k, n, NXrank, NXdims[32], NXtype, NXlen, entry_status, attr_status;
  float r;
  void *data_buffer;
  unsigned char i1_array[4] = {1, 2, 3, 4};
  short int i2_array[4] = {1000, 2000, 3000, 4000};
  int i4_array[4] = {1000000, 2000000, 3000000, 4000000};
  float r4_array[5][4] =
  {{1., 2., 3., 4.}, {5., 6., 7., 8.}, {9., 10., 11., 12.}, {13., 14., 15., 16.}, {17., 18., 19., 20.}};
  double r8_array[5][4] =
  {{1., 2., 3., 4.}, {5., 6., 7., 8.}, {9., 10., 11., 12.}, {13., 14., 15., 16.}, {17., 18., 19., 20.}};
  int array_dims[2] = {5, 4};
  int unlimited_dims[1] = {NX_UNLIMITED};
  int chunk_size[2]={5,4};
  int slab_start[2], slab_size[2];
  char name[64], char_class[64], char_buffer[128];
  char group_name[64], class_name[64];
  char c1_array[5][4] = {{'a', 'b', 'c' ,'d'}, {'e', 'f', 'g' ,'h'}, 
     {'i', 'j', 'k', 'l'}, {'m', 'n', 'o', 'p'}, {'q', 'r', 's' , 't'}};
  int unlimited_cdims[2] = {NX_UNLIMITED, 4};
  NXhandle fileid, clone_fileid;
  NXlink glink, dlink, blink;
  int comp_array[100][20];
  int dims[2];
  int cdims[2];
  int nx_creation_code;
  char nxFile[80]="GPPD_5.h5";
  char filename[256];
  const char* ch_test_data = "NeXus ><}&{'\\&\" Data";
  int64_t grossezahl[4];
  char path[512];


  // read nexus file
  if (NXopen (nxFile, NXACC_RDWR,&fileid) != NX_OK) {cout << " file is reopening" <<'\n'; }
  if(NXinquirefile(fileid,filename,256) != NX_OK) {cout<<" get the file name"<<endl;}
  printf("NXinquirefile found: %s\n", filename);
   NXgetattrinfo (fileid, &i);
  if (i > 0) {
     printf ("Number of global attributes: %d\n", i);
  }
    do { 
     attr_status = NXgetnextattr (fileid, name, NXdims, &NXtype);
     if (attr_status == NX_ERROR) {cout << " attr_status is NX_ERROR" <<'\n'; }
     if (attr_status == NX_OK) {
        switch (NXtype) {
           case NX_CHAR:
              NXlen = sizeof (char_buffer);
              if (NXgetattr (fileid, name, char_buffer, &NXlen, &NXtype) 
                  != NX_OK) {cout << " NXgetattr is NX_ERROR" <<'\n'; }
                if ( strcmp(name, "file_time") &&
                     strcmp(name, "HDF_version") &&
                     strcmp(name, "HDF5_Version") &&
                     strcmp(name, "XML_version") )
                {
                 printf (" %s = %s\n", name, char_buffer);
                }
              break;
        }
     }
  } while (attr_status == NX_OK);  
// 
  if (NXopengroup (fileid, "entry", "NXentry") != NX_OK) {cout << "NXopengroup is NX_ERROR" <<'\n'; }
  NXgetattrinfo(fileid,&i);
  printf("Number of group attributes: %d\n", i);
  if(NXgetpath(fileid,path,512) != NX_OK){cout << "NXgetpath is NX_ERROR" <<'\n'; }
  printf("NXentry path %s\n", path);
  
  do { 
     attr_status = NXgetnextattr (fileid, name, NXdims, &NXtype);
     if (attr_status == NX_ERROR) {cout << "attr_status is NX_ERROR" <<'\n'; }
  printf("NXentry path %s\n", path);
     if (attr_status == NX_OK) {
        switch (NXtype) {
           case NX_CHAR:
              NXlen = sizeof (char_buffer);
              if (NXgetattr (fileid, name, char_buffer, &NXlen, &NXtype) 
                  != NX_OK) 
                 printf ("   %s = %s\n", name, char_buffer);
        }
     }
  } while (attr_status == NX_OK);
  if (NXgetgroupinfo (fileid, &i, group_name, class_name) != NX_OK) {cout << "NXgetgroupinfo is NX_ERROR" <<'\n';}
     printf ("Group: %s(%s) contains %d items\n", group_name, class_name, i);

    do {
     entry_status = NXgetnextentry (fileid, name, char_class, &NXtype);
     if (entry_status == NX_ERROR) {cout << "attr_status is NX_ERROR" <<'\n'; }
     if (strcmp(char_class,"SDS") != 0) {
        if (entry_status != NX_EOD) {
           printf (" Subgroup: %s(%s)\n", name, char_class);
           if (NXopengroup (fileid, "data", "NXdata") != NX_OK) {printf("Got it !!!!!\n"); }
           //if (NXopendata(fileid, name) != NX_OK) {cout << "NXopendata is NX_ERROR" <<'\n'; }
            //if(NXgetpath(fileid,path,512) != NX_OK){cout << "NXgetpath is NX_ERROR" <<'\n'; }
            printf("Data path %s\n", path);

           entry_status = NX_OK;
        }
     } else {
        cout << "I'm running !!!!!" << endl;
        if (entry_status == NX_OK) {
           if (NXopendata(fileid, name) != NX_OK) {cout << "NXopendata is NX_ERROR" <<'\n'; }
            if(NXgetpath(fileid,path,512) != NX_OK){cout << "NXgetpath is NX_ERROR" <<'\n'; }
            printf("Data path %s\n", path);
            if (NXgetinfo (fileid, &NXrank, NXdims, &NXtype) != NX_OK) {cout << "attr_status is NX_ERROR" <<'\n'; }
                 printf ("   %s(%d)", name, NXtype);
              if (NXmalloc ((void **) &data_buffer, NXrank, NXdims, NXtype) != NX_OK) {cout << "attr_status is NX_ERROR" <<'\n'; }
              n = 1;
              for(k=0; k<NXrank; k++)
              {
                  n *= NXdims[k];
              }
              if (NXtype == NX_CHAR) {
                 if (NXgetdata (fileid, data_buffer) != NX_OK) {cout << "NXgetdata is NX_ERROR" <<'\n'; }
                    print_data (" = ", data_buffer, NXtype, n);
              } else if (NXtype != NX_FLOAT32 && NXtype != NX_FLOAT64) {
                 if (NXgetdata (fileid, data_buffer) != NX_OK) {cout << "NXgetdata is NX_ERROR" <<'\n'; }
                    print_data (" = ", data_buffer, NXtype, n);
              }

           if (NXclosedata (fileid) != NX_OK) {cout << "attr_status is NX_ERROR" <<'\n'; }
           if (NXfree ((void **) &data_buffer) != NX_OK) {cout << "attr_status is NX_ERROR" <<'\n'; }
        }
     }
  } while (entry_status == NX_OK);  

*/

// write file
  // if(NXopen (nxFile, NXACC_CREATE5, &fileid)) {cout << " file is opening" <<'\n';}
  // if (NXreopen (fileid, &clone_fileid) == NX_OK) {cout << " file is reopening" <<'\n'; }
  // NXsetnumberformat(fileid,NX_FLOAT32,"%9.3f");
  
  // NXmakegroup (fileid, "entry", "NXentry")  ;
  // NXopengroup (fileid, "entry", "NXentry")  ;
  // NXputattr(fileid,"hugo","namenlos",strlen("namenlos"), NX_CHAR)  ;
  // NXputattr(fileid,"cucumber","passion",strlen("passion"), NX_CHAR)  ;
  //    NXlen = strlen(ch_test_data);
  //    NXmakedata (fileid, "ch_data", NX_CHAR, 1, &NXlen)  ;
  //    NXopendata (fileid, "ch_data")  ;
  //       NXputdata (fileid, ch_test_data)  ;
  //    NXclosedata (fileid)  ;
  //    NXmakedata (fileid, "c1_data", NX_CHAR, 2, array_dims)  ;
  //    NXopendata (fileid, "c1_data")  ;
  //       NXputdata (fileid, c1_array)  ;
  //    NXclosedata (fileid)  ;
  //    NXmakedata (fileid, "i1_data", NX_INT8, 1, &array_dims[1])  ;
  //    NXopendata (fileid, "i1_data")  ;
  //       NXputdata (fileid, i1_array)  ;
  //    NXclosedata (fileid)  ;
  //    NXmakedata (fileid, "i2_data", NX_INT16, 1, &array_dims[1])  ;
  //    NXopendata (fileid, "i2_data")  ;
  //       NXputdata (fileid, i2_array)  ;
  //    NXclosedata (fileid)  ;
  //    NXmakedata (fileid, "i4_data", NX_INT32, 1, &array_dims[1])  ;
  //    NXopendata (fileid, "i4_data")  ;
  //       NXputdata (fileid, i4_array)  ;
  //    NXclosedata (fileid)  ;
  //    NXcompmakedata (fileid, "r4_data", NX_FLOAT32, 2, array_dims,NX_COMP_LZW,chunk_size)  ;
  //    NXopendata (fileid, "r4_data")  ;
  //       NXputdata (fileid, r4_array)  ;
  //    NXclosedata (fileid)  ;
  //    NXmakedata (fileid, "r8_data", NX_FLOAT64, 2, array_dims)  ;
  //    NXopendata (fileid, "r8_data")  ;
  //       slab_start[0] = 4; slab_start[1] = 0; slab_size[0] = 1; slab_size[1] = 4;
      
  //       NXputslab (fileid, (double*)r8_array + 16, slab_start, slab_size)  ;
  //       slab_start[0] = 0; slab_start[1] = 0; slab_size[0] = 4; slab_size[1] = 4;
  //       NXputslab (fileid, r8_array, slab_start, slab_size)  ;
  //       NXputattr (fileid, "ch_attribute", ch_test_data, strlen (ch_test_data), NX_CHAR)  ;
  //       i = 42;
  //       NXputattr (fileid, "i4_attribute", &i, 1, NX_INT32)  ;
  //       r = 3.14159265;
  //       NXputattr (fileid, "r4_attribute", &r, 1, NX_FLOAT32)  ;
  //       NXgetdataID (fileid, &dlink)  ;
  //    NXclosedata (fileid)  ;
  //    dims[0] = 4;
     
  //    NXmakegroup (fileid, "data", "NXdata")  ;
  //    NXopengroup (fileid, "data", "NXdata")  ;
  //       NXmakelink (fileid, &dlink)  ;
  //       dims[0] = 100;
  //       dims[1] = 20;
  //       for(i = 0; i < 100; i++)
  //           {
  //           for(j = 0; j < 20; j++)
  //              {
  //                comp_array[i][j] = i;
  //              }
  //           }
  //       cdims[0] = 20;
  //       cdims[1] = 20;
  //       NXcompmakedata (fileid, "comp_data", NX_INT32, 2, dims, NX_COMP_LZW, cdims)  ;
  //       NXopendata (fileid, "comp_data")  ;
  //          NXputdata (fileid, comp_array)  ;
  //       NXclosedata (fileid)  ;  
  //       NXflush (&fileid)  ;

  //       NXmakedata (fileid, "flush_data", NX_INT32, 1, unlimited_dims)  ;
  //       slab_size[0] = 1;
  //       for (i = 0; i < 7; i++)
  //           {
  //             slab_start[0] = i;
  //             NXopendata (fileid, "flush_data")  ;
  //               NXputslab (fileid, &i, slab_start, slab_size)  ;
  //               NXflush (&fileid)  ;
  //       }
  //    NXclosegroup (fileid)  ;
  //    NXmakegroup (fileid, "sample", "NXsample");
  //    NXopengroup (fileid, "sample", "NXsample");
  //       NXlen = 12;
  //       NXmakedata (fileid, "ch_data", NX_CHAR, 1, &NXlen)  ;
  //       NXopendata (fileid, "ch_data")  ;
  //          NXputdata (fileid, "NeXus sample")  ;
  //       NXclosedata (fileid)  ;
  //       NXgetgroupID (fileid, &glink)  ;
  //    NXclosegroup (fileid)  ;
  //     NXclosegroup (fileid)  ;
  //     NXmakegroup (fileid, "link", "NXentry")  ;
  //     NXopengroup (fileid, "link", "NXentry")  ;
  //        NXmakelink (fileid, &glink)  ;
  //        NXmakenamedlink (fileid,"renLinkGroup", &glink)  ;
  //        NXmakenamedlink (fileid, "renLinkData", &dlink)  ;
  //    NXclosegroup (fileid)  ;
   //   NXclose (&fileid)  ;


  // mine>   

  printf("ADnED::printPixelMap. Det: %d\n", det);
  if ((m_PixelMapSize[det] > 0) && (p_PixelMap[det])) {
    printf("m_PixelMapSize[%d]: %d\n", det, m_PixelMapSize[det]);
    // for (epicsUInt32 index=0; index<m_PixelMapSize[det]; ++index) {
    //   printf("p_PixelMap[%d][%d]: %d\n", det, index, (p_PixelMap[det])[index]);
    // }
  } else {
    printf("No pixel mapping loaded.\n");
  }
}

/**
 * For debug purposes, print TOF transformation array to stdout.
 * @param det The detector number (1 based)
 */
void ADnED::printTofTrans(epicsUInt32 det)
{ 
  printf("ADnED::printTofTrans. Det: %d\n", det);
  p_Transform[det]->printParams();
}

/**
 * Check if a parameter is a ADnEDTransform related one, and return it.
 * If it doesn't match, return -1.
 * @param asynParam - The asyn parameter to test
 * @param transIndex - Return value of the matching ADnEDTransform index
 * @return true=match, false=no match
 */
bool ADnED::matchTransFile(int asynParam, epicsUInt32 &transIndex)
{
  //Unfortunately can't use switch statement here
  if (asynParam == ADnEDDetTOFTransFile0Param) {
    transIndex = 0;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransFile1Param) {
    transIndex = 1;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransFile2Param) {
    transIndex = 2;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransFile3Param) {
    transIndex = 3;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransFile4Param) {
    transIndex = 4;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransFile5Param) {
    transIndex = 5;
    return true;
  }

  return false;
}

/**
 * Check if a parameter is a ADnEDTransform related one, and return it.
 * If it doesn't match, return -1.
 * @param asynParam - The asyn parameter to test
 * @param transIndex - Return value of the matching ADnEDTransform index
 * @return true=match, false=no match
 */
bool ADnED::matchTransInt(int asynParam, epicsUInt32 &transIndex)
{
  //Unfortunately can't use switch statement here
  if (asynParam == ADnEDDetTOFTransInt0Param) {
    transIndex = 0;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransInt1Param) {
    transIndex = 1;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransInt2Param) {
    transIndex = 2;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransInt3Param) {
    transIndex = 3;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransInt4Param) {
    transIndex = 4;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransInt5Param) {
    transIndex = 5;
    return true;
  }

  return false;
}

/**
 * Check if a parameter is a ADnEDTransform related one, and return it.
 * If it doesn't match, return -1.
 * @param asynParam - The asyn parameter to test
 * @param transIndex - Return value of the matching ADnEDTransform index
 * @return true=match, false=no match
 */
bool ADnED::matchTransFloat(int asynParam, epicsUInt32 &transIndex)
{
  //Unfortunately can't use switch statement here
  if (asynParam == ADnEDDetTOFTransFloat0Param) {
    transIndex = 0;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransFloat1Param) {
    transIndex = 1;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransFloat2Param) {
    transIndex = 2;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransFloat3Param) {
    transIndex = 3;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransFloat4Param) {
    transIndex = 4;
    return true;
  }
  if (asynParam == ADnEDDetTOFTransFloat5Param) {
    transIndex = 5;
    return true;
  }

  return false;
}

/**
 * Check the newly loaded pixel map array. If any of the values are
 * outside the pre-defined range for that detector, then clear the array,
 * and return asynError.
 * @param det The detector number (1 based)
 */
asynStatus ADnED::checkPixelMap(epicsUInt32 det)
{ 
  asynStatus status = asynSuccess;
  const char* functionName = "ADnED::checkPixelMap";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  int detSizeValue = 0;
  getIntegerParam(det, ADnEDDetPixelNumSizeParam, &detSizeValue);
  //printf("the detSizeValue" )
   cout<< "it's well for soul. the p_PixelMap[det]" << *p_PixelMap[det]<<endl; 
  if ((m_PixelMapSize[det] > 0) && (p_PixelMap[det])) {
    for (epicsUInt32 index=0; index<m_PixelMapSize[det]; ++index) {

      if ((p_PixelMap[det])[index] > static_cast<epicsUInt32>(detSizeValue)) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                  "%s Det: %d. Pixel ID %d in mapping array was out of allowed range. Must be less than %d.\n", 
                  functionName, det, index, detSizeValue);
        memset(p_PixelMap[det], 0, m_PixelMapSize[det]);
        m_PixelMapSize[det] = 0;
        status = asynError;
      }
    }
  } else {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s No pixel mapping loaded.\n", functionName);
    status = asynError;
  }

  return status;
}

/**
 * Reset the TOF array for a specific detector
 * @param det The detector number (1 based)
 */
void ADnED::resetTOFArray(epicsUInt32 det)
{ 
  int tofStart = 0;
  epicsUInt32 *p_tof;
  
  printf("ADnED::resetTOFArray. det: %d\n", det);

  if (m_tofMax > 0) {
    getIntegerParam(det, ADnEDDetNDArrayTOFStartParam, &tofStart);
    if ((p_Data != NULL) && (tofStart > 0)) {


      p_tof = p_Data + tofStart;
      memset(p_tof, 0, (m_tofMax+1)*sizeof(epicsUInt32));
    }
  } else {
    printf("ADnED::resetTOFArray. Need to alloc memory first.\n");
  }
}

/**
 * Event handler callback for monitor
 */
void ADnED::eventHandler(shared_ptr<epics::pvData::PVStructure> const &pv_struct, epicsUInt32 channelID)
{
  int eventDebug = 0;
  bool eventUpdate = false;
  bool newPulse = false;
  epicsFloat64 updatePeriod = 0.0;
  int numMissingPackets = 0;
  double timeDiffSecs = 0.0;
  epicsUInt32 eventRate = 0;
  int numChanOrDet = 0;
  const char* functionName = "ADnED::eventHandler";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Event Handler. Channel ID %d\n", functionName, channelID);

  /* If we are paused, still do timestamp and seq number checks, 
     otherwise we will see missing packets.*/
  int paused = 0;
  getIntegerParam(ADnEDPauseParam, &paused);

  /* Get the time and decide if we update the PVs.*/
  lock();
  getDoubleParam(ADnEDEventUpdatePeriodParam, &updatePeriod);
  epicsTimeGetCurrent(&m_nowTime);
  m_nowTimeSecs = m_nowTime.secPastEpoch + (m_nowTime.nsec / 1.e9);
  if ((m_nowTimeSecs - m_lastTimeSecs) < (updatePeriod / 1000.0)) {
    eventUpdate = false;
  } else {
    eventUpdate = true;
    timeDiffSecs = m_nowTimeSecs - m_lastTimeSecs;
    m_lastTimeSecs = m_nowTimeSecs;
  }
  unlock();

  //Sanity check on channelID
  if (channelID >= static_cast<epicsUInt32>(s_ADNED_MAX_CHANNELS)) { //0 based
    if (eventUpdate) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s Invalid channel ID %d.\n", functionName, channelID);
    }
    return;
  }

  int numChan = 0;
  getIntegerParam(ADnEDNumChannelsParam, &numChan);
  if (numChan > s_ADNED_MAX_CHANNELS) {
    numChan = s_ADNED_MAX_CHANNELS;
  }
  int numDet = 0;
  getIntegerParam(ADnEDNumDetParam, &numDet);
  if (numDet > s_ADNED_MAX_DETS) {
    numDet = s_ADNED_MAX_DETS;
  }
  getIntegerParam(ADnEDEventDebugParam, &eventDebug);
  for (int det=1; det<=numDet; det++) {
    getIntegerParam(det, ADnEDDetPixelNumStartParam, &m_detStartValues[det]);
    getIntegerParam(det, ADnEDDetPixelNumEndParam, &m_detEndValues[det]);
    getIntegerParam(det, ADnEDDetPixelNumSizeParam, &m_detSizeValues[det]);
    getIntegerParam(det, ADnEDDetNDArrayStartParam, &m_NDArrayStartValues[det]);
    getIntegerParam(det, ADnEDDetNDArrayTOFStartParam, &m_NDArrayTOFStartValues[det]);
    //These two params are used to filter events based on a TOF ROI
    getIntegerParam(det, ADnEDDetTOFROIStartParam, &m_detTOFROIStartValues[det]);
    getIntegerParam(det, ADnEDDetTOFROISizeParam, &m_detTOFROISizeValues[det]);
    getIntegerParam(det, ADnEDDetTOFROIEnableParam, &m_detTOFROIEnabled[det]);
    //Pixel ID mapping
    getIntegerParam(det, ADnEDDetPixelMapEnableParam, &m_detPixelMappingEnabled[det]);
    //TOF Transformation
    getIntegerParam(det, ADnEDDetTOFTransTypeParam, &m_detTOFTransType[det]);
    getDoubleParam(det, ADnEDDetTOFTransOffsetParam, &m_detTOFTransOffset[det]);
    getDoubleParam(det, ADnEDDetTOFTransScaleParam, &m_detTOFTransScale[det]);
    //Pixel ID XY filter
    getIntegerParam(det, ADnEDDetPixelROIStartXParam, &m_detPixelROIStartX[det]);
    getIntegerParam(det, ADnEDDetPixelROIStartYParam, &m_detPixelROIStartY[det]);
    getIntegerParam(det, ADnEDDetPixelROISizeXParam, &m_detPixelROISizeX[det]);
    getIntegerParam(det, ADnEDDetPixelROISizeYParam, &m_detPixelROISizeY[det]);
    getIntegerParam(det, ADnEDDetPixelSizeXParam, &m_detPixelSizeX[det]);
    getIntegerParam(det, ADnEDDetPixelROIEnableParam, &m_detPixelROIEnable[det]);

    if (m_detPixelROISizeX[det] <= 0) {
      if (eventUpdate) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s Invalid Pixel ROI Size X.\n", functionName);
      }
      return;
    } 
  }

  //Compare timeStamp to last timeStamp to detect a new pulse.
  lock();
  ++m_seqCounter[channelID];  
  try {
    if (!m_PVTimeStamp.attach(pv_struct->getSubField<epics::pvData::PVStructure>(ADNED_PV_TIMESTAMP))) {
      if (eventUpdate) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s Failed to attach PVTimeStamp.\n", functionName);
      }
      unlock();
      return;
    }
    m_PVTimeStamp.get(m_TimeStamp[channelID]);
    //Only use channel ID 0 to integrate the proton charge
    if (channelID == 0) {
      if (m_TimeStampLast[0] != m_TimeStamp[0]) {
        newPulse = true;
      }
    }
    if (m_TimeStampLast[channelID] > m_TimeStamp[channelID]) {
      if (eventUpdate) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s Backwards timeStamp detected on channel %d.\n", functionName, channelID);
      }
      setIntegerParam(channelID, ADnEDBadTimeStampParam, 1);
      unlock();
      return;
    }
    m_TimeStampLast[channelID].put(m_TimeStamp[channelID].getSecondsPastEpoch(), m_TimeStamp[channelID].getNanoseconds());
    
    m_seqID[channelID] = static_cast<epicsUInt32>(m_TimeStamp[channelID].getUserTag());
    //Detect missing packets
    if (static_cast<epicsInt32>(m_lastSeqID[channelID]) != -1) {
      if (m_seqID[channelID] != m_lastSeqID[channelID]+1) {
        setIntegerParam(channelID, ADnEDSeqIDMissingParam, m_lastSeqID[channelID]+1);
        getIntegerParam(channelID, ADnEDSeqIDNumMissingParam, &numMissingPackets);
        setIntegerParam(channelID, ADnEDSeqIDNumMissingParam, numMissingPackets+(m_seqID[channelID]-m_lastSeqID[channelID]+1));
        if (eventUpdate) {
          asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s: Missing seq ID numbers on channel %d.\n", functionName, channelID);
        }
      }
    }
    m_lastSeqID[channelID] = m_seqID[channelID];
  } catch (std::exception &e)  {
    if (eventUpdate) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s: Failed to deal with time stamp objects. Exception: %s\n", 
                functionName, e.what());
    }
    unlock();
    return;
  }
  unlock();
  
  
  /* the timestamp data */
  //epics::pvData::PVTimeStamp timestampPtr = pv_struct->getStructureField(ADNED_PV_TIMESTAMP);
  epics::pvData::PVDoublePtr pChargePtr = pv_struct->getSubField<epics::pvData::PVDouble>(ADNED_PV_PCHARGE);  
  //cout << "the chargeData is ::" << pChargePtr <<endl;
  if (!pChargePtr) {
    if (eventUpdate) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s No valid pCharge found.\n", functionName);
    }
    return;
  }

  if ((p_Data == NULL) || (m_dataMaxSize == 0)) {
    return;
  }

  // obtain the pixel smf tof data pointer  
  epics::pvData::PVUIntArrayPtr pixelsPtr = pv_struct->getSubField<epics::pvData::PVUIntArray>(ADNED_PV_PIXELS);
  epics::pvData::PVUIntArrayPtr tofPtr = pv_struct->getSubField<epics::pvData::PVUIntArray>(ADNED_PV_TOF);

  if (pixelsPtr && tofPtr) {
    
    /* the pixel data and tof data  */
    epics::pvData::uint32 pixelsLength = pixelsPtr->getLength();
    epics::pvData::shared_vector<const epics::pvData::uint32> pixelsData = pixelsPtr->view();
    
    epics::pvData::uint32 tofLength = tofPtr->getLength();
    epics::pvData::shared_vector<const epics::pvData::uint32> tofData = tofPtr->view();
    
    /*  the pulse_ID data    */
    epics::pvData::PVIntPtr pulseIDPtr = pv_struct->getSubField<epics::pvData::PVInt>(ADNED_PV_SEQ);
    int pulseID = pulseIDPtr->get();
    
   
    //new stucture para 
    int slab_start[1], slab_size[1];
    size_t everyPulsePixelData[pixelsLength];
    size_t everyPulseTOFData[pixelsLength];
    size_t everyPulseEventNum[1];
    everyPulseEventNum[0] = pixelsLength;

    // size_t pixelsDataCopy[pixelsLength];
    // size_t tofDataCopy[pixelsLength];
    // size_t pulseIDCopy[1];
    // pulseIDCopy[0]=pulseID;
    /* the pixel raw data array */
    for(int list_num_pixel = 0; list_num_pixel<pixelsLength; list_num_pixel++){
          everyPulsePixelData[list_num_pixel] = pixelsData[list_num_pixel];
          everyPulseTOFData[list_num_pixel] = tofData[list_num_pixel];
        }  
    int dim = static_cast<int>(pixelsLength);
        
    /* the fullname */
    getStringParam(ADnEDHdfFullFileNameParam, sizeof(hdfFullFileName), hdfFullFileName); 
    getStringParam(ADnEDHdfFileTemplateParam, sizeof(hdfFileTemplate), hdfFileTemplate); 

    getStringParam(ADnEDHdfHV1MessageParam, sizeof(hdfHV1Message), hdfHV1Message);
    getStringParam(ADnEDHdfHV2MessageParam, sizeof(hdfHV1Message), hdfHV2Message);
    getStringParam(ADnEDHdfGasContentMessageParam, sizeof(hdfGasContentMessage), hdfGasContentMessage);
    
    /*Flag for storage */
    getIntegerParam(ADnEDHdfPauseParam, &storageFlag);
    getIntegerParam(ADnEDHdfNumPulsePerFileParam, &time_per_file);
    pulse_num_per_file = time_per_file*25;
    /* the write mode : single , capture , stream  */
    if(storageFlag==0) {getIntegerParam(ADnEDHdfWriteModeParam, &hdfFileWriteMode); 
            setStringParam(ADnEDHdfStatusMessageParam, "Storing Data");
          } 
    else{hdfFileWriteMode = 9;
       setStringParam(ADnEDHdfStatusMessageParam, "watting ^_+_^");}

    getStringParam(ADnEDHdfFilePathParam, sizeof(hdfFilePath), hdfFilePath); 

    switch(hdfFileWriteMode){
      case 0:
        // NXclose (&capture_file_id);//
        // setIntegerParam(ADnEDSingleHdfFileNumberParam, single_file_num);

        // strcat(hdfFullFileName,hdfFileTemplate);

        // sprintf(single_hdf_file_name, hdfFullFileName, single_file_num);
        // strcat(hdfFilePath,single_hdf_file_name);
        // //cout <<  "hdfFilePath2:::" <<hdfFilePath <<endl;
        // nxstat = NXopen (hdfFilePath, NXACC_CREATE5, &file_id);
        // NXmakegroup (file_id, "GPPD Monitor", "NXentry");
        // NXopengroup (file_id, "GPPD Monitor", "NXentry");

        //  /* out the pulse id data  */       
        //   NXmakedata (file_id, "pulse_ID", NX_UINT64, 1 , &pulse_id_dim);
        //   NXopendata (file_id, "pulse_ID");
        //   NXputdata (file_id, pulseIDCopy);
        //   //NXputattr (file_id, "units", " ", 7, NX_CHAR);
        //   NXclosedata (file_id); 
          
        //     /* Output the pxiel ID */
        //   NXmakedata (file_id, "pixel_ID", NX_UINT64, 1 , &dim);
        //   NXopendata (file_id, "pixel_ID");
        //   NXputdata (file_id, pixelsDataCopy);
        //   //NXputattr (file_id, "units", " ", 7, NX_CHAR);
        //   NXclosedata (file_id);
          
        //   NXmakedata (file_id, "time_of_flight", NX_UINT64, 1, &dim);
        //   NXopendata (file_id, "time_of_flight");
        //   NXputdata (file_id, tofDataCopy);
        //   NXputattr (file_id, "units", "microseconds", 12, NX_CHAR);
        //   NXclosedata (file_id);
          
        //   NXclosegroup (file_id);
        // NXclose (&file_id); 
        // single_file_num++;
        break;

      // capture mode
      case 1:

        if(capture_group_num % pulse_num_per_file == 0){
        setIntegerParam(ADnEDCaptureHdfFileNumberParam, capture_file_num);
        strcat(hdfFullFileName,hdfFileTemplate);
        sprintf(single_hdf_file_name, hdfFullFileName, capture_file_num);
        strcat(hdfFilePath,single_hdf_file_name);
        
        time_t t;
        cout << " the current time is " << time(&t) << "    "  <<asctime(localtime(&t))<<endl;
       
        nxstat = NXopen (hdfFilePath, NXACC_CREATE5, &capture_file_id);
        if (nxstat == NX_ERROR) {
             asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
            "Error cannot open HDF file\n");
             }

        NXmakegroup (capture_file_id, "entry", "NXentry")  ;
        NXopengroup (capture_file_id, "entry", "NXentry")  ;   

        NXmakedata (capture_file_id, "GEM_HV_1", NX_CHAR, 1, &common_aChar_len);
        NXopendata (capture_file_id, "GEM_HV_1");
        NXputdata (capture_file_id, hdfHV1Message);
        NXclosedata (capture_file_id);
        
        NXmakedata (capture_file_id, "GEM_HV_2", NX_CHAR, 1, &common_aChar_len);
        NXopendata (capture_file_id, "GEM_HV_2");
        NXputdata (capture_file_id, hdfHV2Message);
        NXclosedata (capture_file_id);

        NXmakedata (capture_file_id, "GEM_gas", NX_CHAR, 1, &common_aChar_len);
        NXopendata (capture_file_id, "GEM_gas");
        NXputdata (capture_file_id, hdfGasContentMessage);
        NXclosedata (capture_file_id);
        capture_file_num++;

        NXmakegroup (capture_file_id, "data", "NXdata");
        NXopengroup (capture_file_id, "data", "NXdata");
        
        NXmakedata (capture_file_id, "event_pixel_id", NX_INT64, 1, unlimited_dims);
        NXopendata (capture_file_id, "event_pixel_id");
        NXclosedata (capture_file_id);
        //  NXflush (&capture_file_id);

        NXmakedata (capture_file_id, "event_tof_of_flight", NX_INT64, 1, unlimited_dims);
        NXopendata (capture_file_id, "event_tof_of_flight");
        NXclosedata (capture_file_id);
           // NXflush (&capture_file_id);

        NXmakedata (capture_file_id, "event_num_per_pulse", NX_INT64, 1, unlimited_dims);
        NXopendata (capture_file_id, "event_num_per_pulse");
        NXclosedata (capture_file_id);

        }
              
            NXopenpath (capture_file_id,"/entry/data");
            NXopendata (capture_file_id, "event_pixel_id");
            slab_start[0] = start_every_slap; slab_size[0] = pixelsLength; 
            cout << start_every_slap << "      "  << pixelsLength << endl;
            NXputslab (capture_file_id, everyPulsePixelData, slab_start,slab_size);
            NXclosedata (capture_file_id);
            NXflush (&capture_file_id);


            NXopenpath (capture_file_id,"/entry/data");
            NXopendata (capture_file_id, "event_tof_of_flight");
            NXputslab (capture_file_id, everyPulseTOFData, slab_start,slab_size);
            NXputattr (capture_file_id, "units", "microseconds", 12, NX_CHAR);
            NXclosedata (capture_file_id);
            NXflush (&capture_file_id);

            

            NXopenpath (capture_file_id,"/entry/data");
            NXopendata (capture_file_id, "event_num_per_pulse");
            slab_start[0] = capture_group_num%pulse_num_per_file; slab_size[0] = 1;
            NXputslab (capture_file_id, everyPulseEventNum, slab_start,slab_size);
            NXclosedata (capture_file_id);
            NXflush (&capture_file_id);

          NXclosegroup (capture_file_id);
          start_every_slap += pixelsLength;
          if(capture_group_num % pulse_num_per_file == (pulse_num_per_file-1))
           {
              NXclosegroup (capture_file_id);
              NXclose (&capture_file_id);
              start_every_slap = 0;
            }
          capture_group_num++;
         
          break;

      // stream mode 
      case 2:
        cout<< "I'm running::: " << stream_group_num <<endl;
        if(stream_group_num ==0)
        { 
          strcat(hdfFullFileName,".h5");
          strcat(hdfFilePath,hdfFullFileName);
          nxstat = NXopen (hdfFilePath, NXACC_CREATE5, &stream_file_id);
          
          if (nxstat == NX_ERROR) {
             asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
            "Error cannot open file NXfile.h5\n");
             }
           NXmakegroup (stream_file_id, "entry", "NXentry");
           NXopengroup (stream_file_id, "entry", "NXentry");   

           NXmakedata (stream_file_id, "GEM_HV_1", NX_CHAR, 1, &common_aChar_len);
           NXopendata (stream_file_id, "GEM_HV_1");
           NXputdata (stream_file_id, hdfHV1Message);
           NXclosedata (stream_file_id);

           NXmakedata (stream_file_id, "GEM_HV_2", NX_CHAR, 1, &common_aChar_len);
           NXopendata (stream_file_id, "GEM_HV_2");
           NXputdata (stream_file_id, hdfHV2Message);
           NXclosedata (stream_file_id);

           NXmakedata (stream_file_id, "GEM_gas", NX_CHAR, 1, &common_aChar_len);
           NXopendata (stream_file_id, "GEM_gas");
           NXputdata (stream_file_id, hdfGasContentMessage);
           NXclosedata (stream_file_id);

            NXmakegroup (stream_file_id, "data", "NXdata");
            NXopengroup (stream_file_id, "data", "NXdata");

            NXmakedata (stream_file_id, "event_pixel_id", NX_INT64, 1, unlimited_dims);
            NXopendata (stream_file_id, "event_pixel_id");
            NXclosedata (stream_file_id);
          //  NXflush (&stream_file_id);

            NXmakedata (stream_file_id, "event_tof_of_flight", NX_INT64, 1, unlimited_dims);
            NXopendata (stream_file_id, "event_tof_of_flight");
            NXclosedata (stream_file_id);
          

          NXmakedata (stream_file_id, "event_num_per_pulse", NX_INT64, 1, unlimited_dims);
          NXopendata (stream_file_id, "event_num_per_pulse");
          NXclosedata (stream_file_id);

          // NXflush (&stream_file_id);
            
         }

        /*Output the pxiel ID */
        NXopenpath (stream_file_id,"/entry/data");
        NXopendata (stream_file_id, "event_pixel_id");
        slab_start[0] = start_every_slap;slab_size[0] = pixelsLength; 
        NXputslab (stream_file_id, everyPulsePixelData, slab_start,slab_size);
        NXclosedata (stream_file_id);
        NXflush (&stream_file_id);
      
        NXopenpath (stream_file_id,"/entry/data");
        NXopendata (stream_file_id, "event_tof_of_flight");   
        NXputslab (stream_file_id, everyPulseTOFData, slab_start,slab_size);
        NXputattr (stream_file_id, "units", "microseconds", 12, NX_CHAR);
        NXclosedata (stream_file_id);
        NXflush (&stream_file_id);

        NXopenpath (stream_file_id,"/entry/data");
        NXopendata (stream_file_id, "event_num_per_pulse");
        slab_start[0] = stream_group_num; slab_size[0] = 1;
        NXputslab (stream_file_id, everyPulseEventNum, slab_start,slab_size);
        NXclosedata (stream_file_id);
        NXflush (&stream_file_id);

        NXclosegroup (stream_file_id);   
        start_every_slap += static_cast<int>(pixelsLength);
        stream_group_num++ ;
        break;

       
    }
  
    /* Output time channels */
    if (pixelsLength != tofLength) {
      if (eventUpdate) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s pixelsLength != tofLength.\n", functionName);
      }
      return;
    }

    //Count events to calculate event rate.
    m_eventsSinceLastUpdate += pixelsLength;

    lock();

    if (!paused) {

    int mappedPixelIndex = 0;
    epicsFloat64 tof = 0.0;
    epicsUInt32 tofInt = 0;
    int plotType = 0;
    int tofBins = 0;
    epicsUInt32 x_pos = 0;
    epicsUInt32 y_pos = 0;
    int tofIndex = 0;
    for (size_t i=0; i<pixelsLength; ++i) {
      for (int det=1; det<=numDet; det++) {
        
        //Dtermine if this pixel ID is in this DET range.
        if ((pixelsData[i] >= static_cast<epicsUInt32>(m_detStartValues[det])) 
            && (pixelsData[i] <= static_cast<epicsUInt32>(m_detEndValues[det]))) {
          
          //Offset pixel ID here so this detector pixel ID range starts at 0
          mappedPixelIndex = pixelsData[i] - m_detStartValues[det];
          //cout << "the mappedPixelIndex is ::" << mappedPixelIndex <<"    "<<pixelsData[i]<< "   " <<m_detStartValues[det]<<endl;
          tof = static_cast<epicsFloat64>(tofData[i]);
          // cout << "the tof is ::" << tof << endl; 
          // 
          //If enabled, do TOF tranformation (to d-space for example).
          if (m_detTOFTransType[det] != 0) {
            tof = p_Transform[det]->calculate(m_detTOFTransType[det], mappedPixelIndex, tofData[i]);
            
            //Apply scale and offset. This is used to rebin into the available TOF array.
            if (m_detTOFTransScale[det] >=0) {
              tof = (tof * m_detTOFTransScale[det]) + m_detTOFTransOffset[det];
            }
          }

          //Do pixel ID mapping if enabled
          if (m_detPixelMappingEnabled[det]) {
            if ((m_PixelMapSize[det] > 0) && (p_PixelMap[det])) {
              mappedPixelIndex = (p_PixelMap[det])[pixelsData[i] - m_detStartValues[det]];
            }
          }

	  tofInt = static_cast<epicsUInt32>(floor(tof));
    // I'm here //   diffrence type
    //cout << "the tofInt is ::" << tofInt << endl;
	  //Read type of 2-D plot and TOF binning
	  getIntegerParam(det, ADnEDDet2DTypeParam, &plotType);
	  getIntegerParam(det, ADnEDDetTOFNumBinsParam, &tofBins);
    //cout << "the ADnEDDet2DTypeParam is ::" << ADnEDDet2DTypeParam << endl;
    //e ADnEDDet2DTypeParam is ::106
	  //cout << "the ADnEDDetTOFNumBinsParam is ::" << tofBins << endl;
    // the ADnEDDetTOFNumBinsParam is :: 1
    if (tofBins < 1) {
	    tofBins = 1;
	  } else if (static_cast<epicsUInt32>(tofBins) > m_tofMax) { 
	    tofBins = m_tofMax;
	  }

          //Integrate Pixel ID Data, optionally filtering on TOF ROI filter (for X/Y plot only).
          if (m_detTOFROIEnabled[det]) {
            if ((tof >= static_cast<epicsFloat64>(m_detTOFROIStartValues[det])) 
                && (tof < static_cast<epicsFloat64>(m_detTOFROIStartValues[det] + m_detTOFROISizeValues[det]))) {
              p_Data[m_NDArrayStartValues[det]+mappedPixelIndex]++;

            }
          } else { //No TOF ROI filter enabled. Choose which 2-D plot to produce.
	    if (static_cast<epicsUInt32>(plotType) == s_ADNED_2D_PLOT_XY) {
	      //Standard X/Y plot
	      p_Data[m_NDArrayStartValues[det]+mappedPixelIndex]++;
       //<mine >  here stasic  the XY data
       // cout << m_NDArrayStartValues[det]+mappedPixelIndex<< " **** " <<  p_Data[m_NDArrayStartValues[det]+mappedPixelIndex]++<<endl;

	    } else { 
	      if ((tof <= m_tofMax) && (tof >= 0)) {
		if (static_cast<epicsUInt32>(plotType) == s_ADNED_2D_PLOT_XTOF) {
		  // X/TOF plot
		  x_pos = mappedPixelIndex % m_detPixelSizeX[det]; 
		  tofIndex = (x_pos * tofBins) + int(floor(tof / (m_tofMax / tofBins))); 
		} else if (static_cast<epicsUInt32>(plotType) == s_ADNED_2D_PLOT_YTOF) {
		  // Y/TOF plot
		  y_pos = int(floor(mappedPixelIndex / m_detPixelSizeX[det]));
		  tofIndex = (y_pos * tofBins) + int(floor(tof / (m_tofMax / tofBins)));
		} else if (static_cast<epicsUInt32>(plotType) == s_ADNED_2D_PLOT_PIXELIDTOF) {
		  // PixelID/TOF plot
		  tofIndex = (mappedPixelIndex * tofBins) + int(floor(tof / (m_tofMax / tofBins)));
		}
		if (tofIndex < (m_detSizeValues[det] - 1)) {
		  p_Data[m_NDArrayStartValues[det] + tofIndex]++;
		}
	      }
	    }
          }

          //Integrate TOF/D-Space, optionally filtering on Pixel ID X/Y ROI
          if ((tof <= m_tofMax) && (tof >= 0)) {
            if (m_detPixelROIEnable[det]) {
              //If pixel mapping is not enabled, this is meaningless, so just integrate as normal.
              if (!m_detPixelMappingEnabled[det]) { 
                p_Data[m_NDArrayTOFStartValues[det]+tofInt]++;
              } else {
                //Only integrate TOF if we are inside pixel ID XY ROI.
                //ROI is assumed to start from 0,0 (not from whatever is the pixel ID range). 
                //So we need to offset, but this has already been done by the pixel mapping above.
		if (m_detPixelSizeX[det] > 0) {
		  if (((mappedPixelIndex % m_detPixelSizeX[det]) >= m_detPixelROIStartX[det]) && 
		      ((mappedPixelIndex % m_detPixelSizeX[det]) < (m_detPixelROIStartX[det] + m_detPixelROISizeX[det]))) {
		    if ((mappedPixelIndex >= (m_detPixelROIStartY[det] * m_detPixelSizeX[det])) &&
			((mappedPixelIndex < ((m_detPixelROIStartY[det] + m_detPixelROISizeY[det]) * m_detPixelSizeX[det])))) {
		      p_Data[m_NDArrayTOFStartValues[det]+tofInt]++;
         
		    }
		  }
		}
              }
            } else {
              p_Data[m_NDArrayTOFStartValues[det]+tofInt]++;
            }
          }

          //Count events to calculate event rate
          m_detEventsSinceLastUpdate[det]++;
          //Count total events
          m_detTotalEvents[det]++;
        }

      }
      }
    
    if (newPulse) {
      m_pChargeInt += pChargePtr->get();
      ++m_pulseCounter;
    }

    }

    //Update params at slower rate.
    //There is only one timer shared between channel threads. So any of them could reset
    //the timer and be responsible for posting the param updates. This is why we post 
    //the updates for all the channels and detectors each time.
    if (eventUpdate) {
      //Channel params
      for (int chan=0; chan<numChan; ++chan) {
	setIntegerParam(chan, ADnEDSeqCounterParam, m_seqCounter[chan]);
	setIntegerParam(chan, ADnEDSeqIDParam, m_seqID[chan]);
      }      //Other params
      setIntegerParam(ADnEDPulseCounterParam, m_pulseCounter);
      eventRate = static_cast<epicsUInt32>(floor(m_eventsSinceLastUpdate/timeDiffSecs));
      setIntegerParam(ADnEDEventRateParam, eventRate);
      m_eventsSinceLastUpdate = 0;
      for (int det=1; det<=numDet; det++) {
        eventRate = static_cast<epicsUInt32>(floor(m_detEventsSinceLastUpdate[det]/timeDiffSecs));
        setIntegerParam(det, ADnEDDetEventRateParam, eventRate);
        m_detEventsSinceLastUpdate[det] = 0;
        setDoubleParam(det, ADnEDDetEventTotalParam, m_detTotalEvents[det]);
      }
      setDoubleParam(ADnEDPChargeParam, pChargePtr->get());
      setDoubleParam(ADnEDPChargeIntParam, m_pChargeInt);
      //Callbacks for channel and det related parameters (so start at 0 rather than 1)
      numChanOrDet = std::max(numChan, numDet);
      for (int det=0; det<=numChanOrDet; det++) {
        callParamCallbacks(det);
      }
      callParamCallbacks();
    }

    unlock();
  }

  if (eventDebug != 0) {
    cout << "channelID: " << channelID << endl;
    pv_struct->dumpValue(cout);
    cout << endl;
  }
  
}

/**
 * Allocate local storage for event handler. This is only done when any of the detector
 * sizes or TOF range has changed. It then publishes the start and end points of
 * the detector and TOF data in the NDArray object. 
 */
/*
void ADnED::saveHDF5StreamMode(epics::pvData::uint32 pixelsize, size_t single_pixel_data[pixelsize], size_t single_tof_data[pixelsize])
{
      pass;
}
*/

asynStatus ADnED::allocArray(void) 
{
  asynStatus status = asynSuccess;
  const char* functionName = "ADnED::allocArray";
  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s", functionName);

  if (m_dataAlloc != true) {
    //Nothing has changed
    return asynSuccess;
  }

  int numDet = 0;
  int detStart = 0;
  int detEnd = 0;
  int detSize = 0;
  int tofMax = 0;
  getIntegerParam(ADnEDNumDetParam, &numDet);
  getIntegerParam(ADnEDTOFMaxParam, &tofMax);
  m_tofMax = tofMax;

  if (numDet == 0) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s No detectors.\n", functionName);
    return asynError;
  }

  m_dataMaxSize = 0;

  for (int det=1; det<=numDet; det++) {
    
    getIntegerParam(det, ADnEDDetPixelNumStartParam, &detStart);
    getIntegerParam(det, ADnEDDetPixelNumEndParam, &detEnd);
    getIntegerParam(det, ADnEDDetPixelNumSizeParam, &detSize);
    
    printf("ADnED::allocArray: det: %d, detStart: %d, detEnd: %d, detSize: %d, tofMax: %d\n", 
           det, detStart, detEnd, detSize, tofMax);

    //Calculate sizes and do sanity checks
    if (detStart <= detEnd) {
      //If user has left detSize 0, just set equal to pixel ID range.
      if (detSize == 0) {
        detSize = detEnd-detStart+1;
        setIntegerParam(det, ADnEDDetPixelNumSizeParam, detSize);
      }
    } else {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s detStart > detEnd.\n", functionName);
      setIntegerParam(ADnEDAllocSpaceStatusParam, s_ADNED_ALLOC_STATUS_FAIL);
      return asynError;
    }
    
    printf("ADnED::allocArray: m_dataMaxSize: %d\n", m_dataMaxSize);
    printf("ADnED::allocArray: detSize: %d\n", detSize);

    setIntegerParam(det, ADnEDDetNDArrayStartParam, m_dataMaxSize);
    setIntegerParam(det, ADnEDDetNDArrayEndParam, m_dataMaxSize+detSize-1);
    setIntegerParam(det, ADnEDDetNDArraySizeParam, detSize);
    callParamCallbacks(det);
    
    m_dataMaxSize += detSize;
    
  }
  
  printf("ADnED::allocArray: final m_dataMaxSize: %d\n", m_dataMaxSize);
  printf("ADnED::allocArray: TOF Size: %d\n", numDet * (tofMax+1));

  epicsUInt32 tofStart = m_dataMaxSize;
  epicsUInt32 tofEnd = 0;
  for (int det=1; det<=numDet; det++) {
    tofEnd = tofStart+tofMax;
    printf("ADnED::allocArray: det %d, TOF start: %d, TOF end: %d\n", det, tofStart, tofEnd);
    setIntegerParam(det, ADnEDDetNDArrayTOFStartParam, tofStart);
    setIntegerParam(det, ADnEDDetNDArrayTOFEndParam, tofEnd);
    tofStart = tofEnd+1;
    callParamCallbacks(det);
  }

  if (p_Data) {
    free(p_Data);
    p_Data = NULL;
  }
  
  if (!p_Data) {
    if (m_dataMaxSize != 0) {
      m_bufferMaxSize = m_dataMaxSize+(numDet * (tofMax+1));
      p_Data = static_cast<epicsUInt32*>(calloc(m_bufferMaxSize, sizeof(epicsUInt32)));
    } else {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s Not allocating zero sized array.\n", functionName);
      setIntegerParam(ADnEDAllocSpaceStatusParam, s_ADNED_ALLOC_STATUS_FAIL);
      status = asynError;
    }
  } else {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s pData already allocated.\n", functionName);
    setIntegerParam(ADnEDAllocSpaceStatusParam, s_ADNED_ALLOC_STATUS_FAIL);
    status = asynError;
  }
  if (!p_Data) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s pData failed to allocate.\n", functionName);
    setIntegerParam(ADnEDAllocSpaceStatusParam, s_ADNED_ALLOC_STATUS_FAIL);
    status = asynError;
  }

  if (status == asynSuccess) {
    m_dataAlloc = false;
    setIntegerParam(ADnEDAllocSpaceStatusParam, s_ADNED_ALLOC_STATUS_OK);
  }
  
  return status;
}

/**
 * Clear parameters and data members on a acqusition start.
 */
asynStatus ADnED::clearParams(void)
{
  bool status = true;
  const char* functionName = "ADnED::clearParams";

  status = ((setIntegerParam(ADnEDPulseCounterParam, 0) == asynSuccess) && status);
  status = ((setDoubleParam(ADnEDPChargeParam, 0.0) == asynSuccess) && status);
  status = ((setDoubleParam(ADnEDPChargeIntParam, 0.0) == asynSuccess) && status);
  //status = ((setIntegerParam(ADnEDHdfFileNumberParam, 0) == asynSuccess) && status);

  m_pChargeInt = 0.0;
  m_pulseCounter = 0;

  if (p_Data != NULL) {
    memset(p_Data, 0, m_bufferMaxSize*sizeof(epicsUInt32));
  }

  for (int chan=0; chan<s_ADNED_MAX_CHANNELS; ++chan) {
    status = ((setIntegerParam(chan, ADnEDSeqCounterParam, 0) == asynSuccess) && status);
    status = ((setIntegerParam(chan, ADnEDSeqIDParam, 0) == asynSuccess) && status);
    status = ((setIntegerParam(chan, ADnEDSeqIDMissingParam, 0) == asynSuccess) && status);
    status = ((setIntegerParam(chan, ADnEDSeqIDNumMissingParam, 0) == asynSuccess) && status);
    status = ((setIntegerParam(chan, ADnEDBadTimeStampParam, 0) == asynSuccess) && status);
    m_seqCounter[chan] = 0;
    m_seqID[chan] = 0;
    m_lastSeqID[chan] = -1;  
    m_TimeStamp[chan].put(0,0);
    m_TimeStampLast[chan].put(0,0);
    callParamCallbacks(chan);
  }

  for (int det=0; det<=s_ADNED_MAX_DETS; ++det) {
    m_detTotalEvents[det] = 0.0;
    setDoubleParam(det, ADnEDDetEventTotalParam, m_detTotalEvents[det]);
    callParamCallbacks(det);
  }

  if (!status) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s: ERROR: Failed to clear params.\n", functionName);
    return asynError;
  }
  
  return asynSuccess;
}


/**
 * Event readout task.
 */
void ADnED::eventTask(void)
{
  epicsEventWaitStatus eventStatus;
  epicsFloat64 timeout = 0.001;
  bool acquire = 0;
  bool error = true;
  int numChannels = 0;
  char pvName[s_ADNED_MAX_CHANNELS][s_ADNED_MAX_STRING_SIZE] = {{0}};
  const char* functionName = "ADnED::eventTask";
 
  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Started Event Thread.\n", functionName);

  setStringParam(ADStatusMessage, "Startup");
  setStringParam(ADnEDHdfStatusMessageParam, "Startup");


  while (1) {

    //Wait for a stop event, with a short timeout, to catch any that were done after last one.
    eventStatus = epicsEventWaitWithTimeout(m_stopEvent, timeout);          
    if (eventStatus == epicsEventWaitOK) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Got Stop Event Before Start Event.\n", functionName);
    }

    lock();
    setIntegerParam(ADAcquire, 0);
    if (!error) {
      setStringParam(ADStatusMessage, "Idle");
      setStringParam(ADnEDHdfStatusMessageParam, "Idle");

    }
    callParamCallbacks();
    unlock();

    eventStatus = epicsEventWait(m_startEvent);          
    if (eventStatus == epicsEventWaitOK) {
      printf("Got start event.");
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Got Start Event.\n", functionName);
      acquire = true;
      error = false;
      lock();

      //Get the number of required active PVAccess channels
      numChannels = 0;
      getIntegerParam(ADnEDNumChannelsParam, &numChannels);
      if (numChannels > s_ADNED_MAX_CHANNELS) {
        numChannels = s_ADNED_MAX_CHANNELS;
      }
        
      if (allocArray() != asynSuccess) {
        setStringParam(ADStatusMessage, "allocArray Error");
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s: ERROR: Failed to allocate array.\n", functionName);
        acquire = false;
        error = true;
      } else {
      
        //Clear arrays at start of acquire every time.
        if (p_Data != NULL) {
          memset(p_Data, 0, m_bufferMaxSize*sizeof(epicsUInt32));
        }
      
        setIntegerParam(ADStatus, ADStatusAcquire);
        setStringParam(ADStatusMessage, "Online montoring");
        // Start frame thread
        cout << "Send start frame" << endl;
        epicsEventSignal(this->m_startFrame);
        callParamCallbacks();

        //Read the PV names
        for (int chan=0; chan<numChannels; ++chan) {
          getStringParam(chan, ADnEDPVNameParam, sizeof(pvName[chan]), pvName[chan]);
        }

        //Connect channel here      
        if (!error) {
          try {
            for (int channel=0; channel<numChannels; ++channel) {
              if (pvName[channel][0] != '0') {
                if (setupChannelMonitor(pvName[channel], channel) != asynSuccess) {
                  throw std::runtime_error("Unknown error from setupChannelMonitor.");
                }
              }
            }
          } catch (std::exception &e)  {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                      "%s: ERROR: Problem creating monitor. Exception: %s\n", 
                      functionName, e.what());
            setStringParam(ADStatusMessage, "Connecting To PV.");
            setIntegerParam(ADStatus, ADStatusError);
            //<mine>   change the value of “acquire” and "error"
            acquire = true;
            error = false;
          }
        }

        if (!error) {
          for (int channel=0; channel<numChannels; ++channel) {
            if (p_Monitor[channel]) {
              p_Monitor[channel]->start();
            }
          }
        } else {
          cout << "Send Stop Frame" << endl;
          epicsEventSignal(this->m_stopFrame);    
        }       
      
      }
    } // end of if (eventStatus == epicsEventWaitOK)
    
    //If we failed to connect or setup, notify error.
    if (error) {
      setIntegerParam(ADStatus, ADStatusError);    
      acquire = false;
    }

    //Complete Start callback
    callParamCallbacks();
    setIntegerParam(ADnEDStartParam, 0);
    callParamCallbacks();
    unlock();
    
    while (acquire) {
      //Wait for a stop event, with a short timeout.
      //eventStatus = epicsEventWaitWithTimeout(m_stopEvent, timeout);      
      eventStatus = epicsEventWaitWithTimeout(m_stopEvent, 0.1);      
      if (eventStatus == epicsEventWaitOK) {
        cout << "Got stop event" << endl;
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Got Stop Event.\n", functionName);
        acquire = false;
      }

      if (!acquire) {
        lock();
        setIntegerParam(ADStatus, ADStatusIdle);
        cout << "Send Stop Frame" << endl;
        epicsEventSignal(this->m_stopFrame);
      }
      
    } // End of while(acquire)

    //Stop monitor here
    for (int channel=0; channel<numChannels; ++channel) {
      if (p_Monitor[channel]) {
        p_Monitor[channel]->stop();
      }
    }

    //Zero the event rate params
    int numDet = 0;
    getIntegerParam(ADnEDNumDetParam, &numDet);
    setIntegerParam(ADnEDEventRateParam, 0);
    for (int det=1; det<=numDet; det++) {
      setIntegerParam(det, ADnEDDetEventRateParam, 0);
      callParamCallbacks(det);
    }

    //Complete Stop callback
    callParamCallbacks();
    setIntegerParam(ADnEDStopParam, 0);
    callParamCallbacks();
    unlock();

  } // End of while(1)

  asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s: ERROR: Exiting ADnEDEventTask main loop.\n", functionName);

}


//Global C utility functions to tie in with EPICS
static void ADnEDEventTaskC(void *drvPvt)
{
  ADnED *pPvt = (ADnED *)drvPvt;
  
  pPvt->eventTask();
}

/**
 * Set up a PVAccess channel and a associated monitor.
 * This function may throw an exception.
 * @param pvName The PV name to use.
 * @param channel The PVAccess channel number, because we can support more than one channel monitor.
 * @return asynStatus If we complete normally, asynSuccess will be returned
 */
asynStatus ADnED::setupChannelMonitor(const char *pvName, int channel)
{
  bool connectStatus = false;
  bool newMonitor = false;
  const char* functionName = "ADnED::setupChannelMonitor";
  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s. PV Name: %s, Channel: %d", functionName, pvName, channel);
  
  cout << "PV Name: " << pvName << "  Channel: " << channel << endl;

  if (channel >= s_ADNED_MAX_CHANNELS) {
    throw std::runtime_error("channel >= s_ADNED_MAX_CHANNELS.");
  }

  if ((!p_ChannelRequester) || (!p_MonitorRequester)) {
    throw std::runtime_error("No Channel or Monitor Requester.");
  }

  if (!p_ChannelProvider) {
    p_ChannelProvider = epics::pvAccess::getChannelProviderRegistry()->getProvider(ADNED_PV_PROVIDER);
    if (!p_ChannelProvider) {
      throw std::runtime_error("No Channel Provider.");
    }
  }
  
  if (!p_Channel[channel]) {
    p_Channel[channel] = (shared_ptr<epics::pvAccess::Channel>)
      (p_ChannelProvider->createChannel(pvName, p_ChannelRequester, ADNED_PV_PRIORITY));
    connectStatus = p_ChannelRequester->waitUntilConnected(ADNED_PV_TIMEOUT);
  } else {
    std::string channelName(p_Channel[channel]->getChannelName());
    if (channelName != pvName) {
      p_Channel[channel]->destroy();
      p_Channel[channel] = (shared_ptr<epics::pvAccess::Channel>)
        (p_ChannelProvider->createChannel(pvName, p_ChannelRequester, ADNED_PV_PRIORITY));
      connectStatus = p_ChannelRequester->waitUntilConnected(ADNED_PV_TIMEOUT);
      newMonitor = true;
    }
    connectStatus = p_Channel[channel]->isConnected();
  }

  if (!connectStatus) {
    throw std::runtime_error("Timeout connecting to PV");
  }
  
  if ((!p_Monitor[channel]) || (newMonitor)) {
    if (p_Monitor[channel]) {
      p_Monitor[channel]->destroy();
    }
    shared_ptr<epics::pvData::PVStructure> pvRequest = epics::pvData::CreateRequest::create()->createRequest(ADNED_PV_REQUEST);
    p_Monitor[channel] = p_Channel[channel]->createMonitor(p_MonitorRequester[channel], pvRequest);
    p_MonitorRequester[channel]->waitUntilConnected(ADNED_PV_TIMEOUT);
  }
  
  return asynSuccess;
}


/**
 * Frame readout task.
 */
void ADnED::frameTask(void)
{
  epicsEventWaitStatus eventStatus;
  epicsFloat64 timeout = 0.001;
  bool acquire = false;
  int arrayCounter = 0;
  int arrayCallbacks = 0;
  epicsFloat64 updatePeriod = 0.0;
  epicsTimeStamp nowTime;
  NDArray *pNDArray = NULL;
  const char* functionName = "ADnED::frameTask";
 
  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Started Frame Thread.\n", functionName);

  while (1) {

    //Wait for a stop event, with a short timeout, to catch any that were done during last read.
    eventStatus = epicsEventWaitWithTimeout(m_stopFrame, 0.01);          
    if (eventStatus == epicsEventWaitOK) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Got Stop Frame Event Before Start Frame Event.\n", functionName);
    }
    
    callParamCallbacks();

    eventStatus = epicsEventWait(m_startFrame);          
    if (eventStatus == epicsEventWaitOK) {
      cout << "Got start frame" << endl;
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Got Start Frame Event.\n", functionName);
      acquire = true;
      arrayCounter = 0;
      lock();
      setIntegerParam(NDArrayCounter, arrayCounter);
      //setIntegerParam(ADnEDFrameStatus, ADStatusAcquire);
      //setStringParam(ADnEDFrameStatusMessage, "Acquiring Frames");
      callParamCallbacks();
      unlock();
    }

    while (acquire) {

      //Get the update period.
      getDoubleParam(ADnEDFrameUpdatePeriodParam, &updatePeriod);
      timeout = updatePeriod / 1000.0;
      
      //Wait for a stop event
      eventStatus = epicsEventWaitWithTimeout(m_stopFrame, timeout);
      if (eventStatus == epicsEventWaitOK) {
        cout << "Got Stop Frame" << endl;
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Got Stop Frame Event.\n", functionName);
        acquire = false;
      }

      if (acquire) {
        //Copy p_Data here into an NDArray of the same size. Do array callbacks.
        getIntegerParam(NDArrayCallbacks, &arrayCallbacks);
        if (arrayCallbacks) {
          ++arrayCounter;
          size_t dims[1] = {m_bufferMaxSize};
          if ((pNDArray = this->pNDArrayPool->alloc(1, dims, NDUInt32, 0, NULL)) == NULL) {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s: ERROR: pNDArrayPool->alloc failed.\n", functionName);
          } else {
            epicsTimeGetCurrent(&nowTime);
            pNDArray->uniqueId = arrayCounter;
            pNDArray->timeStamp = nowTime.secPastEpoch + nowTime.nsec / 1.e9;
            pNDArray->pAttributeList->add("TIMESTAMP", "Host Timestamp", NDAttrFloat64, &(pNDArray->timeStamp));
            lock();
            memcpy(pNDArray->pData, p_Data, m_bufferMaxSize * sizeof(epicsUInt32));
          
            unlock();
            asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s: Calling NDArray callback\n", functionName);
            //
            doCallbacksGenericPointer(pNDArray, NDArrayData, 0);
            //cout <<pNDArray <<"     the NDArrayData is :::"<< NDArrayData<<endl; 
          } //0x7f8c280a0070     the NDArrayData is :::38

          lock();
          //Free the NDArray 
          pNDArray->release();
          setIntegerParam(NDArrayCounter, arrayCounter);          
          callParamCallbacks();
          unlock();
        }

      }
      
    } // End of while(acquire)


  } // End of while(1)

  asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s: ERROR: Exiting ADnEDFrameTask main loop.\n", functionName);

}


//Global C utility functions to tie in with EPICS
static void ADnEDFrameTaskC(void *drvPvt)
{
  ADnED *pPvt = (ADnED *)drvPvt;
  
  pPvt->frameTask();
}





/*************************************************************************************/
/** The following functions have C linkage, and can be called directly or from iocsh */

extern "C" {

/**
 * Config function for IOC shell. It instantiates an instance of the driver.
 * @param portName The Asyn port name to use
 * @param maxBuffers Used by asynPortDriver (set to -1 for unlimited)
 * @param maxMemory Used by asynPortDriver (set to -1 for unlimited)
 * @param debug This debug flag is passed to xsp3_config in the Xspress API (0 or 1)
 */
  asynStatus ADnEDConfig(const char *portName, int maxBuffers, size_t maxMemory, int debug)
  {
    asynStatus status = asynSuccess;
    
    /*Instantiate class.*/
    try {
      new ADnED(portName, maxBuffers, maxMemory, debug);
    } catch (...) {
      cout << "Unknown exception caught when trying to construct ADnED." << endl;
      status = asynError;
    }
    
    return(status);
  }

/**
 * Config function for IOC shell. It instantiates a PVAccess client factory for this IOC.
 */
  asynStatus ADnEDCreateFactory()
  { 
    /*Instantiate factory.*/
    return ADnED::createFactory();
  }


   
  /* Code for iocsh registration */
  
  /* ADnEDConfig */
  static const iocshArg ADnEDConfigArg0 = {"Port name", iocshArgString};
  static const iocshArg ADnEDConfigArg1 = {"Max Buffers", iocshArgInt};
  static const iocshArg ADnEDConfigArg2 = {"Max Memory", iocshArgInt};
  static const iocshArg ADnEDConfigArg3 = {"Debug", iocshArgInt};
  static const iocshArg * const ADnEDConfigArgs[] =  {&ADnEDConfigArg0,
                                                         &ADnEDConfigArg1,
                                                         &ADnEDConfigArg2,
                                                         &ADnEDConfigArg3};
  
  static const iocshFuncDef configADnED = {"ADnEDConfig", 4, ADnEDConfigArgs};
  static void configADnEDCallFunc(const iocshArgBuf *args)
  {
    ADnEDConfig(args[0].sval, args[1].ival, args[2].ival, args[3].ival);
  }

  static const iocshFuncDef configADnEDCreateFactory = {"ADnEDCreateFactory", 0, NULL};
  static void configADnEDCreateFactoryCallFunc(const iocshArgBuf *args)
  {
    ADnEDCreateFactory();
  }

  
  static void ADnEDRegister(void)
  {
    iocshRegister(&configADnED, configADnEDCallFunc);
    iocshRegister(&configADnEDCreateFactory, configADnEDCreateFactoryCallFunc);
  }
  
  epicsExportRegistrar(ADnEDRegister);

} // extern "C"


/****************************************************************************************/
