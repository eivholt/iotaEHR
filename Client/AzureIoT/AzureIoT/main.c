/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

// This sample C application for Azure Sphere demonstrates Azure IoT SDK C APIs
// The application uses the Azure IoT SDK C APIs to
// 1. Use the buttons to trigger sending heart rate and blood SpO2 % to Azure IoT Central.
// 2. Use Device Twin to control an LED.

// You will need to provide four pieces of information to use this application, all of which are set
// in the app_manifest.json.
// 1. The Scope Id for your IoT Central application (set in 'CmdArgs')
// 2. The Tenant Id obtained from 'azsphere tenant show-selected' (set in 'DeviceAuthentication')
// 3. The Azure DPS Global endpoint address 'global.azure-devices-provisioning.net'
//    (set in 'AllowedConnections')
// 4. The IoT Hub Endpoint address for your IoT Central application (set in 'AllowedConnections')

#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

// applibs_versions.h defines the API struct versions to use for applibs APIs.
#include "applibs_versions.h"
#include <applibs/log.h>
#include <applibs/networking.h>
#include <applibs/gpio.h>
#include <applibs/storage.h>
#include <applibs/i2c.h>

// By default, this sample is targeted at the MT3620 Reference Development Board (RDB).
// This can be changed using the project property "Target Hardware Definition Directory".
// This #include imports the sample_hardware abstraction from that hardware definition.
#include <hw/sample_hardware.h>
#include "max30102.h"
#include "algorithm_by_RF.h"

#include "epoll_timerfd_utilities.h"

// Azure IoT SDK
#include <iothub_client_core_common.h>
#include <iothub_device_client_ll.h>
#include <iothub_client_options.h>
#include <iothubtransportmqtt.h>
#include <iothub.h>
#include <azure_sphere_provisioning.h>

static volatile sig_atomic_t terminationRequired = false;

#include "parson.h" // used to parse Device Twin messages.

// Azure IoT Hub/Central defines.
#define SCOPEID_LENGTH 20
static char scopeId[SCOPEID_LENGTH]; // ScopeId for the Azure IoT Central application, set in
                                     // app_manifest.json, CmdArgs

static IOTHUB_DEVICE_CLIENT_LL_HANDLE iothubClientHandle = NULL;
static const int keepalivePeriodSeconds = 20;
static bool iothubAuthenticated = false;
static void SendMessageCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void *context);
static void TwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char *payload,
                         size_t payloadSize, void *userContextCallback);
static void TwinReportBoolState(const unsigned char *propertyName, bool propertyValue);
static void TwinReportStringState(const unsigned char* propertyName, const unsigned char* propertyValue);
static void ReportStatusCallback(int result, void *context);
static const char *GetReasonString(IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason);
static const char *getAzureSphereProvisioningResultString(
    AZURE_SPHERE_PROV_RETURN_VALUE provisioningResult);
static void SendTelemetry(const unsigned char *key, const unsigned char *value);
static void SetupAzureClient(void);

// Function to generate simulated Temperature data/telemetry
static void SendDeviceHeartbeat(void);

// Initialization/Cleanup
static int InitPeripheralsAndHandlers(void);
static void ClosePeripheralsAndHandlers(void);

// File descriptors - initialized to invalid value
// Buttons
static int sendMessageButtonGpioFd = -1;
static int sendTelemetryButtonGpioFd = -1;

// LED
static int deviceTwinStatusLedGpioFd = -1;
static bool statusLedOn = false;

// HR4 defines
#define PROXIMITY_THRESHOLD  32000
#define delay(x)             (usleep(x*1000))   //macro to provide ms pauses
#define MIKROE_INT    MT3620_GPIO2  //Socket#1=GPIO2_PWM2

// Timer / polling
static int buttonPollTimerFd = -1;
static int azureTimerFd = -1;
static int epollFd = -1;
static int i2cFd = -1;
static int intPinFd = -1;

