/*
 * Copyright (C) 2024 Renesas Electronics Corporation.
 * Copyright (C) 2024 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <memory>

#include <aos/common/crypto/crypto.hpp>
#include <aos/common/crypto/utils.hpp>
#include <aos/common/tools/string.hpp>
#include <aos/common/types.hpp>
#include <aos/iam/certhandler.hpp>

#include <pbconvert/common.hpp>
#include <pbconvert/iam.hpp>

#include "logger/logmodule.hpp"
#include "protectedmessagehandler.hpp"

namespace aos::iam::iamserver {

namespace {

/***********************************************************************************************************************
 * Constants
 **********************************************************************************************************************/

const Error cStreamNotFoundError = {ErrorEnum::eNotFound, "stream not found"};

} // namespace

/***********************************************************************************************************************
 * Public
 **********************************************************************************************************************/

Error ProtectedMessageHandler::Init(NodeController& nodeController, iam::identhandler::IdentHandlerItf& identHandler,
    iam::permhandler::PermHandlerItf& permHandler, iam::nodeinfoprovider::NodeInfoProviderItf& nodeInfoProvider,
    iam::nodemanager::NodeManagerItf& nodeManager, iam::certprovider::CertProviderItf& certProvider,
    iam::provisionmanager::ProvisionManagerItf& provisionManager)
{
    LOG_DBG() << "Initialize message handler: handler=protected";

    mProvisionManager = &provisionManager;

    return PublicMessageHandler::Init(
        nodeController, identHandler, permHandler, nodeInfoProvider, nodeManager, certProvider);
}

// cppcheck-suppress duplInheritedMember
void ProtectedMessageHandler::RegisterServices(grpc::ServerBuilder& builder)
{
    LOG_DBG() << "Register services: handler=protected";

    PublicMessageHandler::RegisterServices(builder);

    if (GetPermHandler() != nullptr) {
        builder.RegisterService(static_cast<iamproto::IAMPermissionsService::Service*>(this));
    }

    if (iam::nodeinfoprovider::IsMainNode(GetNodeInfo())) {
        builder.RegisterService(static_cast<iamproto::IAMCertificateService::Service*>(this));
        builder.RegisterService(static_cast<iamproto::IAMProvisioningService::Service*>(this));
        builder.RegisterService(static_cast<iamproto::IAMNodesService::Service*>(this));
    }
}

// cppcheck-suppress duplInheritedMember
void ProtectedMessageHandler::Close()
{
    LOG_DBG() << "Close message handler: handler=protected";

    PublicMessageHandler::Close();
}

/***********************************************************************************************************************
 * Private
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * IAMPublicNodesService implementation
 **********************************************************************************************************************/

grpc::Status ProtectedMessageHandler::RegisterNode(grpc::ServerContext*                     context,
    grpc::ServerReaderWriter<iamproto::IAMIncomingMessages, iamproto::IAMOutgoingMessages>* stream)
{
    LOG_DBG() << "Process register node: handler=protected";

    return GetNodeController()->HandleRegisterNodeStream(
        {cAllowedStatuses.cbegin(), cAllowedStatuses.cend()}, stream, context, GetNodeManager());
}

/***********************************************************************************************************************
 * IAMNodesService implementation
 **********************************************************************************************************************/

grpc::Status ProtectedMessageHandler::PauseNode([[maybe_unused]] grpc::ServerContext* context,
    const iamproto::PauseNodeRequest* request, iamproto::PauseNodeResponse* response)
{
    const auto& nodeID = request->node_id();

    LOG_DBG() << "Process pause node: nodeID=" << nodeID.c_str();

    if (!ProcessOnThisNode(nodeID)) {
        if (auto status = RequestWithRetry([&]() {
                auto handler = GetNodeController()->GetNodeStreamHandler(nodeID);
                if (!handler) {
                    return common::pbconvert::ConvertAosErrorToGrpcStatus(cStreamNotFoundError);
                }

                return handler->PauseNode(request, response, cDefaultTimeout);
            });
            !status.ok()) {
            return status;
        }
    }

    if (auto err = SetNodeStatus(nodeID, NodeStatusEnum::ePaused); !err.IsNone()) {
        LOG_ERR() << "Set node status failed: error=" << err;

        common::pbconvert::SetErrorInfo(err, *response);
    }

    return grpc::Status::OK;
}

