/*
 * Copyright (c) 2011-2014, fortiss GmbH.
 * Licensed under the Apache License, Version 2.0.
 *
 * Use, modification and distribution are subject to the terms specified
 * in the accompanying license file LICENSE.txt located at the root directory
 * of this software distribution. A copy is available at
 * http://chromosome.fortiss.org/.
 *
 * This file is part of CHROMOSOME.
 *
 * $Id: monitorBComponentWrapper.h 7769 2014-03-11 15:27:13Z geisinger $
 */

/**
 * \file
 *         Component wrapper - implements interface of a component
 *              to the data handler
 *
 * \author
 *         This file has been generated by the CHROMOSOME Modeling Tool (XMT)
 *         (fortiss GmbH).
 */

#ifndef SENSORMONITOR_ADV_MONITORB_MONITORBCOMPONENTWRAPPER_H
#define SENSORMONITOR_ADV_MONITORB_MONITORBCOMPONENTWRAPPER_H

/******************************************************************************/
/***   Includes                                                             ***/
/******************************************************************************/
#include "xme/core/executionManager/include/executionManagerDataStructures.h"

#include "sensorMonitor/topic/dictionaryData.h"

/******************************************************************************/
/***   Type definitions                                                     ***/
/******************************************************************************/

/**
 * \enum sensorMonitor_adv_monitorB_monitorBComponentWrapper_internalFunctionId_e
 *
 * \brief Values for identifying functions of monitorB component.
 */
enum sensorMonitor_adv_monitorB_monitorBComponentWrapper_internalFunctionId_e
{
    SENSORMONITOR_ADV_MONITORB_MONITORBCOMPONENTWRAPPER_FUNCTION_PRINTSENSORVALUE = 0  ///< Function 'printSensorValue'
};

/**
 * \enum sensorMonitor_adv_monitorB_monitorBComponentWrapper_internalPortId_e
 *
 * \brief Values for sensorMonitor_adv_monitorB_monitorBComponentWrapper_internalPortId_t.
 */
enum sensorMonitor_adv_monitorB_monitorBComponentWrapper_internalPortId_e
{
    SENSORMONITOR_ADV_MONITORB_MONITORBCOMPONENTWRAPPER_PORT_SENSORVALUEIN = 0  ///< Port 'sensorValueIn'
};

/**
 * \typedef sensorMonitor_adv_monitorB_monitorBComponentWrapper_internalPortId_t
 *
 * \brief Defines internal port ids of component 'monitorB'.
 *
 * \details These can be used when calling the sensorMonitor_adv_monitorB_monitorBComponentWrapper_receivePort function.
 *          For the definition of possible values, see enum sensorMonitor_adv_monitorB_monitorBComponentWrapper_internalPortId_e.
 */
typedef uint8_t sensorMonitor_adv_monitorB_monitorBComponentWrapper_internalPortId_t;

/******************************************************************************/
/***   Prototypes                                                           ***/
/******************************************************************************/
XME_EXTERN_C_BEGIN

/**
 * \brief Initializes this component wrapper.
 *
 * \retval XME_STATUS_SUCCESS on success.
 */
xme_status_t
sensorMonitor_adv_monitorB_monitorBComponentWrapper_init(void);

/**
 * \brief Finalizes this component wrapper.
 */
void
sensorMonitor_adv_monitorB_monitorBComponentWrapper_fini(void);

/**
 * \brief Associate an internal port number with the corresponding port handle.
 *        For the ids of the individual ports, see the definition of sensorMonitor_adv_monitorB_monitorBComponentWrapper_internalPortId_t.
 *
 * \param dataPacketId Port handle from the dataHandler.
 * \param componentInternalPortId Component internal port number of the above port.
 *
 * \retval XME_STATUS_SUCCESS if no problems occurred.
 * \retval XME_STATUS_INVALID_PARAMETER if componentInternalPortId is unknown.
 */
xme_status_t
sensorMonitor_adv_monitorB_monitorBComponentWrapper_receivePort
(
    xme_core_dataManager_dataPacketId_t dataPacketId,
    sensorMonitor_adv_monitorB_monitorBComponentWrapper_internalPortId_t componentInternalPortId
);

/**
 * \brief This function is called by the function wrapper after the step
 *        function has been called. It signals to the middleware that all
 *        input ports have been read.
 */
void
sensorMonitor_adv_monitorB_monitorBComponentWrapper_completeReadOperations(void);

/**
 * \brief Read data from port 'sensorValueIn'.
 *
 * \param[out] data User provided storage, to which the data is copied.
 *
 * \retval XME_STATUS_SUCCESS if reading was successful.
 * \retval XME_STATUS_NO_SUCH_VALUE when there is now new data on this port
 *         (only possible when this port is an optional port of the current function).
 * \retval XME_STATUS_INVALID_PARAMETER if data is NULL.
 * \retval XME_STATUS_INTERNAL_ERROR in case of any other error.
 */
xme_status_t
sensorMonitor_adv_monitorB_monitorBComponentWrapper_readPortSensorValueIn
(
    const sensorMonitor_topic_sensorData_t* data
);

xme_status_t
sensorMonitor_adv_monitorB_monitorBComponentWrapper_readInputPortAttribute
(
    sensorMonitor_adv_monitorB_monitorBComponentWrapper_internalPortId_t portId,
    xme_core_attribute_key_t attributeKey,
    void* const buffer,
    uint32_t bufferSize
);

xme_status_t
sensorMonitor_adv_monitorB_monitorBComponentWrapper_readNextPacket
(
    sensorMonitor_adv_monitorB_monitorBComponentWrapper_internalPortId_t portId
);


XME_EXTERN_C_END

#endif // #ifndef SENSORMONITOR_ADV_MONITORB_MONITORBCOMPONENTWRAPPER_H