// Azure IoT poll periods
static const int AzureIoTDefaultPollPeriodSeconds = 20;
static const int AzureIoTMinReconnectPeriodSeconds = 10;
static const int AzureIoTMaxReconnectPeriodSeconds = 10 * 60;
static int azureIoTPollPeriodSeconds = -1;

// Application states
static char nprId[12] = { 0 };

// Button state variables
static GPIO_Value_Type sendMessageButtonState = GPIO_Value_High;
static GPIO_Value_Type sendTelemetryButtonState = GPIO_Value_High;

static void ButtonPollTimerEventHandler(EventData *eventData);
static bool IsButtonPressed(int fd, GPIO_Value_Type *oldState);
static void SendTelemetryButtonHandler(void);
static void AzureTimerEventHandler(EventData *eventData);

// HR4 variables
static uint8_t max30102_revision = 0;
static uint8_t max30102_part_id = 0;

/// <summary>
///     Signal handler for termination requests. This handler must be async-signal-safe.
/// </summary>
static void TerminationHandler(int signalNumber)
{
    // Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
    terminationRequired = true;
}


void InitHR4(void) {
	intPinFd = GPIO_OpenAsInput(MIKROE_INT);

	i2cFd = I2CMaster_Open(MT3620_RDB_HEADER4_ISU2_I2C);
	if (i2cFd < 0) {
		Log_Debug("ERROR: I2CMaster_Open: errno=%d (%s)\n", errno, strerror(errno));
		return;
	}

	int result = I2CMaster_SetBusSpeed(i2cFd, I2C_BUS_SPEED_STANDARD);
	if (result != 0) {
		Log_Debug("ERROR: I2CMaster_SetBusSpeed: errno=%d (%s)\n", errno, strerror(errno));
		return;
	}

	result = I2CMaster_SetTimeout(i2cFd, 100);
	if (result != 0) {
		Log_Debug("ERROR: I2CMaster_SetTimeout: errno=%d (%s)\n", errno, strerror(errno));
		return;
	}
}

int Read_i2c(uint8_t addr, uint16_t count, uint8_t* ptr)
{
	int r = I2CMaster_WriteThenRead(i2cFd, MAX30101_SAD, &addr, sizeof(addr), ptr, count);
	if (r == -1)
		Log_Debug("ERROR: I2CMaster_Writer: errno=%d (%s)\n", errno, strerror(errno));
	return r;
}

void Write_i2c(uint8_t addr, uint16_t count, uint8_t* ptr)
{
	uint8_t buff[2];
	buff[0] = addr;
	buff[1] = *ptr;

	int r = I2CMaster_Write(i2cFd, MAX30101_SAD, buff, 2);
	if (r == -1)
		Log_Debug("ERROR: I2CMaster_Writer: errno=%d (%s)\n", errno, strerror(errno));
}

/// <summary>
///     Main entry point for this sample.
/// </summary>
int main(int argc, char *argv[])
{
    Log_Debug("IoT Central Application starting.\n");

    if (argc == 2) {
        Log_Debug("Setting Azure Scope ID %s\n", argv[1]);
        strncpy(scopeId, argv[1], SCOPEID_LENGTH);
    } else {
        Log_Debug("ScopeId needs to be set in the app_manifest CmdArgs\n");
        return -1;
    }

    if (InitPeripheralsAndHandlers() != 0) {
        terminationRequired = true;
    }

    // Main loop
    while (!terminationRequired) {
        if (WaitForEventAndCallHandler(epollFd) != 0) {
            terminationRequired = true;
        }
    }

    ClosePeripheralsAndHandlers();

    Log_Debug("Application exiting.\n");

    return 0;
}

/// <summary>
/// Button timer event:  Check the status of buttons A and B
/// </summary>
static void ButtonPollTimerEventHandler(EventData *eventData)
{
    if (ConsumeTimerFdEvent(buttonPollTimerFd) != 0) {
        terminationRequired = true;
        return;
    }

    SendTelemetryButtonHandler();
}

