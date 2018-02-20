/* Copyright (c) 2010-2017 Benjamin Dobell, Glass Echidna

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.*/

// C Standard Library
#include <cstdio>

// libusb
#include <libusb.h>

// Heimdall
#include "BeginDumpPacket.h"
#include "BeginSessionPacket.h"
#include "BridgeManager.h"
#include "DeviceTypePacket.h"
#include "DumpPartFileTransferPacket.h"
#include "DumpPartPitFilePacket.h"
#include "DumpResponse.h"
#include "EndModemFileTransferPacket.h"
#include "EndPhoneFileTransferPacket.h"
#include "EndPitFileTransferPacket.h"
#include "EndSessionPacket.h"
#include "FilePartSizePacket.h"
#include "FileTransferPacket.h"
#include "FlashPartFileTransferPacket.h"
#include "FlashPartPitFilePacket.h"
#include "InboundPacket.h"
#include "Interface.h"
#include "OutboundPacket.h"
#include "PitFilePacket.h"
#include "PitFileResponse.h"
#include "ReceiveFilePartPacket.h"
#include "ResponsePacket.h"
#include "SendFilePartPacket.h"
#include "SendFilePartResponse.h"
#include "SessionSetupPacket.h"
#include "SessionSetupResponse.h"
#include <sstream>  // std::ostringstream
#include <algorithm>    // std::copy
#include <iterator> // std::ostream_iterator
#include <iostream> // std::cout

// Future versions of libusb will use usb_interface instead of interface.
#ifndef usb_interface
#define usb_interface interface
#endif

#define USB_CLASS_CDC_DATA 0x0A

using namespace libpit;
using namespace Heimdall;

const DeviceIdentifier BridgeManager::supportedDevices[BridgeManager::kSupportedDeviceCount] = {
	DeviceIdentifier(BridgeManager::kVidSamsung, BridgeManager::kPidGalaxyS),
	DeviceIdentifier(BridgeManager::kVidSamsung, BridgeManager::kPidGalaxyS2),
	DeviceIdentifier(BridgeManager::kVidSamsung, BridgeManager::kPidDroidCharge)
};

enum
{
	kFileTransferSequenceMaxLengthDefault = 800,
	kFileTransferPacketSizeDefault = 131072,
	kFileTransferSequenceTimeoutDefault = 30000 // 30 seconds
};

