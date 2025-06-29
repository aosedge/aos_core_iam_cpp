/*
 * Copyright (C) 2024 Renesas Electronics Corporation.
 * Copyright (C) 2024 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <Poco/Buffer.h>
#include <Poco/JSON/Object.h>
#include <Poco/Net/Context.h>
#include <Poco/URI.h>

#include <aos/common/tools/uuid.hpp>
#include <utils/json.hpp>

#include "logger/logmodule.hpp"
#include "pocowsclient.hpp"
#include "vismessage.hpp"
#include "wsexception.hpp"

namespace aos::iam::visidentifier {

namespace {

/***********************************************************************************************************************
 * Statics
 **********************************************************************************************************************/

template <class F>
auto OnScopeExit(F&& f)
{
    return std::unique_ptr<void, typename std::decay<F>::type>(reinterpret_cast<void*>(1), std::forward<F>(f));
}

} // namespace

/***********************************************************************************************************************
 * Public
 **********************************************************************************************************************/

PocoWSClient::PocoWSClient(const aos::iam::config::VISIdentifierModuleParams& config, MessageHandlerFunc handler)
    : mConfig(config)
    , mHandleSubscription(std::move(handler))
{
    mHttpRequest.setMethod(Poco::Net::HTTPRequest::HTTP_GET);
    mHttpRequest.setVersion(Poco::Net::HTTPMessage::HTTP_1_1);
}

void PocoWSClient::Connect()
{
    std::lock_guard lock(mMutex);

    if (mIsConnected) {
        return;
    }

    const Poco::URI uri(mConfig.mVISServer);

    try {
        StopReceiveFramesThread();

        Poco::Net::Context::Ptr context = new Poco::Net::Context(
            Poco::Net::Context::TLS_CLIENT_USE, "", "", mConfig.mCaCertFile, Poco::Net::Context::VERIFY_RELAXED, 9);

        // HTTPSClientSession is not copyable or movable.
        mClientSession = std::make_unique<Poco::Net::HTTPSClientSession>(uri.getHost(), uri.getPort(), context);
        mWebSocket.emplace(Poco::Net::WebSocket(*mClientSession, mHttpRequest, mHttpResponse));
        mWebSocket->setKeepAlive(true);
        mWebSocket->setReceiveTimeout(0);

        mIsConnected = true;
        mWSClientErrorEvent.Reset();

        StartReceiveFramesThread();

        LOG_INF() << "Connected to VIS. URI = " << uri.toString().c_str();
    } catch (const Poco::Exception& e) {
        LOG_ERR() << "Failed to connect to VIS: URI = " << uri.toString().c_str() << ", error = " << e.what();

        throw WSException(e.what(), AOS_ERROR_WRAP(aos::ErrorEnum::eFailed));
    }
}

void PocoWSClient::Close()
{
    std::lock_guard lock(mMutex);

    LOG_INF() << "Close Web Socket client";

    try {
        if (mIsConnected) {
            mWebSocket->shutdown();
        }
    } catch (const std::exception& e) {
        LOG_WRN() << "Failed to close Web Socket client: error = " << e.what();
    }

    mIsConnected = false;
    mWSClientErrorEvent.Set(WSClientEvent::EventEnum::CLOSED, "ws connection has been closed on the client side.");
}

void PocoWSClient::Disconnect()
{
    std::lock_guard lock(mMutex);

    if (!mIsConnected) {
        return;
    }

    LOG_INF() << "Disconnect Web Socket client";

    try {
        mWebSocket->shutdown();
        mWebSocket->close();
    } catch (const std::exception& e) {
        LOG_WRN() << "Failed to disconnect Web Socket client: error = " << e.what();
    }

    mIsConnected = false;
}

std::string PocoWSClient::GenerateRequestID()
{
    const auto uuid    = aos::uuid::CreateUUID();
    const auto uuidStr = aos::uuid::UUIDToString(uuid);

    return {uuidStr.begin(), uuidStr.end()};
}

WSClientEvent::Details PocoWSClient::WaitForEvent()
{
    return mWSClientErrorEvent.Wait();
}

PocoWSClient::ByteArray PocoWSClient::SendRequest(const std::string& requestId, const ByteArray& message)
{
    auto requestParams = std::make_shared<RequestParams>(requestId);
    mPendingRequests.Add(requestParams);

    const auto onScopeExit = OnScopeExit([&](void*) { mPendingRequests.Remove(requestParams); });

    AsyncSendMessage(message);

    LOG_DBG() << "Sent message: requestId = " << requestId.c_str();

    std::string response;
    if (!requestParams->TryWaitForResponse(response, GetWebSocketTimeout())) {
        LOG_ERR() << "Timeout waiting for server response: requestId = " << requestId.c_str();

        throw WSException("", AOS_ERROR_WRAP(aos::ErrorEnum::eTimeout));
    }

    LOG_DBG() << "Got server response: requestId = " << requestId.c_str() << ", response = " << response.c_str();

    return {response.cbegin(), response.cend()};
}