/// <summary>
/// Azure timer event:  Check connection status and send telemetry
/// </summary>
static void AzureTimerEventHandler(EventData *eventData)
{
    if (ConsumeTimerFdEvent(azureTimerFd) != 0) {
        terminationRequired = true;
        return;
    }

    bool isNetworkReady = false;
    if (Networking_IsNetworkingReady(&isNetworkReady) != -1) {
        if (isNetworkReady && !iothubAuthenticated) {
            SetupAzureClient();
        }
    } else {
        Log_Debug("Failed to get Network state\n");
    }

    if (iothubAuthenticated) {
		//SendDeviceHeartbeat();
        IoTHubDeviceClient_LL_DoWork(iothubClientHandle);
    }
}

// event handler data structures. Only the event handler field needs to be populated.
static EventData buttonPollEventData = {.eventHandler = &ButtonPollTimerEventHandler};
static EventData azureEventData = {.eventHandler = &AzureTimerEventHandler};

/// <summary>
///     Set up SIGTERM termination handler, initialize peripherals, and set up event handlers.
/// </summary>
/// <returns>0 on success, or -1 on failure</returns>
static int InitPeripheralsAndHandlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = TerminationHandler;
    sigaction(SIGTERM, &action, NULL);

    epollFd = CreateEpollFd();
    if (epollFd < 0) {
        return -1;
    }

    // Open button A GPIO as input
    Log_Debug("Opening SAMPLE_BUTTON_1 as input\n");
    sendMessageButtonGpioFd = GPIO_OpenAsInput(SAMPLE_BUTTON_1);
    if (sendMessageButtonGpioFd < 0) {
        Log_Debug("ERROR: Could not open button A: %s (%d).\n", strerror(errno), errno);
        return -1;
    }

    // Open button B GPIO as input
    Log_Debug("Opening SAMPLE_BUTTON_2 as input\n");
    sendTelemetryButtonGpioFd = GPIO_OpenAsInput(SAMPLE_BUTTON_2);
    if (sendTelemetryButtonGpioFd < 0) {
        Log_Debug("ERROR: Could not open button B: %s (%d).\n", strerror(errno), errno);
        return -1;
    }

    // LED 4 Blue is used to show Device Twin settings state
    Log_Debug("Opening SAMPLE_LED as output\n");
    deviceTwinStatusLedGpioFd =
        GPIO_OpenAsOutput(SAMPLE_LED, GPIO_OutputMode_PushPull, GPIO_Value_High);
    if (deviceTwinStatusLedGpioFd < 0) {
        Log_Debug("ERROR: Could not open LED: %s (%d).\n", strerror(errno), errno);
        return -1;
    }

	InitHR4();
	maxim_max30102_i2c_setup(Read_i2c, Write_i2c);
	max30102_revision = max30102_get_revision();
	max30102_part_id = max30102_get_part_id();
	Log_Debug("HeartRate Click Revision: 0x%02X\n", max30102_get_revision());
	Log_Debug("HeartRate Click Part ID:  0x%02X\n\n", max30102_get_part_id());

    // Set up a timer to poll for button events.
    struct timespec buttonPressCheckPeriod = {0, 1000 * 1000};
    buttonPollTimerFd =
        CreateTimerFdAndAddToEpoll(epollFd, &buttonPressCheckPeriod, &buttonPollEventData, EPOLLIN);
    if (buttonPollTimerFd < 0) {
        return -1;
    }

    azureIoTPollPeriodSeconds = AzureIoTDefaultPollPeriodSeconds;
    struct timespec azureTelemetryPeriod = {azureIoTPollPeriodSeconds, 0};
    azureTimerFd =
        CreateTimerFdAndAddToEpoll(epollFd, &azureTelemetryPeriod, &azureEventData, EPOLLIN);
    if (buttonPollTimerFd < 0) {
        return -1;
    }

    return 0;
}