int BridgeManager::FindDeviceInterface(void)
{
	Interface::Print("Detecting device...\n");
	char location[32];
  uint8_t path[8];
	struct libusb_device **devices;
	int deviceCount = libusb_get_device_list(libusbContext, &devices);

	for (int deviceIndex = 0; deviceIndex < deviceCount; deviceIndex++)
	{
		libusb_device_descriptor descriptor;
		libusb_get_device_descriptor(devices[deviceIndex], &descriptor);
		int r  =0;
		int bus = libusb_get_bus_number(devices[deviceIndex]);
		 r = libusb_get_port_numbers(devices[deviceIndex], path, sizeof(path));
		 sprintf(location, "%d", bus);
		 int k;
		 for (k=0; k<r; k++) {
					char s[8];
					sprintf(s, "%s%d", k==0 ? "-" : ".", path[k]);
					strcat(location, s);
			}
			/*
			*
			* Ricardo Rodriguez: Multiport support
			*
			*/
			bool foundPath = strcmp(this->usbPath,location) ? false : true;
			//printf("%s\t\t%s\n",this->usbPath,location );
			//printf("%s\n", foundPath ? "true" : "false");
		for (int i = 0; i < BridgeManager::kSupportedDeviceCount; i++)
		{

			if (descriptor.idVendor == supportedDevices[i].vendorId && descriptor.idProduct == supportedDevices[i].productId && foundPath)
			{

				heimdallDevice = devices[deviceIndex];
				libusb_ref_device(heimdallDevice);
				break;
			}
		}

		if (heimdallDevice)
			break;
	}

	libusb_free_device_list(devices, deviceCount);

	if (!heimdallDevice)
	{
		Interface::PrintDeviceDetectionFailed();
		return (BridgeManager::kInitialiseDeviceNotDetected);
	}

	int result = libusb_open(heimdallDevice, &deviceHandle);
	if (result != LIBUSB_SUCCESS)
	{
		Interface::PrintError("Failed to access device. libusb error: %d\n", result);
		return (BridgeManager::kInitialiseFailed);
	}

	libusb_device_descriptor deviceDescriptor;
	result = libusb_get_device_descriptor(heimdallDevice, &deviceDescriptor);
	if (result != LIBUSB_SUCCESS)
	{
		Interface::PrintError("Failed to retrieve device description\n");
		return (BridgeManager::kInitialiseFailed);
	}

	if (verbose)
	{
		unsigned char stringBuffer[128];

		if (libusb_get_string_descriptor_ascii(deviceHandle, deviceDescriptor.iManufacturer,
			stringBuffer, 128) >= 0)
		{
			Interface::Print("      Manufacturer: \"%s\"\n", stringBuffer);
		}

		if (libusb_get_string_descriptor_ascii(deviceHandle, deviceDescriptor.iProduct,
			stringBuffer, 128) >= 0)
		{
			Interface::Print("           Product: \"%s\"\n", stringBuffer);
		}

		if (libusb_get_string_descriptor_ascii(deviceHandle, deviceDescriptor.iSerialNumber,
			stringBuffer, 128) >= 0)
		{
			Interface::Print("         Serial No: \"%s\"\n", stringBuffer);
		}

		Interface::Print("\n            length: %d\n", deviceDescriptor.bLength);
		Interface::Print("      device class: %d\n", deviceDescriptor.bDeviceClass);
		Interface::Print("               S/N: %d\n", deviceDescriptor.iSerialNumber);
		Interface::Print("           VID:PID: %04X:%04X\n", deviceDescriptor.idVendor, deviceDescriptor.idProduct);
		Interface::Print("         bcdDevice: %04X\n", deviceDescriptor.bcdDevice);
		Interface::Print("   iMan:iProd:iSer: %d:%d:%d\n", deviceDescriptor.iManufacturer, deviceDescriptor.iProduct,
			deviceDescriptor.iSerialNumber);
		Interface::Print("          nb confs: %d\n", deviceDescriptor.bNumConfigurations);
	}

	libusb_config_descriptor *configDescriptor;
	result = libusb_get_config_descriptor(heimdallDevice, 0, &configDescriptor);

	if (result != LIBUSB_SUCCESS || !configDescriptor)
	{
		Interface::PrintError("Failed to retrieve config descriptor\n");
		return (BridgeManager::kInitialiseFailed);
	}

	interfaceIndex = -1;
	altSettingIndex = -1;

	for (int i = 0; i < configDescriptor->bNumInterfaces; i++)
	{
		for (int j = 0 ; j < configDescriptor->usb_interface[i].num_altsetting; j++)
		{
			if (verbose)
			{
				Interface::Print("\ninterface[%d].altsetting[%d]: num endpoints = %d\n",
					i, j, configDescriptor->usb_interface[i].altsetting[j].bNumEndpoints);
				Interface::Print("   Class.SubClass.Protocol: %02X.%02X.%02X\n",
					configDescriptor->usb_interface[i].altsetting[j].bInterfaceClass,
					configDescriptor->usb_interface[i].altsetting[j].bInterfaceSubClass,
					configDescriptor->usb_interface[i].altsetting[j].bInterfaceProtocol);
			}

			int inEndpointAddress = -1;
			int outEndpointAddress = -1;

			for (int k = 0; k < configDescriptor->usb_interface[i].altsetting[j].bNumEndpoints; k++)
			{
				const libusb_endpoint_descriptor *endpoint = &configDescriptor->usb_interface[i].altsetting[j].endpoint[k];

				if (verbose)
				{
					Interface::Print("       endpoint[%d].address: %02X\n", k, endpoint->bEndpointAddress);
					Interface::Print("           max packet size: %04X\n", endpoint->wMaxPacketSize);
					Interface::Print("          polling interval: %02X\n", endpoint->bInterval);
				}

				if (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN)
					inEndpointAddress = endpoint->bEndpointAddress;
				else
					outEndpointAddress = endpoint->bEndpointAddress;
			}

			if (interfaceIndex < 0
				&& configDescriptor->usb_interface[i].altsetting[j].bNumEndpoints == 2
				&& configDescriptor->usb_interface[i].altsetting[j].bInterfaceClass == USB_CLASS_CDC_DATA
				&& inEndpointAddress != -1
				&& outEndpointAddress != -1)
			{
				interfaceIndex = i;
				altSettingIndex = j;
				inEndpoint = inEndpointAddress;
				outEndpoint = outEndpointAddress;
			}
		}
	}

	libusb_free_config_descriptor(configDescriptor);

	if (interfaceIndex < 0)
	{
		Interface::PrintError("Failed to find correct interface configuration\n");
		return (BridgeManager::kInitialiseFailed);
	}

	return (BridgeManager::kInitialiseSucceeded);
}

bool BridgeManager::ClaimDeviceInterface(void)
{
	Interface::Print("Claiming interface...\n");

	int result = libusb_claim_interface(deviceHandle, interfaceIndex);

#ifdef OS_LINUX

	if (result != LIBUSB_SUCCESS)
	{
		detachedDriver = true;
		Interface::Print("Attempt failed. Detaching driver...\n");
		libusb_detach_kernel_driver(deviceHandle, interfaceIndex);
		Interface::Print("Claiming interface again...\n");
		result = libusb_claim_interface(deviceHandle, interfaceIndex);
	}

#endif

	if (result != LIBUSB_SUCCESS)
	{
		Interface::PrintError("Claiming interface failed!\n");
		return (false);
	}

	interfaceClaimed = true;

	return (true);
}

bool BridgeManager::SetupDeviceInterface(void)
{
	Interface::Print("Setting up interface...\n");

	int result = libusb_set_interface_alt_setting(deviceHandle, interfaceIndex, altSettingIndex);

	if (result != LIBUSB_SUCCESS)
	{
		Interface::PrintError("Setting up interface failed!\n");
		return (false);
	}

	Interface::Print("\n");
	return (true);
}

void BridgeManager::ReleaseDeviceInterface(void)
{
	Interface::Print("Releasing device interface...\n");

	libusb_release_interface(deviceHandle, interfaceIndex);

#ifdef OS_LINUX

	if (detachedDriver)
	{
		Interface::Print("Re-attaching kernel driver...\n");
		libusb_attach_kernel_driver(deviceHandle, interfaceIndex);
	}

#endif

	interfaceClaimed = false;
	Interface::Print("\n");
}