grpc::Status ProtectedMessageHandler::ResumeNode([[maybe_unused]] grpc::ServerContext* context,
    const iamproto::ResumeNodeRequest* request, iamproto::ResumeNodeResponse* response)
{
    const auto& nodeID = request->node_id();

    LOG_DBG() << "Process resume node: nodeID=" << nodeID.c_str();

    if (!ProcessOnThisNode(nodeID)) {
        if (auto status = RequestWithRetry([&]() {
                auto handler = GetNodeController()->GetNodeStreamHandler(nodeID);
                if (!handler) {
                    return common::pbconvert::ConvertAosErrorToGrpcStatus(cStreamNotFoundError);
                }

                return handler->ResumeNode(request, response, cDefaultTimeout);
            });
            !status.ok()) {
            return status;
        }
    }

    if (auto err = SetNodeStatus(nodeID, NodeStatusEnum::eProvisioned); !err.IsNone()) {
        LOG_ERR() << "Set node status failed: error=" << err;

        common::pbconvert::SetErrorInfo(err, *response);
    }

    return grpc::Status::OK;
}

/***********************************************************************************************************************
 * IAMProvisioningService implementation
 **********************************************************************************************************************/

grpc::Status ProtectedMessageHandler::GetCertTypes([[maybe_unused]] grpc::ServerContext* context,
    const iamproto::GetCertTypesRequest* request, iamproto::CertTypes* response)
{
    const auto& nodeID = request->node_id();

    LOG_DBG() << "Process get cert types: ID = " << nodeID.c_str();

    if (!ProcessOnThisNode(nodeID)) {
        return RequestWithRetry([&]() {
            auto handler = GetNodeController()->GetNodeStreamHandler(nodeID);
            if (!handler) {
                return common::pbconvert::ConvertAosErrorToGrpcStatus(cStreamNotFoundError);
            }

            return handler->GetCertTypes(request, response, cDefaultTimeout);
        });
    }

    Error                            err;
    iam::provisionmanager::CertTypes certTypes;

    Tie(certTypes, err) = mProvisionManager->GetCertTypes();
    if (!err.IsNone()) {
        LOG_ERR() << "Get certificate types error: " << AOS_ERROR_WRAP(err);

        return common::pbconvert::ConvertAosErrorToGrpcStatus(cStreamNotFoundError);
    }

    for (const auto& type : certTypes) {
        response->add_types(type.CStr());
    }

    return grpc::Status::OK;
}

grpc::Status ProtectedMessageHandler::StartProvisioning([[maybe_unused]] grpc::ServerContext* context,
    const iamproto::StartProvisioningRequest* request, iamproto::StartProvisioningResponse* response)
{
    const auto& nodeID = request->node_id();

    LOG_DBG() << "Process start provisioning request: nodeID=" << nodeID.c_str();

    if (!ProcessOnThisNode(nodeID)) {
        return RequestWithRetry([&]() {
            auto handler = GetNodeController()->GetNodeStreamHandler(nodeID);
            if (!handler) {
                return common::pbconvert::ConvertAosErrorToGrpcStatus(cStreamNotFoundError);
            }

            return handler->StartProvisioning(request, response, cProvisioningTimeout);
        });
    }

    if (auto err = mProvisionManager->StartProvisioning(request->password().c_str()); !err.IsNone()) {
        LOG_ERR() << "Start provisioning error: error=" << err;

        common::pbconvert::SetErrorInfo(err, *response);
    }

    return grpc::Status::OK;
}

grpc::Status ProtectedMessageHandler::FinishProvisioning([[maybe_unused]] grpc::ServerContext* context,
    const iamproto::FinishProvisioningRequest* request, iamproto::FinishProvisioningResponse* response)
{
    const auto& nodeID = request->node_id();

    LOG_DBG() << "Process finish provisioning request: nodeID=" << nodeID.c_str();

    if (!ProcessOnThisNode(nodeID)) {
        if (auto status = RequestWithRetry([&]() {
                auto handler = GetNodeController()->GetNodeStreamHandler(nodeID);
                if (!handler) {
                    return common::pbconvert::ConvertAosErrorToGrpcStatus(cStreamNotFoundError);
                }

                return handler->FinishProvisioning(request, response, cProvisioningTimeout);
            });
            !status.ok()) {
            return status;
        }
    } else {
        if (auto err = mProvisionManager->FinishProvisioning(request->password().c_str()); !err.IsNone()) {
            LOG_ERR() << "Finish provisioning failed: error=" << err;

            common::pbconvert::SetErrorInfo(err, *response);

            return grpc::Status::OK;
        }
    }

    if (auto err = SetNodeStatus(nodeID, NodeStatusEnum::eProvisioned); !err.IsNone()) {
        LOG_ERR() << "Set node status failed: error=" << err;

        common::pbconvert::SetErrorInfo(err, *response);
    }

    return grpc::Status::OK;
}