/// <summary>
///     Close peripherals and handlers.
/// </summary>
static void ClosePeripheralsAndHandlers(void)
{
    Log_Debug("Closing file descriptors\n");

    // Leave the LEDs off
    if (deviceTwinStatusLedGpioFd >= 0) {
        GPIO_SetValue(deviceTwinStatusLedGpioFd, GPIO_Value_High);
    }

    CloseFdAndPrintError(buttonPollTimerFd, "ButtonTimer");
    CloseFdAndPrintError(azureTimerFd, "AzureTimer");
    CloseFdAndPrintError(sendMessageButtonGpioFd, "SendMessageButton");
    CloseFdAndPrintError(sendTelemetryButtonGpioFd, "SendTelemetryButton");
    CloseFdAndPrintError(deviceTwinStatusLedGpioFd, "StatusLed");
	CloseFdAndPrintError(intPinFd, "MIKROE_INT");
	CloseFdAndPrintError(i2cFd, "MIKROE_I2C");
    CloseFdAndPrintError(epollFd, "Epoll");		
}

/// <summary>
///     Sets the IoT Hub authentication state for the app
///     The SAS Token expires which will set the authentication state
/// </summary>
static void HubConnectionStatusCallback(IOTHUB_CLIENT_CONNECTION_STATUS result,
                                        IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason,
                                        void *userContextCallback)
{
    iothubAuthenticated = (result == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED);
    Log_Debug("IoT Hub Authenticated: %s\n", GetReasonString(reason));

	if (iothubAuthenticated) 
	{
		char max30102_revision_string[10] = { 0 };
		char max30102_part_id_string[10] = { 0 };
		snprintf(max30102_revision_string, 10, "0x%02X", max30102_revision);
		snprintf(max30102_part_id_string, 10, "0x%02X", max30102_part_id);
		TwinReportStringState("max30102_revision", max30102_revision_string);
		TwinReportStringState("max30102_part_id", max30102_part_id_string);
	}
}

/// <summary>
///     Sets up the Azure IoT Hub connection (creates the iothubClientHandle)
///     When the SAS Token for a device expires the connection needs to be recreated
///     which is why this is not simply a one time call.
/// </summary>
static void SetupAzureClient(void)
{
    if (iothubClientHandle != NULL)
        IoTHubDeviceClient_LL_Destroy(iothubClientHandle);

    AZURE_SPHERE_PROV_RETURN_VALUE provResult =
        IoTHubDeviceClient_LL_CreateWithAzureSphereDeviceAuthProvisioning(scopeId, 10000,
                                                                          &iothubClientHandle);
    Log_Debug("IoTHubDeviceClient_LL_CreateWithAzureSphereDeviceAuthProvisioning returned '%s'.\n",
              getAzureSphereProvisioningResultString(provResult));

    if (provResult.result != AZURE_SPHERE_PROV_RESULT_OK) {

        // If we fail to connect, reduce the polling frequency, starting at
        // AzureIoTMinReconnectPeriodSeconds and with a backoff up to
        // AzureIoTMaxReconnectPeriodSeconds
        if (azureIoTPollPeriodSeconds == AzureIoTDefaultPollPeriodSeconds) {
            azureIoTPollPeriodSeconds = AzureIoTMinReconnectPeriodSeconds;
        } else {
            azureIoTPollPeriodSeconds *= 2;
            if (azureIoTPollPeriodSeconds > AzureIoTMaxReconnectPeriodSeconds) {
                azureIoTPollPeriodSeconds = AzureIoTMaxReconnectPeriodSeconds;
            }
        }

        struct timespec azureTelemetryPeriod = {azureIoTPollPeriodSeconds, 0};
        SetTimerFdToPeriod(azureTimerFd, &azureTelemetryPeriod);

        Log_Debug("ERROR: failure to create IoTHub Handle - will retry in %i seconds.\n",
                  azureIoTPollPeriodSeconds);
        return;
    }

    // Successfully connected, so make sure the polling frequency is back to the default
    azureIoTPollPeriodSeconds = AzureIoTDefaultPollPeriodSeconds;
    struct timespec azureTelemetryPeriod = {azureIoTPollPeriodSeconds, 0};
    SetTimerFdToPeriod(azureTimerFd, &azureTelemetryPeriod);

    iothubAuthenticated = true;

    if (IoTHubDeviceClient_LL_SetOption(iothubClientHandle, OPTION_KEEP_ALIVE,
                                        &keepalivePeriodSeconds) != IOTHUB_CLIENT_OK) {
        Log_Debug("ERROR: failure setting option \"%s\"\n", OPTION_KEEP_ALIVE);
        return;
    }

    IoTHubDeviceClient_LL_SetDeviceTwinCallback(iothubClientHandle, TwinCallback, NULL);
	IoTHubDeviceClient_LL_SetConnectionStatusCallback(iothubClientHandle,
		HubConnectionStatusCallback, NULL);
}