void PocoWSClient::AsyncSendMessage(const ByteArray& message)
{
    if (message.empty()) {
        return;
    }

    std::lock_guard lock(mMutex);

    if (!mIsConnected) {
        throw WSException("Not connected", AOS_ERROR_WRAP(aos::ErrorEnum::eFailed));
    }

    try {
        mWebSocket->setSendTimeout(GetWebSocketTimeout().Microseconds());

        const int len = mWebSocket->sendFrame(&message.front(), message.size(), Poco::Net::WebSocket::FRAME_TEXT);

        LOG_DBG() << "Sent " << len << "/" << message.size() << " bytes.";
    } catch (const std::exception& e) {
        mWSClientErrorEvent.Set(WSClientEvent::EventEnum::FAILED, e.what());

        throw WSException(e.what(), AOS_ERROR_WRAP(aos::ErrorEnum::eFailed));
    }
}

PocoWSClient::~PocoWSClient()
{
    PocoWSClient::Close();
    StopReceiveFramesThread();
}

/***********************************************************************************************************************
 * Private
 **********************************************************************************************************************/

void PocoWSClient::HandleResponse(const std::string& frame)
{
    try {
        Poco::Dynamic::Var objectVar;
        aos::Error         err;

        aos::Tie(objectVar, err) = aos::common::utils::ParseJson(frame);
        AOS_ERROR_CHECK_AND_THROW(err, "can't parse as json");

        const auto object = objectVar.extract<Poco::JSON::Object::Ptr>();

        if (object.isNull()) {
            AOS_ERROR_THROW(ErrorEnum::eInvalidArgument, "can't extract json object");
        }

        if (!object->has(VISMessage::cActionTagName)) {
            AOS_ERROR_THROW(ErrorEnum::eInvalidArgument, "action tag is missing");
        }

        if (const auto action = object->get(VISMessage::cActionTagName); action == "subscription") {
            mHandleSubscription(frame);

            return;
        }

        const auto requestId = object->get(VISMessage::cRequestIdTagName).convert<std::string>();
        if (requestId.empty()) {
            AOS_ERROR_THROW(ErrorEnum::eInvalidArgument, "requestId tag is empty");
        }

        if (!mPendingRequests.SetResponse(requestId, frame)) {
            mHandleSubscription(frame);
        }
    } catch (const aos::common::utils::AosException& e) {
        LOG_ERR() << "Failed to handle VIS response: error = " << e.message().c_str();
    }
}

void PocoWSClient::ReceiveFrames() noexcept
{
    LOG_DBG() << "Start receiving frames.";

    try {
        int                flags;
        int                n;
        Poco::Buffer<char> buffer(0);

        do {
            n = mWebSocket->receiveFrame(buffer, flags);
            LOG_DBG() << "received frame from VIS: bytes = " << n << ", flags = " << flags;

            if ((flags & Poco::Net::WebSocket::FRAME_OP_BITMASK) == Poco::Net::WebSocket::FRAME_OP_CLOSE) {
                mWSClientErrorEvent.Set(WSClientEvent::EventEnum::FAILED, "got Close frame from server");

                return;
            }

            if (n > 0) {

                const std::string message(buffer.begin(), buffer.end());

                buffer.resize(0);

                HandleResponse(message);
            }

        } while (flags != 0 || n != 0);
    } catch (const std::exception& e) {
        LOG_DBG() << "ReceiveFrames stopped: error = " << e.what();

        mWSClientErrorEvent.Set(WSClientEvent::EventEnum::FAILED, e.what());

        return;
    } catch (...) {
        LOG_WRN() << "ReceiveFrames stopped: error =  unknown";
    }

    LOG_DBG() << "ReceiveFrames stopped.";

    mWSClientErrorEvent.Set(WSClientEvent::EventEnum::FAILED, "ReceiveFrames stopped");
}

void PocoWSClient::StartReceiveFramesThread()
{
    StopReceiveFramesThread();

    mReceivedFramesThread = std::thread(&PocoWSClient::ReceiveFrames, this);
}

void PocoWSClient::StopReceiveFramesThread()
{
    if (mReceivedFramesThread.joinable()) {
        mReceivedFramesThread.join();
    }
}

Duration PocoWSClient::GetWebSocketTimeout()
{
    if (mConfig.mWebSocketTimeout > 0) {
        return mConfig.mWebSocketTimeout;
    }

    return cDefaultTimeout;
}

} // namespace aos::iam::visidentifier