bool BridgeManager::InitialiseProtocol(void)
{
	Interface::Print("Initialising protocol...\n");

	unsigned char dataBuffer[7];

	// Send "ODIN"
	memcpy(dataBuffer, "ODIN", 4);
	memset(dataBuffer + 4, 0, 1);

	if (!SendBulkTransfer(dataBuffer, 4, 1000))
	{
		Interface::PrintError("Failed to send handshake!");
	}

	// Expect "LOKE"
	memset(dataBuffer, 0, 7);

	int dataTransferred = 0;

	int result = libusb_bulk_transfer(deviceHandle, inEndpoint, dataBuffer, 7, &dataTransferred, 1000);

	if (result != LIBUSB_SUCCESS)
	{
		if (verbose)
			Interface::PrintError("Failed to receive handshake response. Result: %d\n", result);
	}
	else
	{
		if (dataTransferred == 4 && memcmp(dataBuffer, "LOKE", 4) == 0)
		{
			// Successfully received "LOKE"
			Interface::Print("Protocol initialisation successful.\n\n");
			return (true);
		}
		else
		{
			if (verbose)
				Interface::PrintError("Expected: \"LOKE\"\nReceived: \"%s\"\n", dataBuffer);

			Interface::PrintError("Unexpected handshake response!\n");
		}
	}
	Interface::Print("Expected: \"LOKE\"\nReceived: \"%s\"\n", dataBuffer);
	Interface::PrintError("Protocol initialisation failed!\n\n");
	return (true);
}

BridgeManager::BridgeManager(bool verbose)
{
	this->verbose = verbose;

	libusbContext = nullptr;
	deviceHandle = nullptr;
	heimdallDevice = nullptr;

	inEndpoint = -1;
	outEndpoint = -1;
	interfaceIndex = -1;
	altSettingIndex = -1;
	isOldPhone = false;

	interfaceClaimed = false;

#ifdef OS_LINUX

	detachedDriver = false;

#endif

	fileTransferSequenceMaxLength = kFileTransferSequenceMaxLengthDefault;
	fileTransferPacketSize = kFileTransferPacketSizeDefault;
	fileTransferSequenceTimeout = kFileTransferSequenceTimeoutDefault;

	usbLogLevel = UsbLogLevel::Default;
}

BridgeManager::~BridgeManager()
{
	if (interfaceClaimed)
		ReleaseDeviceInterface();

	if (deviceHandle)
		libusb_close(deviceHandle);

	if (heimdallDevice)
		libusb_unref_device(heimdallDevice);

	if (libusbContext)
		libusb_exit(libusbContext);
}

bool BridgeManager::DetectDevice(void)
{
	// Initialise libusb
	int result = libusb_init(&libusbContext);

	if (result != LIBUSB_SUCCESS)
	{
		Interface::PrintError("Failed to initialise libusb. libusb error: %d\n", result);
		return (false);
	}

	// Setup libusb log level.
	switch (usbLogLevel)
	{
		case UsbLogLevel::None:
			libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_NONE);
			break;

		case UsbLogLevel::Error:
			libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_ERROR);
			break;

		case UsbLogLevel::Warning:
			libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_WARNING);
			break;

		case UsbLogLevel::Info:
			libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_INFO);
			break;

		case UsbLogLevel::Debug:
			libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_DEBUG);
			break;
	}

	// Get handle to Galaxy S device
	struct libusb_device **devices;
	int deviceCount = libusb_get_device_list(libusbContext, &devices);
	int i = 0, j = 0;
	uint8_t path[8];
	int r  =0;
	char location[32];
	for (int deviceIndex = 0; deviceIndex < deviceCount; deviceIndex++)
	{
		libusb_device_descriptor descriptor;
		libusb_get_device_descriptor(devices[deviceIndex], &descriptor);

		for (int i = 0; i < BridgeManager::kSupportedDeviceCount; i++)
		{
			if (descriptor.idVendor == supportedDevices[i].vendorId && descriptor.idProduct == supportedDevices[i].productId)
			{
				//libusb_free_device_list(devices, deviceCount);


				 int bus = libusb_get_bus_number(devices[deviceIndex]);
				 r = libusb_get_port_numbers(devices[deviceIndex], path, sizeof(path));
				 sprintf(location, "%d", bus);
				 int k;
        for (k=0; k<r; k++) {
            char s[8];
            sprintf(s, "%s%d", k==0 ? "-" : ".", path[k]);
            strcat(location, s);
        }

				Interface::Print("Dev:\t%s\n", location);

			}
		}
	}

	libusb_free_device_list(devices, deviceCount);
	/*
	* Ricardo Rodriguez: I don't care about the missing dev, as we are looking for devs
	*/
	//Interface::PrintDeviceDetectionFailed();
	return (true);
}

