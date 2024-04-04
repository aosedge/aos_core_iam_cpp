/*
 * Copyright (C) 2024 Renesas Electronics Corporation.
 * Copyright (C) 2024 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <Poco/JSON/JSONException.h>

#include "log.hpp"
#include "pocowsclient.hpp"
#include "utils/json.hpp"
#include "visidentifier.hpp"
#include "vismessage.hpp"
#include "wsexception.hpp"

/***********************************************************************************************************************
 * VISSubscriptions
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Public
 **********************************************************************************************************************/

void VISSubscriptions::RegisterSubscription(const std::string& subscriptionId, Handler&& subscriptionHandler)
{
    std::lock_guard lock(mMutex);

    LOG_DBG() << "Registred subscription: id = " << subscriptionId.c_str();

    mSubscriptionMap[subscriptionId] = std::move(subscriptionHandler);
}

aos::Error VISSubscriptions::ProcessSubscription(const std::string& subscriptionId, const Poco::Dynamic::Var value)
{
    std::lock_guard lock(mMutex);

    const auto it = mSubscriptionMap.find(subscriptionId);

    if (it == mSubscriptionMap.cend()) {
        LOG_ERR() << "Subscription id not found: id = " << subscriptionId.c_str();

        return aos::ErrorEnum::eNotFound;
    }

    return it->second(value);
}

/***********************************************************************************************************************
 * VISIdentifier
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Public
 **********************************************************************************************************************/

VISIdentifier::VISIdentifier()
    : mWSClientIsConnected {Poco::Event::EventType::EVENT_MANUALRESET}
    , mStopHandleSubjectsChangedThread {Poco::Event::EventType::EVENT_AUTORESET}
{
}

