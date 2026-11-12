// The signalling WebSocket of src/patch_bay_websocket.h on rtc::WebSocket of libdatachannel.
#include "patch_bay_websocket.h"

#include <exception>
#include <memory>
#include <string>
#include <variant>

#include <rtc/rtc.hpp>

static std::shared_ptr<rtc::WebSocket> websocket;

bool websocket_open(const char *url, const char *certificates, websocket_callback callback) {
    try {
        rtc::WebSocket::Configuration configuration;
        if (certificates) configuration.caCertificatePemFile = certificates;
        websocket = std::make_shared<rtc::WebSocket>(configuration);
        websocket->onOpen([callback] { callback(WEBSOCKET_OPEN, nullptr, 0); });
        websocket->onClosed([callback] { callback(WEBSOCKET_CLOSED, nullptr, 0); });
        websocket->onError([callback](std::string error) { callback(WEBSOCKET_ERROR, error.c_str(), error.size()); });
        websocket->onMessage([callback](rtc::message_variant message) {
            auto text = std::get_if<std::string>(&message);
            if (text) callback(WEBSOCKET_MESSAGE, text->c_str(), text->size());
        });
        websocket->open(url);
        return true;
    } catch (const std::exception &) {
        websocket.reset();
        return false;
    }
}

bool websocket_send(const char *text) {
    try {
        if (!websocket || !websocket->isOpen()) return false;
        websocket->send(std::string(text)); // false only means that the text waits in the buffer
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

void websocket_close(void) {
    if (websocket) websocket->close();
}
