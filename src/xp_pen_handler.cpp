/*
userspace-tablet-driver-daemon
Copyright (C) 2021 - Aren Villanueva <https://github.com/kurikaesu/>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>
#include <algorithm>
#include <thread>
#include <set>
#include "xp_pen_handler.h"
#include "transfer_handler_pair.h"
#include "artist_22r_pro.h"
#include "artist_22e_pro.h"
#include "artist_13_3_pro.h"
#include "artist_24_pro.h"
#include "artist_12_pro.h"
#include "deco_pro_small.h"
#include "deco_pro_medium.h"
#include "transfer_setup_data.h"
#include "deco_01v2.h"
#include "star_g430s.h"
#include "ac19.h"

xp_pen_handler::xp_pen_handler() {
    std::cout << "xp_pen_handler initialized" << std::endl;

    addHandler(new artist_22r_pro());
    addHandler(new artist_22e_pro());
    addHandler(new artist_13_3_pro());
    addHandler(new artist_24_pro());
    addHandler(new artist_12_pro());
    addHandler(new deco_pro_small());
    addHandler(new deco_pro_medium());
    addHandler(new deco_01v2());
    addHandler(new star_g430s());
    addHandler(new ac19());
}

int xp_pen_handler::getVendorId() {
    return 0x28bd;
}

std::vector<int> xp_pen_handler::getProductIds() {
    return handledProducts;
}

std::string xp_pen_handler::vendorName() {
    return "XP-Pen";
}

void xp_pen_handler::setConfig(nlohmann::json config) {
    for (auto product : productHandlers) {
        auto productString = std::to_string(product.first);
        if (!config.contains(productString) || config[productString] == nullptr) {
            config[productString] = nlohmann::json({});
        }

        product.second->setConfig(config[productString]);
    }

    jsonConfig = config;
}

nlohmann::json xp_pen_handler::getConfig() {
    for (auto product : productHandlers) {
        jsonConfig[std::to_string(product.first)] = product.second->getConfig();
    }

    return jsonConfig;
}

void xp_pen_handler::handleMessages() {
    auto messages = messageQueue->getMessagesFor(message_destination::driver, getVendorId());
    size_t handledMessages = 0;
    size_t totalMessages = messages.size();

    if (totalMessages > 0) {
        // Cancel transfers first
        for (auto transfer: libusbTransfers) {
            libusb_cancel_transfer(transfer);
        }

        libusbTransfers.clear();

        for (auto message: messages) {
            auto handler = productHandlers.find(message->device);
            if (handler != productHandlers.end()) {
                auto responses = handler->second->handleMessage(message);
                delete message;

                for (auto response: responses) {
                    messageQueue->addMessage(response);
                }

                handledMessages++;
            }
        }

        // Re-enable transfers
        for (auto setupData: transfersSetUp) {
            setupTransfers(setupData.handle, setupData.interface_number, setupData.maxPacketSize, setupData.productId);
        }

        std::cout << "Handled " << handledMessages << " out of " << totalMessages << " messages." << std::endl;
    }
}

std::set<short> xp_pen_handler::getConnectedDevices() {
    std::set<short> connectedDevices;

    for (auto device : deviceInterfaceMap) {
        connectedDevices.insert(device.second->productId);
    }

    return connectedDevices;
}

bool xp_pen_handler::handleProductAttach(libusb_device* device, const libusb_device_descriptor descriptor) {
    libusb_device_handle* handle = NULL;
    device_interface_pair* interfacePair = nullptr;
    const int maxRetries = 5;
    int currentAttept = 0;

    if (std::find(handledProducts.begin(), handledProducts.end(), descriptor.idProduct) != handledProducts.end()) {
        std::cout << "Handling " << productHandlers[descriptor.idProduct]->getProductName(descriptor.idProduct) << std::endl;
        while (interfacePair == nullptr  && currentAttept < maxRetries) {
            interfacePair = claimDevice(device, handle, descriptor);
            if (interfacePair == nullptr) {
                std::cout << "Could not claim device on attempt " << currentAttept << ". Detaching and then waiting" << std::endl;
                handleProductDetach(device, descriptor);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                ++currentAttept;
            }
        }

        if (interfacePair != nullptr) {
            deviceInterfaces.push_back(interfacePair);
            deviceInterfaceMap[device] = interfacePair;
            return true;
        }

        std::cout << "Giving up" << std::endl;
        return false;
    }

    std::cout << "Unknown product " << descriptor.idProduct << std::endl;

    return false;
}

void xp_pen_handler::handleProductDetach(libusb_device *device, struct libusb_device_descriptor descriptor) {
    for (auto deviceObj : deviceInterfaceMap) {
        if (deviceObj.first == device) {
            std::cout << "Handling device detach" << std::endl;

            if (productHandlers.find(descriptor.idProduct) != productHandlers.end()) {
                productHandlers[descriptor.idProduct]->detachDevice(deviceObj.second->deviceHandle);
            }

            cleanupDevice(deviceObj.second);
            libusb_close(deviceObj.second->deviceHandle);

            auto deviceInterfacesIterator = std::find(deviceInterfaces.begin(), deviceInterfaces.end(), deviceObj.second);
            if (deviceInterfacesIterator != deviceInterfaces.end()) {
                deviceInterfaces.erase(deviceInterfacesIterator);
            }

            auto deviceMapIterator = std::find(deviceInterfaceMap.begin(), deviceInterfaceMap.end(), deviceObj);
            if (deviceMapIterator != deviceInterfaceMap.end()) {
                deviceInterfaceMap.erase(deviceMapIterator);
            }

            break;
        }
    }
}

void xp_pen_handler::sendInitKey(libusb_device_handle *handle, int interface_number) {
    std::cout << "Sending init key on endpont " << interface_number << std::endl;

    unsigned char key[] = {0x02, 0xb0, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    int sentBytes;
    int ret = libusb_interrupt_transfer(handle, interface_number | LIBUSB_ENDPOINT_OUT, key, sizeof(key), &sentBytes, 1000);
    if (ret != LIBUSB_SUCCESS) {
        std::cout << "Failed to send key on interface " << interface_number << " ret: " << ret << " errno: " << errno << std::endl;
        return;
    }

    if (sentBytes != sizeof(key)) {
        std::cout << "Didn't send all of the key on interface " << interface_number << " only sent " << sentBytes << std::endl;
        return;
    }
}