int BridgeManager::Initialise(bool resume)
{
	Interface::Print("Initialising connection...\n");

	// Initialise libusb
	int result = libusb_init(&libusbContext);

	if (result != LIBUSB_SUCCESS)
	{
		Interface::PrintError("Failed to initialise libusb. libusb error: %d\n", result);
		Interface::Print("Failed to connect to device!");
		return (BridgeManager::kInitialiseFailed);
	}

	// Setup libusb log level.
	switch (usbLogLevel)
	{
		case UsbLogLevel::None:
			libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_NONE);
			break;

		case UsbLogLevel::Error:
			libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_ERROR);
			break;

		case UsbLogLevel::Warning:
			libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_WARNING);
			break;

		case UsbLogLevel::Info:
			libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_INFO);
			break;

		case UsbLogLevel::Debug:
			libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_DEBUG);
			break;
	}

	result = FindDeviceInterface();

	if (result != BridgeManager::kInitialiseSucceeded)
		return (result);

	if (!ClaimDeviceInterface())
		return (BridgeManager::kInitialiseFailed);

	if (!SetupDeviceInterface())
		return (BridgeManager::kInitialiseFailed);

	if (!resume)
	{
		if (!InitialiseProtocol())
			return (BridgeManager::kInitialiseFailed);
	}

	return (BridgeManager::kInitialiseSucceeded);
}

bool BridgeManager::BeginSession(void)
{
	Interface::Print("Beginning session...\n");

	if(isOldPhone){
		BeginSessionPacket beginSessionPacket;

		if (!SendPacket(&beginSessionPacket))
		{
			Interface::PrintError("Failed to begin session!\n");
			return (false);
		}
	}else{
		BeginSessionPacketNew beginSessionPacketNew;

		if (!SendPacket(&beginSessionPacketNew))
		{
			Interface::PrintError("Failed to begin session!\n");
			return (false);
		}
	}

	SessionSetupResponse beginSessionResponse;
	ReceivePacket(&beginSessionResponse);
	/*
	* Ricardo Rodriguez: Here we need to review on newest versions why fail but don't affect system...
	*
	*/
	// if (!ReceivePacket(&beginSessionResponse))
	// 	return (false);

	unsigned long long deviceDefaultPacketSize = beginSessionResponse.GetResult();

	Interface::Print("\nSome devices may take up to 2 minutes to respond.\nPlease be patient!\n\n");
	Sleep(3000); // Give the user time to read the message.

	
	if (deviceDefaultPacketSize != 0) // 0 means changing the packet size is not supported.
	{
		fileTransferSequenceTimeout = 120000; // 2 minutes!
		fileTransferPacketSize = 1048576; // 1 MiB
		fileTransferSequenceMaxLength = 30; // Therefore, fileTransferPacketSize * fileTransferSequenceMaxLength == 30 MiB per sequence.

		FilePartSizePacket filePartSizePacket(fileTransferPacketSize);

		if (!SendPacket(&filePartSizePacket))
		{
			Interface::PrintError("Failed to send file part size packet!\n");
			return (false);
		}

		SessionSetupResponse filePartSizeResponse;

		if (!ReceivePacket(&filePartSizeResponse))
			return (false);

		if (filePartSizeResponse.GetResult() != 0)
		{
			Interface::PrintError("Unexpected file part size response!\nExpected: 0\nReceived: %d\n", filePartSizeResponse.GetResult());
			return (false);
		}
	}

	Interface::Print("Session begun.\n\n");
	return (true);
}

bool BridgeManager::EndSession(bool reboot) const
{
	Interface::Print("Ending session...\n");

	EndSessionPacket *endSessionPacket = new EndSessionPacket(EndSessionPacket::kRequestEndSession);
	bool success = SendPacket(endSessionPacket);
	delete endSessionPacket;

	if (!success)
	{
		Interface::PrintError("Failed to send end session packet!\n");

		return (false);
	}

	ResponsePacket *endSessionResponse = new ResponsePacket(ResponsePacket::kResponseTypeEndSession);
	success = ReceivePacket(endSessionResponse);
	delete endSessionResponse;

	if (!success)
	{
		Interface::PrintError("Failed to receive session end confirmation!\n");

		return (false);
	}

	if (reboot)
	{
		Interface::Print("Rebooting device...\n");

		EndSessionPacket *rebootDevicePacket = new EndSessionPacket(EndSessionPacket::kRequestRebootDevice);
		bool success = SendPacket(rebootDevicePacket);
		delete rebootDevicePacket;

		if (!success)
		{
			Interface::PrintError("Failed to send reboot device packet!\n");

			return (false);
		}

		ResponsePacket *rebootDeviceResponse = new ResponsePacket(ResponsePacket::kResponseTypeEndSession);
		success = ReceivePacket(rebootDeviceResponse);
		delete rebootDeviceResponse;

		if (!success)
		{
			Interface::PrintError("Failed to receive reboot confirmation!\n");

			return (false);
		}
	}

	return (true);
}

bool BridgeManager::SendBulkTransfer(unsigned char *data, int length, int timeout, bool retry) const
{
	int dataTransferred;
	int result = libusb_bulk_transfer(deviceHandle, outEndpoint, data, length, &dataTransferred, timeout);

	if (result != LIBUSB_SUCCESS && retry)
	{
		static const int retryDelay = 250;

		if (verbose)
			Interface::PrintError("libusb error %d whilst sending bulk transfer.", result);

		// Retry
		for (int i = 0; i < 5; i++)
		{
			if (verbose)
				Interface::PrintErrorSameLine(" Retrying...\n");

			// Wait longer each retry
			Sleep(retryDelay * (i + 1));

			result = libusb_bulk_transfer(deviceHandle, outEndpoint, data, length, &dataTransferred, timeout);

			if (result == LIBUSB_SUCCESS)
				break;

			if (verbose)
				Interface::PrintError("libusb error %d whilst sending bulk transfer.", result);
		}

		if (verbose)
			Interface::PrintErrorSameLine("\n");
	}

	return (result == LIBUSB_SUCCESS && dataTransferred == length);
}

