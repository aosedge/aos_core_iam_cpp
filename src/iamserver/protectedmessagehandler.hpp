/*
 * Copyright (C) 2024 Renesas Electronics Corporation.
 * Copyright (C) 2024 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PROTECTEDMESSAGEHANDLER_HPP_
#define PROTECTEDMESSAGEHANDLER_HPP_

#include <array>
#include <chrono>
#include <string>

#include <grpcpp/server_builder.h>

#include <aos/common/crypto/utils.hpp>
#include <aos/iam/certhandler.hpp>
#include <aos/iam/identhandler.hpp>
#include <aos/iam/nodeinfoprovider.hpp>
#include <aos/iam/permhandler.hpp>
#include <aos/iam/provisionmanager.hpp>
#include <config/config.hpp>

#include <iamanager/v5/iamanager.grpc.pb.h>

#include "nodecontroller.hpp"
#include "publicmessagehandler.hpp"

namespace aos::iam::iamserver {

/**
 * Protected message handler. Responsible for handling protected IAM services.
 */
class ProtectedMessageHandler :
    // public services
    public PublicMessageHandler,
    // protected services
    private iamproto::IAMNodesService::Service,
    private iamproto::IAMProvisioningService::Service,
    private iamproto::IAMCertificateService::Service,
    private iamproto::IAMPermissionsService::Service {
public:
    /**
     * Initializes protected message handler instance.
     *
     * @param nodeController node controller.
     * @param identHandler identification handler.
     * @param permHandler permission handler.
     * @param nodeInfoProvider node info provider.
     * @param nodeManager node manager.
     * @param certProvider certificate provider.
     * @param provisionManager provision manager.
     */
    Error Init(NodeController& nodeController, iam::identhandler::IdentHandlerItf& identHandler,
        iam::permhandler::PermHandlerItf& permHandler, iam::nodeinfoprovider::NodeInfoProviderItf& nodeInfoProvider,
        iam::nodemanager::NodeManagerItf& nodeManager, iam::certprovider::CertProviderItf& certProvider,
        iam::provisionmanager::ProvisionManagerItf& provisionManager);

    /**
     * Registers grpc services.
     *
     * @param builder server builder.
     */
    // cppcheck-suppress duplInheritedMember
    void RegisterServices(grpc::ServerBuilder& builder);

    using PublicMessageHandler::OnNodeInfoChange;
    using PublicMessageHandler::OnNodeRemoved;

    // identhandler::SubjectsObserverItf interface
    using PublicMessageHandler::SubjectsChanged;

    /**
     * Closes protected message handler.
     */
    // cppcheck-suppress duplInheritedMember
    void Close();

private:
    static constexpr auto       cDefaultTimeout      = std::chrono::minutes(1);
    static constexpr auto       cProvisioningTimeout = std::chrono::minutes(5);
    static constexpr std::array cAllowedStatuses     = {NodeStatusEnum::eProvisioned, NodeStatusEnum::ePaused};

    // IAMPublicNodesService interface
    grpc::Status RegisterNode(grpc::ServerContext*                                              context,
        grpc::ServerReaderWriter<iamproto::IAMIncomingMessages, iamproto::IAMOutgoingMessages>* stream) override;

    // IAMNodesService interface
    grpc::Status PauseNode(grpc::ServerContext* context, const iamproto::PauseNodeRequest* request,
        iamproto::PauseNodeResponse* response) override;
    grpc::Status ResumeNode(grpc::ServerContext* context, const iamproto::ResumeNodeRequest* request,
        iamproto::ResumeNodeResponse* response) override;

    // IAMProvisioningService interface
    grpc::Status GetCertTypes(grpc::ServerContext* context, const iamproto::GetCertTypesRequest* request,
        iamproto::CertTypes* response) override;
    grpc::Status StartProvisioning(grpc::ServerContext* context, const iamproto::StartProvisioningRequest* request,
        iamproto::StartProvisioningResponse* response) override;
    grpc::Status FinishProvisioning(grpc::ServerContext* context, const iamproto::FinishProvisioningRequest* request,
        iamproto::FinishProvisioningResponse* response) override;
    grpc::Status Deprovision(grpc::ServerContext* context, const iamproto::DeprovisionRequest* request,
        iamproto::DeprovisionResponse* response) override;

    // IAMCertificateService interface
    grpc::Status CreateKey(grpc::ServerContext* context, const iamproto::CreateKeyRequest* request,
        iamproto::CreateKeyResponse* response) override;
    grpc::Status ApplyCert(grpc::ServerContext* context, const iamproto::ApplyCertRequest* request,
        iamproto::ApplyCertResponse* response) override;

    // IAMPermissionsService interface
    grpc::Status RegisterInstance(grpc::ServerContext* context, const iamproto::RegisterInstanceRequest* request,
        iamproto::RegisterInstanceResponse* response) override;
    grpc::Status UnregisterInstance(grpc::ServerContext* context, const iamproto::UnregisterInstanceRequest* request,
        google::protobuf::Empty* response) override;

    iam::provisionmanager::ProvisionManagerItf* mProvisionManager = nullptr;
};

} // namespace aos::iam::iamserver

#endif