/// <summary>
///     Callback invoked when a Device Twin update is received from IoT Hub.
///     Updates local state for 'showEvents' (bool).
/// </summary>
/// <param name="payload">contains the Device Twin JSON document (desired and reported)</param>
/// <param name="payloadSize">size of the Device Twin JSON document</param>
static void TwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char *payload,
                         size_t payloadSize, void *userContextCallback)
{
    size_t nullTerminatedJsonSize = payloadSize + 1;
    char *nullTerminatedJsonString = (char *)malloc(nullTerminatedJsonSize);
    if (nullTerminatedJsonString == NULL) {
        Log_Debug("ERROR: Could not allocate buffer for twin update payload.\n");
        abort();
    }

    // Copy the provided buffer to a null terminated buffer.
    memcpy(nullTerminatedJsonString, payload, payloadSize);
    // Add the null terminator at the end.
    nullTerminatedJsonString[nullTerminatedJsonSize - 1] = 0;

    JSON_Value *rootProperties = NULL;
    rootProperties = json_parse_string(nullTerminatedJsonString);
    if (rootProperties == NULL) {
        Log_Debug("WARNING: Cannot parse the string as JSON content.\n");
        goto cleanup;
    }

    JSON_Object *rootObject = json_value_get_object(rootProperties);
    JSON_Object *desiredProperties = json_object_dotget_object(rootObject, "desired");
    if (desiredProperties == NULL) {
        desiredProperties = rootObject;
    }

    // Handle the Device Twin Desired Properties here.
    JSON_Object *LEDState = json_object_dotget_object(desiredProperties, "StatusLED");
    if (LEDState != NULL) {
        statusLedOn = (bool)json_object_get_boolean(LEDState, "value");
        GPIO_SetValue(deviceTwinStatusLedGpioFd,
                      (statusLedOn == true ? GPIO_Value_Low : GPIO_Value_High));
        TwinReportBoolState("StatusLED", statusLedOn);
    }

	JSON_Object* nprIdState = json_object_dotget_object(desiredProperties, "nprId");
	if (nprIdState != NULL) {
		strcpy(nprId, json_object_get_string(nprIdState, "value"));
		TwinReportStringState("nprId", nprId);
		TwinReportStringState("nprId_property", nprId);
	}

cleanup:
    // Release the allocated memory.
    json_value_free(rootProperties);
    free(nullTerminatedJsonString);
}

/// <summary>
///     Converts the IoT Hub connection status reason to a string.
/// </summary>
static const char *GetReasonString(IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason)
{
    static char *reasonString = "unknown reason";
    switch (reason) {
    case IOTHUB_CLIENT_CONNECTION_EXPIRED_SAS_TOKEN:
        reasonString = "IOTHUB_CLIENT_CONNECTION_EXPIRED_SAS_TOKEN";
        break;
    case IOTHUB_CLIENT_CONNECTION_DEVICE_DISABLED:
        reasonString = "IOTHUB_CLIENT_CONNECTION_DEVICE_DISABLED";
        break;
    case IOTHUB_CLIENT_CONNECTION_BAD_CREDENTIAL:
        reasonString = "IOTHUB_CLIENT_CONNECTION_BAD_CREDENTIAL";
        break;
    case IOTHUB_CLIENT_CONNECTION_RETRY_EXPIRED:
        reasonString = "IOTHUB_CLIENT_CONNECTION_RETRY_EXPIRED";
        break;
    case IOTHUB_CLIENT_CONNECTION_NO_NETWORK:
        reasonString = "IOTHUB_CLIENT_CONNECTION_NO_NETWORK";
        break;
    case IOTHUB_CLIENT_CONNECTION_COMMUNICATION_ERROR:
        reasonString = "IOTHUB_CLIENT_CONNECTION_COMMUNICATION_ERROR";
        break;
    case IOTHUB_CLIENT_CONNECTION_OK:
        reasonString = "IOTHUB_CLIENT_CONNECTION_OK";
        break;
    }
    return reasonString;
}

