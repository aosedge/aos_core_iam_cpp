/*
 * Copyright (C) 2024 Renesas Electronics Corporation.
 * Copyright (C) 2024 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POCOWSCLIENT_HPP_
#define POCOWSCLIENT_HPP_

#include <memory>
#include <mutex>
#include <optional>

#include <Poco/Event.h>
#include <Poco/Net/HTTPMessage.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/WebSocket.h>

#include "visidentifier/visconfig.hpp"
#include "visidentifier/wsclient.hpp"
#include "wsclientevent.hpp"
#include "wspendingrequests.hpp"

/**
 * Poco web socket client.
 */
class PocoWSClient : public WSClientItf {
public:
    /**
     * Creates Web socket client instance.
     *
     * @param config VIS config.
     * @param handler handler functor.
     */
    PocoWSClient(const VISConfig& config, MessageHandlerFunc handler);

    /**
     * Connects to Web Socket server.
     */
    void Connect() override;

    /**
     * Closes Web Socket client.
     */
    void Close() override;

    /**
     * Disconnects Web Socket client.
     */
    void Disconnect() override;

    /**
     * Generates request id.
     *
     * @returns std::string
     */
    std::string GenerateRequestID() override;

    /**
     * Waits for Web Socket Client Event.
     *
     * @returns WSClientEvent::Details
     */
    WSClientEvent::Details WaitForEvent() override;

    /**
     * Sends request. Blocks till the response is received or timed-out (WSException is thrown).
     *
     * @param requestId request id
     * @param message request payload
     * @returns ByteArray
     */
    ByteArray SendRequest(const std::string& requestId, const ByteArray& message) override;

    /**
     * Sends message. Doesn't wait for response.
     *
     * @param message request payload
     */
    void AsyncSendMessage(const ByteArray& message);

    /**
     * Destroys web socket client instance.
     */
    ~PocoWSClient() override;

private:
    void HandleResponse(const std::string& frame);
    void ReceiveFrames();
    void StartReceiveFramesThread();
    void StopReceiveFramesThread();

    VISConfig                                      mConfig;
    std::recursive_mutex                           mMutex;
    std::thread                                    mReceivedFramesThread;
    std::unique_ptr<Poco::Net::HTTPSClientSession> mClientSession;
    std::optional<Poco::Net::WebSocket>            mWebSocket;
    bool                                           mIsConnected {false};
    Poco::Net::HTTPRequest                         mHttpRequest;
    Poco::Net::HTTPResponse                        mHttpResponse;
    PendingRequests                                mPendingRequests;
    MessageHandlerFunc                             mHandleSubscription;
    WSClientEvent                                  mWSClientErrorEvent;
};

#endif