grpc::Status ProtectedMessageHandler::Deprovision([[maybe_unused]] grpc::ServerContext* context,
    const iamproto::DeprovisionRequest* request, iamproto::DeprovisionResponse* response)
{
    const auto& nodeID = request->node_id();

    LOG_DBG() << "Process deprovision request: nodeID=" << nodeID.c_str();

    if (!ProcessOnThisNode(nodeID)) {
        if (auto status = RequestWithRetry([&]() {
                auto handler = GetNodeController()->GetNodeStreamHandler(nodeID);
                if (!handler) {
                    return common::pbconvert::ConvertAosErrorToGrpcStatus(cStreamNotFoundError);
                }

                return handler->Deprovision(request, response, cProvisioningTimeout);
            });
            !status.ok()) {
            return status;
        }
    } else {
        if (auto err = mProvisionManager->Deprovision(request->password().c_str()); !err.IsNone()) {
            LOG_ERR() << "Deprovision failed: error=" << err;

            common::pbconvert::SetErrorInfo(err, *response);

            return grpc::Status::OK;
        }
    }

    if (auto err = SetNodeStatus(nodeID, NodeStatusEnum::eUnprovisioned); !err.IsNone()) {
        LOG_ERR() << "Set node status failed: error=" << err;

        common::pbconvert::SetErrorInfo(err, *response);
    }

    return grpc::Status::OK;
}

/***********************************************************************************************************************
 * IAMCertificateService implementation
 **********************************************************************************************************************/

grpc::Status ProtectedMessageHandler::CreateKey([[maybe_unused]] grpc::ServerContext* context,
    const iamproto::CreateKeyRequest* request, iamproto::CreateKeyResponse* response)
{
    const auto& nodeID   = request->node_id();
    const auto  certType = String(request->type().c_str());

    LOG_DBG() << "Process create key request: nodeID=" << nodeID.c_str() << ", type=" << certType;

    StaticString<cSystemIDLen> subject = request->subject().c_str();

    if (subject.IsEmpty() && !GetIdentHandler()) {
        Error err(ErrorEnum::eNotFound, "Subject can't be empty");

        LOG_ERR() << "Create key failed: error=" << err;

        common::pbconvert::SetErrorInfo(err, *response);

        return grpc::Status::OK;
    }

    Error err = ErrorEnum::eNone;

    if (subject.IsEmpty() && GetIdentHandler()) {
        Tie(subject, err) = GetIdentHandler()->GetSystemID();
        if (!err.IsNone()) {
            LOG_ERR() << "Get system ID failed: error=" << err;

            common::pbconvert::SetErrorInfo(err, *response);

            return grpc::Status::OK;
        }
    }

    if (!ProcessOnThisNode(nodeID)) {
        return RequestWithRetry([&]() {
            auto handler = GetNodeController()->GetNodeStreamHandler(nodeID);
            if (!handler) {
                return common::pbconvert::ConvertAosErrorToGrpcStatus(cStreamNotFoundError);
            }

            iamproto::CreateKeyRequest keyRequest = *request;

            keyRequest.set_subject(subject.CStr());

            return handler->CreateKey(&keyRequest, response, cDefaultTimeout);
        });
    }

    const auto password = String(request->password().c_str());
    auto       csr      = std::make_unique<StaticString<crypto::cCSRPEMLen>>();

    if (err = mProvisionManager->CreateKey(certType, subject, password, *csr); !err.IsNone()) {
        LOG_ERR() << "Create key failed: error=" << err;

        common::pbconvert::SetErrorInfo(err, *response);

        return grpc::Status::OK;
    }

    response->set_node_id(nodeID);
    response->set_type(certType.CStr());
    response->set_csr(csr->CStr());

    return grpc::Status::OK;
}