/// <summary>
///     Converts AZURE_SPHERE_PROV_RETURN_VALUE to a string.
/// </summary>
static const char *getAzureSphereProvisioningResultString(
    AZURE_SPHERE_PROV_RETURN_VALUE provisioningResult)
{
    switch (provisioningResult.result) {
    case AZURE_SPHERE_PROV_RESULT_OK:
        return "AZURE_SPHERE_PROV_RESULT_OK";
    case AZURE_SPHERE_PROV_RESULT_INVALID_PARAM:
        return "AZURE_SPHERE_PROV_RESULT_INVALID_PARAM";
    case AZURE_SPHERE_PROV_RESULT_NETWORK_NOT_READY:
        return "AZURE_SPHERE_PROV_RESULT_NETWORK_NOT_READY";
    case AZURE_SPHERE_PROV_RESULT_DEVICEAUTH_NOT_READY:
        return "AZURE_SPHERE_PROV_RESULT_DEVICEAUTH_NOT_READY";
    case AZURE_SPHERE_PROV_RESULT_PROV_DEVICE_ERROR:
        return "AZURE_SPHERE_PROV_RESULT_PROV_DEVICE_ERROR";
    case AZURE_SPHERE_PROV_RESULT_GENERIC_ERROR:
        return "AZURE_SPHERE_PROV_RESULT_GENERIC_ERROR";
    default:
        return "UNKNOWN_RETURN_VALUE";
    }
}

/// <summary>
///     Sends telemetry to IoT Hub
/// </summary>
/// <param name="key">The telemetry item to update</param>
/// <param name="value">new telemetry value</param>
static void SendTelemetry(const unsigned char *key, const unsigned char *value)
{
    static char eventBuffer[100] = {0};
    static const char *EventMsgTemplate = "{ \"%s\": \"%s\" }";
    int len = snprintf(eventBuffer, sizeof(eventBuffer), EventMsgTemplate, key, value);
    if (len < 0)
        return;

    Log_Debug("Sending IoT Hub Message: %s\n", eventBuffer);

    IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromString(eventBuffer);

    if (messageHandle == 0) {
        Log_Debug("WARNING: unable to create a new IoTHubMessage\n");
        return;
    }

    if (IoTHubDeviceClient_LL_SendEventAsync(iothubClientHandle, messageHandle, SendMessageCallback,
                                             /*&callback_param*/ 0) != IOTHUB_CLIENT_OK) {
        Log_Debug("WARNING: failed to hand over the message to IoTHubClient\n");
    } else {
        Log_Debug("INFO: IoTHubClient accepted the message for delivery\n");
    }

    IoTHubMessage_Destroy(messageHandle);
}

/// <summary>
///     Callback confirming message delivered to IoT Hub.
/// </summary>
/// <param name="result">Message delivery status</param>
/// <param name="context">User specified context</param>
static void SendMessageCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void *context)
{
    Log_Debug("INFO: Message received by IoT Hub. Result is: %d\n", result);
}