int BridgeManager::ReceiveBulkTransfer(unsigned char *data, int length, int timeout, bool retry) const
{
	if (data == nullptr)
	{
		// HACK: It seems WinUSB ignores us when we try to read with length zero.
		static unsigned char dummyData;
		data = &dummyData;
		length = 1;
	}

	int dataTransferred;
	int result = libusb_bulk_transfer(deviceHandle, inEndpoint, data, length, &dataTransferred, timeout);

	if (result != LIBUSB_SUCCESS && retry)
	{
		static const int retryDelay = 250;

		if (verbose)
			Interface::PrintError("libusb error %d whilst receiving bulk transfer.", result);

		// Retry
		for (int i = 0; i < 5; i++)
		{
			if (verbose)
				Interface::PrintErrorSameLine(" Retrying...\n");

			// Wait longer each retry
			Sleep(retryDelay * (i + 1));

			result = libusb_bulk_transfer(deviceHandle, inEndpoint, data, length, &dataTransferred, timeout);

			if (result == LIBUSB_SUCCESS)
				break;

			if (verbose)
				Interface::PrintError("libusb error %d whilst receiving bulk transfer.", result);
		}

		if (verbose)
			Interface::PrintErrorSameLine("\n");
	}

	if (result != LIBUSB_SUCCESS)
		return (result);

	return (dataTransferred);
}

bool BridgeManager::SendPacket(OutboundPacket *packet, int timeout, int emptyTransferFlags) const
{
	packet->Pack();

	if (emptyTransferFlags & kEmptyTransferBefore)
	{
		if (!SendBulkTransfer(nullptr, 0, kDefaultTimeoutEmptyTransfer, false) && verbose)
		{
			Interface::PrintWarning("Empty bulk transfer before sending packet failed. Continuing anyway...\n");
		}
	}

	if (!SendBulkTransfer(packet->GetData(), packet->GetSize(), timeout))
		return (false);

	if (emptyTransferFlags & kEmptyTransferAfter)
	{
		if (!SendBulkTransfer(nullptr, 0, kDefaultTimeoutEmptyTransfer, false) && verbose)
		{
			Interface::PrintWarning("Empty bulk transfer after sending packet failed. Continuing anyway...\n");
		}
	}

	return (true);
}


bool BridgeManager::ReceivePacket(InboundPacket *packet, int timeout, int emptyTransferFlags) const
{
	if (emptyTransferFlags & kEmptyTransferBefore)
	{
		if (ReceiveBulkTransfer(nullptr, 0, kDefaultTimeoutEmptyTransfer, false) < 0 && verbose)
		{
			Interface::PrintWarning("Empty bulk transfer before receiving packet failed. Continuing anyway...\n");
		}
	}

	int receivedSize = ReceiveBulkTransfer(packet->GetData(), packet->GetSize(), timeout);

	if (receivedSize < 0)
		return (false);

	if (receivedSize != packet->GetSize() && !packet->IsSizeVariable())
	{
		if (verbose)
			Interface::PrintError("Incorrect packet size received - expected size = %d, received size = %d.\n", packet->GetSize(), receivedSize);

		return (false);
	}

	packet->SetReceivedSize(receivedSize);

	bool unpacked = packet->Unpack();

	if (!unpacked && verbose){
		Interface::PrintError("Failed to unpack received packet.\n");

	}

	if (emptyTransferFlags & kEmptyTransferAfter)
	{
		if (ReceiveBulkTransfer(nullptr, 0, kDefaultTimeoutEmptyTransfer, false) < 0 && verbose)
		{
			Interface::PrintWarning("Empty bulk transfer after receiving packet failed. Continuing anyway...\n");
		}
	}

	return (unpacked);
}

bool BridgeManager::RequestDeviceType(unsigned long long request, int *result) const
{
	DeviceTypePacket deviceTypePacket;
	bool success = SendPacket(&deviceTypePacket);

	if (!success)
	{
		Interface::PrintError("Failed to request device info packet!\n");

		if (verbose)
			Interface::PrintError("Failed request: %u\n", request);

		return (false);
	}

	SessionSetupResponse deviceTypeResponse;

	if (!ReceivePacket(&deviceTypeResponse))
		return (false);

	*result = deviceTypeResponse.GetResult();

	return (true);
}

