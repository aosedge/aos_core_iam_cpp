/*
 * Copyright (C) 2024 Renesas Electronics Corporation.
 * Copyright (C) 2024 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PUBLICMESSAGEHANDLER_HPP_
#define PUBLICMESSAGEHANDLER_HPP_

#include <array>
#include <optional>
#include <shared_mutex>
#include <string>

#include <grpcpp/server_builder.h>

#include <aos/common/crypto/utils.hpp>
#include <aos/iam/certhandler.hpp>
#include <aos/iam/certprovider.hpp>
#include <aos/iam/identhandler.hpp>
#include <aos/iam/nodeinfoprovider.hpp>
#include <aos/iam/nodemanager.hpp>
#include <aos/iam/permhandler.hpp>
#include <pbconvert/common.hpp>

#include <iamanager/version.grpc.pb.h>

#include "nodecontroller.hpp"
#include "streamwriter.hpp"

namespace aos::iam::iamserver {

/**
 * Public message handler. Responsible for handling public IAM services.
 */
class PublicMessageHandler :
    // public services
    protected iamanager::IAMVersionService::Service,
    protected iamproto::IAMPublicService::Service,
    protected iamproto::IAMPublicIdentityService::Service,
    protected iamproto::IAMPublicPermissionsService::Service,
    protected iamproto::IAMPublicNodesService::Service,
    // NodeInfo listener interface.
    public iam::nodemanager::NodeInfoListenerItf,
    // identhandler subject observer interface
    public iam::identhandler::SubjectsObserverItf {
public:
    /**
     * Initializes public message handler instance.
     *
     * @param nodeController node controller.
     * @param identHandler identification handler.
     * @param permHandler permission handler.
     * @param nodeInfoProvider node info provider.
     * @param nodeManager node manager.
     * @param certProvider certificate provider.
     */
    Error Init(NodeController& nodeController, iam::identhandler::IdentHandlerItf& identHandler,
        iam::permhandler::PermHandlerItf& permHandler, iam::nodeinfoprovider::NodeInfoProviderItf& nodeInfoProvider,
        iam::nodemanager::NodeManagerItf& nodeManager, iam::certprovider::CertProviderItf& certProvider);

    /**
     * Registers grpc services.
     *
     * @param builder server builder.
     */
    void RegisterServices(grpc::ServerBuilder& builder);

    /**
     * Node info change notification.
     *
     * @param info node info.
     */
    void OnNodeInfoChange(const NodeInfo& info) override;

    /**
     * Node info removed notification.
     *
     * @param id id of the node been removed.
     */
    void OnNodeRemoved(const String& id) override;

    /**
     * Subjects observer interface implementation.
     *
     * @param[in] messages subject changed messages.
     * @returns Error.
     */
    Error SubjectsChanged(const Array<StaticString<cSubjectIDLen>>& messages) override;

    /**
     * Start public message handler.
     */
    void Start();

    /**
     * Closes public message handler.
     */
    void Close();

protected:
    iam::identhandler::IdentHandlerItf*         GetIdentHandler() { return mIdentHandler; }
    iam::permhandler::PermHandlerItf*           GetPermHandler() { return mPermHandler; }
    iam::nodeinfoprovider::NodeInfoProviderItf* GetNodeInfoProvider() { return mNodeInfoProvider; }
    NodeController*                             GetNodeController() { return mNodeController; }
    NodeInfo&                                   GetNodeInfo() { return mNodeInfo; }
    iam::nodemanager::NodeManagerItf*           GetNodeManager() { return mNodeManager; }
    Error                                       SetNodeStatus(const std::string& nodeID, const NodeStatus& status);
    bool                                        ProcessOnThisNode(const std::string& nodeID);

    template <typename R>
    grpc::Status RequestWithRetry(R request)
    {
        std::unique_lock lock {mMutex};

        grpc::Status status = grpc::Status::OK;

        for (auto i = 0; i < cRequestRetryMaxTry; i++) {
            if (mClose) {
                return common::pbconvert::ConvertAosErrorToGrpcStatus({ErrorEnum::eWrongState, "handler is closed"});
            }

            if (status = request(); status.ok()) {
                return status;
            }

            mRetryCondVar.wait_for(lock, cRequestRetryTimeout, [this] { return mClose; });
        }

        return status;
    }

private:
    static constexpr auto       cIamAPIVersion       = 5;
    static constexpr std::array cAllowedStatuses     = {NodeStatusEnum::eUnprovisioned};
    static constexpr auto       cRequestRetryTimeout = std::chrono::seconds(10);
    static constexpr auto       cRequestRetryMaxTry  = 3;

    // IAMVersionService interface
    grpc::Status GetAPIVersion(
        grpc::ServerContext* context, const google::protobuf::Empty* request, iamanager::APIVersion* response) override;

    // IAMPublicService interface
    grpc::Status GetNodeInfo(
        grpc::ServerContext* context, const google::protobuf::Empty* request, iamproto::NodeInfo* response) override;
    grpc::Status GetCert(
        grpc::ServerContext* context, const iamproto::GetCertRequest* request, iamproto::CertInfo* response) override;
    grpc::Status SubscribeCertChanged(grpc::ServerContext* context,
        const iamanager::v5::SubscribeCertChangedRequest*  request,
        grpc::ServerWriter<iamanager::v5::CertInfo>*       writer) override;

    // IAMPublicIdentityService interface
    grpc::Status GetSystemInfo(
        grpc::ServerContext* context, const google::protobuf::Empty* request, iamproto::SystemInfo* response) override;
    grpc::Status GetSubjects(
        grpc::ServerContext* context, const google::protobuf::Empty* request, iamproto::Subjects* response) override;
    grpc::Status SubscribeSubjectsChanged(grpc::ServerContext* context, const google::protobuf::Empty* request,
        grpc::ServerWriter<iamproto::Subjects>* writer) override;

    // IAMPublicPermissionsService interface
    grpc::Status GetPermissions(grpc::ServerContext* context, const iamproto::PermissionsRequest* request,
        iamproto::PermissionsResponse* response) override;

    // IAMPublicNodesService interface
    grpc::Status GetAllNodeIDs(
        grpc::ServerContext* context, const google::protobuf::Empty* request, iamproto::NodesID* response) override;
    grpc::Status GetNodeInfo(grpc::ServerContext* context, const iamproto::GetNodeInfoRequest* request,
        iamproto::NodeInfo* response) override;
    grpc::Status SubscribeNodeChanged(grpc::ServerContext* context, const google::protobuf::Empty* request,
        grpc::ServerWriter<iamproto::NodeInfo>* writer) override;
    grpc::Status RegisterNode(grpc::ServerContext*                                              context,
        grpc::ServerReaderWriter<iamproto::IAMIncomingMessages, iamproto::IAMOutgoingMessages>* stream) override;

    iam::identhandler::IdentHandlerItf*         mIdentHandler     = nullptr;
    iam::permhandler::PermHandlerItf*           mPermHandler      = nullptr;
    iam::nodeinfoprovider::NodeInfoProviderItf* mNodeInfoProvider = nullptr;
    iam::nodemanager::NodeManagerItf*           mNodeManager      = nullptr;
    iam::certprovider::CertProviderItf*         mCertProvider     = nullptr;
    NodeController*                             mNodeController   = nullptr;
    StreamWriter<iamproto::NodeInfo>            mNodeChangedController;
    StreamWriter<iamproto::Subjects>            mSubjectsChangedController;
    NodeInfo                                    mNodeInfo;

    std::vector<std::shared_ptr<CertWriter>> mCertWriters;
    std::mutex                               mCertWritersLock;
    std::condition_variable                  mRetryCondVar;
    std::mutex                               mMutex;
    bool                                     mClose = false;
};

} // namespace aos::iam::iamserver

#endif