/// <summary>
///     Creates and enqueues a report containing the name and value pair of a Device Twin reported
///     property. The report is not sent immediately, but it is sent on the next invocation of
///     IoTHubDeviceClient_LL_DoWork().
/// </summary>
/// <param name="propertyName">the IoT Hub Device Twin property name</param>
/// <param name="propertyValue">the IoT Hub Device Twin property value</param>
static void TwinReportBoolState(const unsigned char *propertyName, bool propertyValue)
{
    if (iothubClientHandle == NULL) {
        Log_Debug("ERROR: client not initialized\n");
    } else {
        static char reportedPropertiesString[30] = {0};
        int len = snprintf(reportedPropertiesString, 30, "{\"%s\":%s}", propertyName,
                           (propertyValue == true ? "true" : "false"));
        if (len < 0)
            return;

        if (IoTHubDeviceClient_LL_SendReportedState(
                iothubClientHandle, (unsigned char *)reportedPropertiesString,
                strlen(reportedPropertiesString), ReportStatusCallback, 0) != IOTHUB_CLIENT_OK) {
            Log_Debug("ERROR: failed to set reported state for '%s'.\n", propertyName);
        } else {
            Log_Debug("INFO: Reported state for '%s' to value '%s'.\n", propertyName,
                      (propertyValue == true ? "true" : "false"));
        }
    }
}

/// <summary>
///     Creates and enqueues a report containing the name and value pair of a Device Twin reported
///     property. The report is not sent immediately, but it is sent on the next invocation of
///     IoTHubDeviceClient_LL_DoWork().
/// </summary>
/// <param name="propertyName">the IoT Hub Device Twin property name</param>
/// <param name="propertyValue">the IoT Hub Device Twin property value</param>
static void TwinReportStringState(const unsigned char* propertyName, const unsigned char* propertyValue)
{
	if (iothubClientHandle == NULL) {
		Log_Debug("ERROR: client not initialized\n");
	}
	else {
		static char reportedPropertiesString[40] = { 0 };
		int len = snprintf(reportedPropertiesString, 40, "{\"%s\":\"%s\"}", propertyName,
			propertyValue);
		if (len < 0)
			return;
		Log_Debug("Sending IoT Hub Message Reported state: %s\n", reportedPropertiesString);
		if (IoTHubDeviceClient_LL_SendReportedState(
			iothubClientHandle, (unsigned char*)reportedPropertiesString,
			strlen(reportedPropertiesString), ReportStatusCallback, 0) != IOTHUB_CLIENT_OK) {
			Log_Debug("ERROR: failed to set reported state for '%s'.\n", propertyName);
		}
		else {
			Log_Debug("INFO: Reported state for '%s' to value '%s'.\n", propertyName,
				propertyValue);
		}
	}
}

/// <summary>
///     Callback invoked when the Device Twin reported properties are accepted by IoT Hub.
/// </summary>
static void ReportStatusCallback(int result, void *context)
{
    Log_Debug("INFO: Device Twin reported properties update result: HTTP status code %d\n", result);
}

/// <summary>
///     Generates a simulated Temperature and sends to IoT Hub.
/// </summary>
void SendDeviceHeartbeat(void)
{
	SendTelemetry("device_heartbeat", "True");
}

/// <summary>
///     Check whether a given button has just been pressed.
/// </summary>
/// <param name="fd">The button file descriptor</param>
/// <param name="oldState">Old state of the button (pressed or released)</param>
/// <returns>true if pressed, false otherwise</returns>
static bool IsButtonPressed(int fd, GPIO_Value_Type *oldState)
{
    bool isButtonPressed = false;
    GPIO_Value_Type newState;
    int result = GPIO_GetValue(fd, &newState);
    if (result != 0) {
        Log_Debug("ERROR: Could not read button GPIO: %s (%d).\n", strerror(errno), errno);
        terminationRequired = true;
    } else {
        // Button is pressed if it is low and different than last known state.
        isButtonPressed = (newState != *oldState) && (newState == GPIO_Value_Low);
        *oldState = newState;
    }

    return isButtonPressed;
}