bool BridgeManager::SendPitData(const PitData *pitData) const
{
	unsigned long long pitBufferSize = pitData->GetPaddedSize();

	// Start file transfer
	PitFilePacket *pitFilePacket = new PitFilePacket(PitFilePacket::kRequestFlash);
	bool success = SendPacket(pitFilePacket);
	delete pitFilePacket;

	if (!success)
	{
		Interface::PrintError("Failed to initialise PIT file transfer!\n");
		return (false);
	}

	PitFileResponse *pitFileResponse = new PitFileResponse();
	success = ReceivePacket(pitFileResponse);
	delete pitFileResponse;

	if (!success)
	{
		Interface::PrintError("Failed to confirm transfer initialisation!\n");
		return (false);
	}

	// Transfer file size
	FlashPartPitFilePacket *flashPartPitFilePacket = new FlashPartPitFilePacket(pitBufferSize);
	success = SendPacket(flashPartPitFilePacket);
	delete flashPartPitFilePacket;

	if (!success)
	{
		Interface::PrintError("Failed to send PIT file part information!\n");
		return (false);
	}

	pitFileResponse = new PitFileResponse();
	success = ReceivePacket(pitFileResponse);
	delete pitFileResponse;

	if (!success)
	{
		Interface::PrintError("Failed to confirm sending of PIT file part information!\n");
		return (false);
	}

	// Create packed in-memory PIT file

	unsigned char *pitBuffer = new unsigned char[pitBufferSize];
	memset(pitBuffer, 0, pitBufferSize);

	pitData->Pack(pitBuffer);

	// Flash pit file
	SendFilePartPacket *sendFilePartPacket = new SendFilePartPacket(pitBuffer, pitBufferSize);
	success = SendPacket(sendFilePartPacket);
	delete sendFilePartPacket;

	delete [] pitBuffer;

	if (!success)
	{
		Interface::PrintError("Failed to send file part packet!\n");
		return (false);
	}

	pitFileResponse = new PitFileResponse();
	success = ReceivePacket(pitFileResponse);
	delete pitFileResponse;

	if (!success)
	{
		Interface::PrintError("Failed to receive PIT file part response!\n");
		//return (false);
	}

	// End pit file transfer
	EndPitFileTransferPacket *endPitFileTransferPacket = new EndPitFileTransferPacket(pitBufferSize);
	success = SendPacket(endPitFileTransferPacket);
	delete endPitFileTransferPacket;

	if (!success)
	{
		Interface::PrintError("Failed to send end PIT file transfer packet!\n");
		return (false);
	}

	pitFileResponse = new PitFileResponse();
	success = ReceivePacket(pitFileResponse);
	delete pitFileResponse;

	if (!success)
	{
		Interface::PrintError("Failed to confirm end of PIT file transfer!\n");
		return (false);
	}

	return (true);
}

int BridgeManager::ReceivePitFile(unsigned char **pitBuffer) const
{
	*pitBuffer = nullptr;

	bool success;

	// Start file transfer
	PitFilePacket *pitFilePacket = new PitFilePacket(PitFilePacket::kRequestDump);
	success = SendPacket(pitFilePacket);
	delete pitFilePacket;

	if (!success)
	{
		Interface::PrintError("Failed to request receival of PIT file!\n");
		return (0);
	}

	PitFileResponse *pitFileResponse = new PitFileResponse();
	success = ReceivePacket(pitFileResponse);
	unsigned long long fileSize = pitFileResponse->GetFileSize();
	delete pitFileResponse;

	if (!success)
	{
		Interface::PrintError("Failed to receive PIT file size!\n");
		return (0);
	}

	unsigned long long transferCount = fileSize / ReceiveFilePartPacket::kDataSize;
	if (fileSize % ReceiveFilePartPacket::kDataSize != 0)
		transferCount++;

	unsigned char *buffer = new unsigned char[fileSize];
	int offset = 0;

	for (unsigned long long i = 0; i < transferCount; i++)
	{
		DumpPartPitFilePacket *requestPacket = new DumpPartPitFilePacket(i);
		success = SendPacket(requestPacket);
		delete requestPacket;

		if (!success)
		{
			Interface::PrintError("Failed to request PIT file part #%d!\n", i);
			delete [] buffer;
			return (0);
		}

		int receiveEmptyTransferFlags = (i == transferCount - 1) ? kEmptyTransferAfter : kEmptyTransferNone;

		ReceiveFilePartPacket *receiveFilePartPacket = new ReceiveFilePartPacket();
		success = ReceivePacket(receiveFilePartPacket, kDefaultTimeoutReceive, receiveEmptyTransferFlags);

		if (!success)
		{
			Interface::PrintError("Failed to receive PIT file part #%d!\n", i);
			delete receiveFilePartPacket;
			delete [] buffer;
			return (0);
		}

		// Copy the whole packet data into the buffer.
		memcpy(buffer + offset, receiveFilePartPacket->GetData(), receiveFilePartPacket->GetReceivedSize());
		offset += receiveFilePartPacket->GetReceivedSize();

		delete receiveFilePartPacket;
	}

	// End file transfer
	pitFilePacket = new PitFilePacket(PitFilePacket::kRequestEndTransfer);
	success = SendPacket(pitFilePacket);
	delete pitFilePacket;

	if (!success)
	{
		Interface::PrintError("Failed to send request to end PIT file transfer!\n");
		delete [] buffer;
		return (0);
	}

	pitFileResponse = new PitFileResponse();
	success = ReceivePacket(pitFileResponse);
	delete pitFileResponse;

	if (!success)
	{
		Interface::PrintError("Failed to receive end PIT file transfer verification!\n");
		delete [] buffer;
		return (0);
	}

	*pitBuffer = buffer;
	return (fileSize);
}