aos::Error VISIdentifier::Init(const Config& config, aos::iam::identhandler::SubjectsObserverItf& subjectsObserver)
{
    std::lock_guard lock(mMutex);

    if (auto err = InitWSClient(config); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    mSubjectsObserver = &subjectsObserver;

    mHandleConnectionThread = std::thread(&VISIdentifier::HandleConnection, this);

    return aos::ErrorEnum::eNone;
}

aos::RetWithError<aos::StaticString<aos::cSystemIDLen>> VISIdentifier::GetSystemID()
{
    std::lock_guard lock(mMutex);

    if (mSystemId.IsEmpty()) {
        try {
            const VISMessage responseMessage(SendGetRequest(cVinVISPath));

            if (!responseMessage.Is(VISActionEnum::eGet)) {
                return {{}, AOS_ERROR_WRAP(aos::ErrorEnum::eFailed)};
            }

            const auto systemId = GetValueByPath(responseMessage.GetJSON(), cVinVISPath);
            if (systemId.empty()) {
                return {{}, AOS_ERROR_WRAP(aos::ErrorEnum::eFailed)};
            }

            if (systemId.size() > mSystemId.MaxSize()) {
                return {{}, AOS_ERROR_WRAP(aos::ErrorEnum::eNoMemory)};
            }

            mSystemId = systemId.c_str();
        } catch (const std::exception& e) {
            LOG_ERR() << "Failed to get system ID: error = " << e.what();

            return {{}, AOS_ERROR_WRAP(aos::ErrorEnum::eFailed)};
        }
    }

    return mSystemId;
}

aos::RetWithError<aos::StaticString<aos::cUnitModelLen>> VISIdentifier::GetUnitModel()
{
    std::lock_guard lock(mMutex);

    if (mUnitModel.IsEmpty()) {
        try {
            const VISMessage responseMessage(SendGetRequest(cUnitModelPath));

            if (!responseMessage.Is(VISActionEnum::eGet)) {
                return {{}, AOS_ERROR_WRAP(aos::ErrorEnum::eFailed)};
            }

            const auto unitModel = GetValueByPath(responseMessage.GetJSON(), cUnitModelPath);
            if (unitModel.empty()) {
                return {{}, AOS_ERROR_WRAP(aos::ErrorEnum::eFailed)};
            }

            if (unitModel.size() > mUnitModel.MaxSize()) {
                return {{}, AOS_ERROR_WRAP(aos::ErrorEnum::eNoMemory)};
            }

            mUnitModel = unitModel.c_str();
        } catch (const std::exception& e) {
            LOG_ERR() << "Failed to get unit model: error = " << e.what();

            return {{}, AOS_ERROR_WRAP(aos::ErrorEnum::eFailed)};
        }
    }

    return mUnitModel;
}

aos::Error VISIdentifier::GetSubjects(aos::Array<aos::StaticString<aos::cSubjectIDLen>>& subjects)
{
    std::lock_guard lock(mMutex);

    if (mSubjects.IsEmpty()) {
        try {
            const VISMessage responseMessage(SendGetRequest(cSubjectsVISPath));

            if (!responseMessage.Is(VISActionEnum::eGet)) {
                return AOS_ERROR_WRAP(aos::ErrorEnum::eFailed);
            }

            const auto responseSubjects = GetValueArrayByPath(responseMessage.GetJSON(), cSubjectsVISPath);

            for (const auto& subject : responseSubjects) {
                if (auto err = mSubjects.PushBack(subject.c_str()); !err.IsNone()) {
                    mSubjects.Clear();

                    return AOS_ERROR_WRAP(err);
                }
            }
        } catch (const Poco::Exception& e) {
            LOG_ERR() << "Failed to get subjects: error = " << e.message().c_str();

            return AOS_ERROR_WRAP(aos::ErrorEnum::eFailed);
        }
    }

    if (mSubjects.Size() > subjects.MaxSize()) {
        return AOS_ERROR_WRAP(aos::ErrorEnum::eNoMemory);
    }

    subjects = mSubjects;

    return aos::ErrorEnum::eNone;
}

VISIdentifier::~VISIdentifier()
{
    Close();
}

/***********************************************************************************************************************
 * Protected
 **********************************************************************************************************************/

aos::Error VISIdentifier::InitWSClient(const Config& config)
{
    try {
        VISIdentifierModuleParams visParams;
        aos::Error                err;

        aos::Tie(visParams, err) = ParseVISIdentifierModuleParams(config.mIdentifier.mParams);
        if (!err.IsNone()) {
            LOG_ERR() << "Failed to parse VIS identifier module params: error = " << err.Message();

            return AOS_ERROR_WRAP(err);
        }

        mWsClientPtr = std::make_shared<PocoWSClient>(
            visParams, std::bind(&VISIdentifier::HandleSubscription, this, std::placeholders::_1));
    } catch (const std::exception& e) {
        LOG_ERR() << "Failed to create WS client: error = " << e.what();

        return AOS_ERROR_WRAP(aos::ErrorEnum::eFailed);
    }

    return aos::ErrorEnum::eNone;
}

void VISIdentifier::SetWSClient(WSClientItfPtr wsClient)
{
    mWsClientPtr = std::move(wsClient);
}

WSClientItfPtr VISIdentifier::GetWSClient()
{
    return mWsClientPtr;
}

void VISIdentifier::HandleSubscription(const std::string& message)
{
    try {
        const VISMessage notification(message);
        const auto       subscriptionId = notification.GetValue<std::string>(VISMessage::cSubscriptionIdTagName);

        if (!notification.Is(VISActionEnum::eSubscriptionNotification) || subscriptionId.empty()) {
            LOG_ERR() << "Unexpected message received: message = " << notification.ToString().c_str();

            return;
        }

        const auto err = mSubscriptions.ProcessSubscription(subscriptionId, notification.GetJSON());
        if (!err.IsNone()) {
            LOG_ERR() << "Failed to process subscription: err = " << err.Message();

            return;
        }
    } catch (const std::exception& e) {
        LOG_ERR() << "Failed to handle subscription: error = " << e.what();
    }
}

void VISIdentifier::WaitUntilConnected()
{
    mWSClientIsConnected.wait();
}

/***********************************************************************************************************************
 * Private
 **********************************************************************************************************************/
void VISIdentifier::Close()
{
    try {
        if (mWsClientPtr) {
            SendUnsubscribeAllRequest();

            mStopHandleSubjectsChangedThread.set();
            mWsClientPtr->Close();
        }

        if (mHandleConnectionThread.joinable()) {
            mHandleConnectionThread.join();
        }

        mWSClientIsConnected.reset();

        LOG_INF() << "VISIdentifier has been closed";

    } catch (const std::exception& e) {
        LOG_ERR() << "Failed to close VISIdentifier: error = " << e.what();
    }
}

void VISIdentifier::HandleConnection()
{
    do {
        try {
            mWsClientPtr->Connect();

            Subscribe(
                cSubjectsVISPath, std::bind(&VISIdentifier::HandleSubjectsSubscription, this, std::placeholders::_1));

            mSystemId.Clear();
            mUnitModel.Clear();
            mSubjects.Clear();

            mWSClientIsConnected.set();

            // block on Wait
            const auto wsClientEvent = mWsClientPtr->WaitForEvent();

            if (wsClientEvent.mCode == WSClientEvent::EventEnum::CLOSED) {
                LOG_INF() << "WS Client connection has been closed. Stopping Vis Identifier Handle Connection thread";

                return;
            }

            mWSClientIsConnected.reset();
            mWsClientPtr->Disconnect();

        } catch (const WSException& e) {
            LOG_ERR() << "WSException has been caught: message = " << e.what();

            mWSClientIsConnected.reset();
            mWsClientPtr->Disconnect();
        } catch (const Poco::Exception& e) {
            LOG_ERR() << "Poco exception caught: message = " << e.message().c_str();
        } catch (...) {
            LOG_ERR() << "Unknown exception caught";
        }
    } while (!mStopHandleSubjectsChangedThread.tryWait(cWSClientReconnectMilliseconds));
}

aos::Error VISIdentifier::HandleSubjectsSubscription(Poco::Dynamic::Var value)
{
    try {
        aos::StaticArray<aos::StaticString<aos::cSubjectIDLen>, aos::cMaxSubjectIDSize> newSubjects;

        const auto responseSubjects = GetValueArrayByPath(value, cSubjectsVISPath);

        for (const auto& subject : responseSubjects) {
            if (auto err = newSubjects.PushBack(subject.c_str()); !err.IsNone()) {
                return err;
            }
        }

        std::lock_guard lock(mMutex);

        if (mSubjects != newSubjects) {
            mSubjects = std::move(newSubjects);
            mSubjectsObserver->SubjectsChanged(mSubjects);
        }
    } catch (const std::exception& e) {
        LOG_ERR() << "Failed to handle subjects subscription: error = " << e.what();

        return aos::ErrorEnum::eFailed;
    }

    return aos::ErrorEnum::eNone;
}

std::string VISIdentifier::SendGetRequest(const std::string& path)
{
    const auto       requestId = mWsClientPtr->GenerateRequestID();
    const VISMessage getMessage(VISActionEnum::eGet, requestId, path);

    WaitUntilConnected();

    const auto response = mWsClientPtr->SendRequest(requestId, getMessage.ToByteArray());

    return {response.cbegin(), response.cend()};
}

void VISIdentifier::SendUnsubscribeAllRequest()
{
    try {
        const VISMessage request(VISActionEnum::eUnsubscribeAll, mWsClientPtr->GenerateRequestID(), "");

        mWsClientPtr->AsyncSendMessage(request.ToByteArray());

    } catch (const std::exception& e) {
        LOG_ERR() << "Failed to send unsubscribe all request: error = " << e.what();
    }
}

void VISIdentifier::Subscribe(const std::string& path, VISSubscriptions::Handler&& callback)
{
    const auto       requestId = mWsClientPtr->GenerateRequestID();
    const VISMessage subscribeMessage(VISActionEnum::eSubscribe, requestId, path);

    const auto       response = mWsClientPtr->SendRequest(requestId, subscribeMessage.ToByteArray());
    const VISMessage responseVISMessage(std::string {response.cbegin(), response.cend()});

    mSubscriptions.RegisterSubscription(
        responseVISMessage.GetValue<std::string>(VISMessage::cSubscriptionIdTagName), std::move(callback));
}

std::string VISIdentifier::GetValueByPath(Poco::Dynamic::Var object, const std::string& valueChildTagName)
{
    auto var = UtilsJson::FindByPath(object, {VISMessage::cValueTagName});

    if (var.isString()) {
        return var.extract<std::string>();
    }

    var = UtilsJson::FindByPath(var, {valueChildTagName});

    return var.extract<std::string>();
}

std::vector<std::string> VISIdentifier::GetValueArrayByPath(
    Poco::Dynamic::Var object, const std::string& valueChildTagName)
{
    auto var = UtilsJson::FindByPath(object, {VISMessage::cValueTagName});

    const auto isArray = [](const Poco::Dynamic::Var var) {
        return var.type() == typeid(Poco::JSON::Array) || var.type() == typeid(Poco::JSON::Array::Ptr);
    };

    if (!isArray(var)) {
        var = UtilsJson::FindByPath(var, {valueChildTagName});
    }

    Poco::JSON::Array::Ptr array;
    if (var.type() == typeid(Poco::JSON::Array::Ptr)) {
        array = var.extract<Poco::JSON::Array::Ptr>();
    } else if (var.type() == typeid(Poco::JSON::Array)) {
        array = new Poco::JSON::Array(var.extract<Poco::JSON::Array>());
    } else {
        throw Poco::JSON::JSONException("key not found or not an array");
    }

    std::vector<std::string> valueArray;

    for (const auto& i : *array) {
        valueArray.push_back(i.convert<std::string>());
    }

    return valueArray;
}