/// <summary>
/// Pressing button B will:
///     Send an 'Heart_rate' and 'Sp02' event to Azure IoT Central
/// </summary>
static void SendTelemetryButtonHandler(void)
{
	if (IsButtonPressed(sendTelemetryButtonGpioFd, &sendTelemetryButtonState))
	{
		float    n_spo2, ratio, correl;                                       //SPO2 value
		int8_t   ch_spo2_valid;                                               //indicator to show if the SPO2 calculation is valid
		int32_t  n_heart_rate;                                                //heart rate value
		int8_t   ch_hr_valid;                                                 //indicator to show if the heart rate calculation is valid
		uint32_t aun_ir_buffer[BUFFER_SIZE];                                  //infrared LED sensor data
		uint32_t aun_red_buffer[BUFFER_SIZE];                                 //red LED sensor data
		int32_t  i;
		int32_t  average_hr;
		float    average_spo2;
		int32_t  nbr_readings;

		int            run_time = 6;  //default to 30 second run time
		struct timeval time_start, time_now;

		maxim_max30102_init();

		Log_Debug("\nRunning test for %d seconds.\n", run_time);
		Log_Debug("HeartRate Click Revision: 0x%02X\n", max30102_get_revision());
		Log_Debug("HeartRate Click Part ID:  0x%02X\n\n", max30102_get_part_id());
		Log_Debug("Begin ... Place your finger on the sensor\n\n");

		gettimeofday(&time_start, NULL);
		time_now = time_start;
		average_hr = nbr_readings = 0;
		average_spo2 = 0.0;

		while (difftime(time_now.tv_sec, time_start.tv_sec) < run_time) {
			//buffer length of BUFFER_SIZE stores ST seconds of samples running at FS sps
			//read BUFFER_SIZE samples, and determine the signal range
			GPIO_Value_Type intVal;
			for (i = 0; i < BUFFER_SIZE; i++) {
				do {
					GPIO_GetValue(intPinFd, &intVal);
				} while (intVal == 1);                               //wait until the interrupt pin asserts

				maxim_max30102_read_fifo((aun_red_buffer + i), (aun_ir_buffer + i));   //read from MAX30102 FIFO
				Log_Debug("*");
			}
			Log_Debug("\n");
			//calculate heart rate and SpO2 after BUFFER_SIZE samples (ST seconds of samples) using Robert's method
			rf_heart_rate_and_oxygen_saturation(aun_ir_buffer, BUFFER_SIZE, aun_red_buffer, &n_spo2, &ch_spo2_valid, &n_heart_rate, &ch_hr_valid, &ratio, &correl);

			if (ch_hr_valid && ch_spo2_valid) {
				Log_Debug("Blood Oxygen Level (SpO2)=%.2f%% [normal is 95-100%%], Heart Rate=%d BPM [normal resting for adults is 60-100 BPM]\n", n_spo2, n_heart_rate);

				average_hr += n_heart_rate;
				average_spo2 += n_spo2;
				nbr_readings++;
			}
			else
				Log_Debug("ch_hr_valid=%d, ch_spo2_valid=%d\n", ch_hr_valid, ch_spo2_valid);

			gettimeofday(&time_now, NULL);
		}

		if (nbr_readings > 0) 
		{
			Log_Debug("\n\nAverage Blood Oxygen Level = %.2f%%\n", average_spo2 / (float)nbr_readings);
			Log_Debug("        Average Heart Rate = %d BPM\n", average_hr / nbr_readings);

			char n_heart_rate_string[10];
			int n_heart_rate_string_len = snprintf(n_heart_rate_string, 10, "%d", average_hr / nbr_readings);
			if (n_heart_rate_string_len > 0) {
				SendTelemetry("Heart_rate", n_heart_rate_string);
			}

			char n_spo2_string[10];
			int n_spo2_string_len = snprintf(n_spo2_string, 10, "%3.2f", average_spo2 / (float)nbr_readings);
			if (n_spo2_string_len > 0) {
				SendTelemetry("SpO2", n_spo2_string);
			}
		}

		max301024_shut_down(1);
    }
}