int BridgeManager::DownloadPitFile(unsigned char **pitBuffer) const
{
	Interface::Print("Downloading device's PIT file...\n");

	int devicePitFileSize = ReceivePitFile(pitBuffer);

	if (!*pitBuffer)
	{
		Interface::PrintError("Failed to download PIT file!\n");
		return (0);
	}

	Interface::Print("PIT file download successful.\n\n");
	return (devicePitFileSize);
}

bool BridgeManager::SendFile(FILE *file, unsigned long long destination, unsigned long long deviceType, unsigned long long fileIdentifier) const
{
	Interface::Print("Start File sending");
	if (destination != EndFileTransferPacket::kDestinationModem && destination != EndFileTransferPacket::kDestinationPhone)
	{
		Interface::PrintError("Attempted to send file to unknown destination!\n");
		return (false);
	}

	if (destination == EndFileTransferPacket::kDestinationModem && fileIdentifier != 0xFFFFFFFF)
	{
		Interface::PrintError("The modem file does not have an identifier!\n");
		return (false);
	}

	FileTransferPacket *flashFileTransferPacket = new FileTransferPacket(FileTransferPacket::kRequestFlash);
	bool success = SendPacket(flashFileTransferPacket);
	delete flashFileTransferPacket;

	if (!success)
	{
		Interface::PrintError("Failed to initialise file transfer!\n");
		return (false);
	}

	FileSeek(file, 0, SEEK_END);
	unsigned long long fileSize = (unsigned long long)FileTell(file); //Ricardo original unsigned long long
	FileRewind(file);

	ResponsePacket *fileTransferResponse = new ResponsePacket(ResponsePacket::kResponseTypeFileTransfer);
	success = ReceivePacket(fileTransferResponse);
	delete fileTransferResponse;

	if (!success)
	{
		Interface::PrintError("Failed to confirm transfer initialisation!\n");
		return (false);
	}

	unsigned long long sequenceCount = fileSize / (fileTransferSequenceMaxLength * fileTransferPacketSize); //Ricardo original unsigned long long
	unsigned long long lastSequenceSize = fileTransferSequenceMaxLength; //Ricardo original unsigned long long
	unsigned long long partialPacketByteCount = fileSize % fileTransferPacketSize; //Ricardo original unsigned long long

	if (fileSize % (fileTransferSequenceMaxLength * fileTransferPacketSize) != 0)
	{
		sequenceCount++;

		unsigned long long lastSequenceBytes = fileSize % (fileTransferSequenceMaxLength * fileTransferPacketSize); //Ricardo original unsigned long long
		lastSequenceSize = lastSequenceBytes / fileTransferPacketSize;

		if (partialPacketByteCount != 0)
			lastSequenceSize++;
	}

	unsigned long long bytesTransferred = 0; //Ricardo original unsigned long long
	unsigned long long  currentPercent; //Ricardo original unsigned long long
	unsigned long long previousPercent = 0; //Ricardo original unsigned long long
	Interface::Print("0%%");

	for (unsigned long long sequenceIndex = 0; sequenceIndex < sequenceCount; sequenceIndex++)
	{
		bool isLastSequence = (sequenceIndex == sequenceCount - 1);
		unsigned long long sequenceSize = (isLastSequence) ? lastSequenceSize : fileTransferSequenceMaxLength; //Ricardo original unsigned long long
		unsigned long long sequenceTotalByteCount = sequenceSize * fileTransferPacketSize; //Ricardo original unsigned long long
		
				

		FlashPartFileTransferPacket *beginFileTransferPacket = new FlashPartFileTransferPacket(sequenceTotalByteCount);
		success = SendPacket(beginFileTransferPacket);
		delete beginFileTransferPacket;

		if (!success)
		{
			Interface::PrintErrorSameLine("\n");
			Interface::PrintError("Failed to begin file transfer sequence!\n");
			return (false);
		}

		fileTransferResponse = new ResponsePacket(ResponsePacket::kResponseTypeFileTransfer);
		bool success = ReceivePacket(fileTransferResponse);
		delete fileTransferResponse;

		if (!success)
		{
			Interface::PrintErrorSameLine("\n");
			Interface::PrintError("Failed to confirm beginning of file transfer sequence!\n");
			return (false);
		}

		for (unsigned long long filePartIndex = 0; filePartIndex < sequenceSize; filePartIndex++) //Ricardo original unsigned long long
		{
			// NOTE: This empty transfer thing is entirely ridiculous, but sadly it seems to be required.
			int sendEmptyTransferFlags = (filePartIndex == 0) ? kEmptyTransferNone : kEmptyTransferBefore;
			// Send
			SendFilePartPacket *sendFilePartPacket = new SendFilePartPacket(file, fileTransferPacketSize);
			success = SendPacket(sendFilePartPacket, kDefaultTimeoutSend, sendEmptyTransferFlags);
			delete sendFilePartPacket;

			if (!success)
			{
				Interface::PrintErrorSameLine("\n");
				Interface::PrintError("Failed to send file part packet!\n");
				return (false);
			}

			// Response
			SendFilePartResponse *sendFilePartResponse = new SendFilePartResponse();
			success = ReceivePacket(sendFilePartResponse);
			int receivedPartIndex = sendFilePartResponse->GetPartIndex();
			delete sendFilePartResponse;

			if (!success)
			{
				Interface::PrintErrorSameLine("\n");
				Interface::PrintError("Failed to receive file part response!\n");

				for (int retry = 0; retry < 4; retry++)
				{
					Interface::PrintErrorSameLine("\n");
					Interface::PrintError("Retrying...");

					// Send
					sendFilePartPacket = new SendFilePartPacket(file, fileTransferPacketSize);
					success = SendPacket(sendFilePartPacket, kDefaultTimeoutSend, sendEmptyTransferFlags);
					delete sendFilePartPacket;

					if (!success)
					{
						Interface::PrintErrorSameLine("\n");
						Interface::PrintError("Failed to send file part packet!\n");
						return (false);
					}

					// Response
					sendFilePartResponse = new SendFilePartResponse();
					success = ReceivePacket(sendFilePartResponse);
					unsigned long long receivedPartIndex = sendFilePartResponse->GetPartIndex(); //Ricardo original unsigned long long

					delete sendFilePartResponse;

					if (receivedPartIndex != filePartIndex)
					{
						Interface::PrintErrorSameLine("\n");
						Interface::PrintError("Expected file part index: %d Received: %d\n", filePartIndex, receivedPartIndex);
						return (false);
					}

					if (success)
						break;
				}

				if (!success)
					return (false);
			}

			if (receivedPartIndex != filePartIndex)
			{
				Interface::PrintErrorSameLine("\n");
				Interface::PrintError("Expected file part index: %d Received: %d\n", filePartIndex, receivedPartIndex);
				return (false);
			}

			bytesTransferred += fileTransferPacketSize;

			if (bytesTransferred > fileSize)
				bytesTransferred = fileSize;

			currentPercent = (unsigned long long)(100.0 * ((long double)bytesTransferred / (long double)fileSize));  //Ricardo original unsigned long long, double,  double

			if (currentPercent != previousPercent)
			{
				if (!verbose)
				{
					if (previousPercent < 10)
						Interface::Print("\b\b%d%%", currentPercent);
					else
						Interface::Print("\b\b\b%d%%", currentPercent);
				}
				else
				{
					Interface::Print("\n%d%%\n", currentPercent);
				}
			}

			previousPercent = currentPercent;
		}

		unsigned long long sequenceEffectiveByteCount = (isLastSequence && partialPacketByteCount != 0) ?
			fileTransferPacketSize * (lastSequenceSize - 1) + partialPacketByteCount : sequenceTotalByteCount; //Ricardo original unsigned long long

		if (destination == EndFileTransferPacket::kDestinationPhone)
		{
			EndPhoneFileTransferPacket *endPhoneFileTransferPacket = new EndPhoneFileTransferPacket(sequenceEffectiveByteCount, 0, deviceType, fileIdentifier, isLastSequence);

			success = SendPacket(endPhoneFileTransferPacket, kDefaultTimeoutSend, kEmptyTransferBeforeAndAfter);
			delete endPhoneFileTransferPacket;

			if (!success)
			{
				Interface::PrintErrorSameLine("\n");
				Interface::PrintError("Failed to end phone file transfer sequence!\n");
				return (false);
			}
		}
		else // destination == EndFileTransferPacket::kDestinationModem
		{
			EndModemFileTransferPacket *endModemFileTransferPacket = new EndModemFileTransferPacket(sequenceEffectiveByteCount, 0, deviceType, isLastSequence);

			success = SendPacket(endModemFileTransferPacket, kDefaultTimeoutSend, kEmptyTransferBeforeAndAfter);
			delete endModemFileTransferPacket;

			if (!success)
			{
				Interface::PrintErrorSameLine("\n");
				Interface::PrintError("Failed to end modem file transfer sequence!\n");
				return (false);
			}
		}

		fileTransferResponse = new ResponsePacket(ResponsePacket::kResponseTypeFileTransfer);
		success = ReceivePacket(fileTransferResponse, fileTransferSequenceTimeout);
		
		delete fileTransferResponse;

		if (!success)
		{
			Interface::PrintErrorSameLine("\n");
			Interface::PrintError("Failed to confirm end of file transfer sequence!\n");
			return (false);
		}
		
	}


	if (!verbose)
		Interface::Print("\n");

	return (true);
}
/*
*
* Ricardo Rodriguez: BridgeManager needs to have Multiport support, this function use a single USB Path
*
*/
void BridgeManager::SetUsbPath(const char * usbpath)
{
	this->usbPath = usbpath;
}
/*
*
* Ricardo Rodriguez: BridgeManager needs to activate old value for protocol in old Galaxy fam
*
*/
void BridgeManager::SetOldProto(bool setme)
{
	this->isOldPhone = setme;
}

void BridgeManager::SetUsbLogLevel(UsbLogLevel usbLogLevel)
{
	this->usbLogLevel = usbLogLevel;

	if (libusbContext)
	{
		switch (usbLogLevel)
		{
			case UsbLogLevel::None:
				libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_NONE);
				break;

			case UsbLogLevel::Error:
				libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_ERROR);
				break;

			case UsbLogLevel::Warning:
				libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_WARNING);
				break;

			case UsbLogLevel::Info:
				libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_INFO);
				break;

			case UsbLogLevel::Debug:
				libusb_set_debug(libusbContext, LIBUSB_LOG_LEVEL_DEBUG);
				break;
		}
	}
}