grpc::Status ProtectedMessageHandler::ApplyCert([[maybe_unused]] grpc::ServerContext* context,
    const iamproto::ApplyCertRequest* request, iamproto::ApplyCertResponse* response)
{
    const auto& nodeID   = request->node_id();
    const auto  certType = String(request->type().c_str());

    LOG_DBG() << "Process apply cert request: nodeID=" << nodeID.c_str() << ",type=" << certType;

    response->set_node_id(nodeID);
    response->set_type(certType.CStr());

    if (!ProcessOnThisNode(nodeID)) {
        return RequestWithRetry([&]() {
            auto handler = GetNodeController()->GetNodeStreamHandler(nodeID);
            if (!handler) {
                return common::pbconvert::ConvertAosErrorToGrpcStatus(cStreamNotFoundError);
            }

            return handler->ApplyCert(request, response, cDefaultTimeout);
        });
    }

    const auto pemCert = String(request->cert().c_str());

    iam::certhandler::CertInfo certInfo;

    if (auto err = mProvisionManager->ApplyCert(certType, pemCert, certInfo); !err.IsNone()) {
        LOG_ERR() << "Apply cert failed: error=" << err;

        common::pbconvert::SetErrorInfo(err, *response);

        return grpc::Status::OK;
    }

    Error       err;
    std::string serial;

    Tie(serial, err) = common::pbconvert::ConvertSerialToProto(certInfo.mSerial);
    if (!err.IsNone()) {
        LOG_ERR() << "Convert serial failed: error=" << err;

        common::pbconvert::SetErrorInfo(err, *response);

        return grpc::Status::OK;
    }

    response->set_cert_url(certInfo.mCertURL.CStr());
    response->set_serial(serial);

    return grpc::Status::OK;
}

/***********************************************************************************************************************
 * IAMPermissionsService implementation
 **********************************************************************************************************************/

grpc::Status ProtectedMessageHandler::RegisterInstance([[maybe_unused]] grpc::ServerContext* context,
    const iamproto::RegisterInstanceRequest* request, iamproto::RegisterInstanceResponse* response)
{
    Error      err         = ErrorEnum::eNone;
    const auto aosInstance = common::pbconvert::ConvertToAos(request->instance());

    LOG_DBG() << "Process register instance: serviceID=" << aosInstance.mServiceID
              << ", subjectID=" << aosInstance.mSubjectID << ", instance=" << aosInstance.mInstance;

    // Convert permissions
    auto aosPermissions = std::make_unique<StaticArray<FunctionServicePermissions, cMaxNumServices>>();

    for (const auto& [service, permissions] : request->permissions()) {
        if (err = aosPermissions->EmplaceBack(); !err.IsNone()) {
            LOG_ERR() << "Failed to push back permissions: error=" << err;

            return common::pbconvert::ConvertAosErrorToGrpcStatus(err);
        }

        FunctionServicePermissions& servicePerm = aosPermissions->Back();
        servicePerm.mName                       = service.c_str();

        for (const auto& [key, val] : permissions.permissions()) {
            if (err = servicePerm.mPermissions.PushBack({key.c_str(), val.c_str()}); !err.IsNone()) {
                LOG_ERR() << "Failed to push back permissions: error=" << err;

                return common::pbconvert::ConvertAosErrorToGrpcStatus(err);
            }
        }
    }

    StaticString<uuid::cUUIDLen> secret;

    Tie(secret, err) = GetPermHandler()->RegisterInstance(aosInstance, *aosPermissions);
    if (!err.IsNone()) {
        LOG_ERR() << "Register instance failed: error=" << err;

        return common::pbconvert::ConvertAosErrorToGrpcStatus(err);
    }

    response->set_secret(secret.CStr());

    return grpc::Status::OK;
}

grpc::Status ProtectedMessageHandler::UnregisterInstance([[maybe_unused]] grpc::ServerContext* context,
    const iamproto::UnregisterInstanceRequest* request, [[maybe_unused]] google::protobuf::Empty* response)
{
    const auto instance = common::pbconvert::ConvertToAos(request->instance());

    LOG_DBG() << "Process unregister instance: serviceID=" << instance.mServiceID
              << ", subjectID=" << instance.mSubjectID << ", instance=" << instance.mInstance;

    if (auto err = GetPermHandler()->UnregisterInstance(instance); !err.IsNone()) {
        LOG_ERR() << "Unregister instance failed: error=" << err;

        return common::pbconvert::ConvertAosErrorToGrpcStatus(err);
    }

    return grpc::Status::OK;
}

} // namespace aos::iam::iamserver
