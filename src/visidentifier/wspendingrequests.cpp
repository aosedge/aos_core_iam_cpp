/*
 * Copyright (C) 2024 Renesas Electronics Corporation.
 * Copyright (C) 2024 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>

#include "wspendingrequests.hpp"

namespace aos::iam::visidentifier {

/***********************************************************************************************************************
 * RequestParams
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Public
 **********************************************************************************************************************/

RequestParams::RequestParams(const std::string& requestId)
    : mRequestId(requestId)
{
}

void RequestParams::SetResponse(const std::string& response)
{
    mResponse = response;
    mEvent.set();
}

const std::string& RequestParams::GetRequestId() const
{
    return mRequestId;
}

bool RequestParams::TryWaitForResponse(std::string& result, const Duration timeout)
{
    if (mEvent.tryWait(timeout.Milliseconds())) {
        result = mResponse;

        return true;
    }

    return false;
}

bool RequestParams::operator<(const RequestParams& rhs) const
{
    return mRequestId < rhs.mRequestId;
}

/***********************************************************************************************************************
 * PendingRequests
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Public
 **********************************************************************************************************************/

void PendingRequests::Add(RequestParamsPtr requestParamsPtr)
{
    std::lock_guard lock(mMutex);

    mRequests.push_back(std::move(requestParamsPtr));
}

void PendingRequests::Remove(RequestParamsPtr requestParamsPtr)
{
    std::lock_guard lock(mMutex);

    mRequests.erase(std::remove(mRequests.begin(), mRequests.end(), requestParamsPtr));
}

bool PendingRequests::SetResponse(const std::string& requestId, const std::string& response)
{
    std::lock_guard lock(mMutex);

    const auto itPendingMessage = std::find_if(mRequests.begin(), mRequests.end(),
        [&requestId](const auto& pendingRequest) { return pendingRequest->GetRequestId() == requestId; });

    if (itPendingMessage == mRequests.end()) {
        return false;
    }

    (*itPendingMessage)->SetResponse(response);

    return true;
}

} // namespace aos::iam::visidentifier
