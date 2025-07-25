/*
 * Copyright (C) 2024 Renesas Electronics Corporation.
 * Copyright (C) 2024 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gmock/gmock.h>

#include <aos/test/log.hpp>

#include <aos/common/crypto/cryptoprovider.hpp>
#include <aos/iam/certhandler.hpp>
#include <aos/iam/certmodules/pkcs11/pkcs11.hpp>
#include <utils/grpchelper.hpp>

#include "iamserver/protectedmessagehandler.hpp"

#include "mocks/certprovidermock.hpp"
#include "mocks/identhandlermock.hpp"
#include "mocks/nodeinfoprovidermock.hpp"
#include "mocks/nodemanagermock.hpp"
#include "mocks/permhandlermock.hpp"
#include "mocks/provisionmanagermock.hpp"
#include "stubs/storagestub.hpp"

using namespace testing;

namespace aos::iam::iamserver {

namespace {

/***********************************************************************************************************************
 * Static
 **********************************************************************************************************************/

constexpr auto cServerURL = "0.0.0.0:4456";
constexpr auto cSystemID  = "system-id";
constexpr auto cUnitModel = "unit-model";

template <typename T>
std::unique_ptr<typename T::Stub> CreateClientStub()
{
    auto tlsChannelCreds = grpc::InsecureChannelCredentials();

    if (tlsChannelCreds == nullptr) {
        return nullptr;
    }

    auto channel = grpc::CreateCustomChannel(cServerURL, tlsChannelCreds, grpc::ChannelArguments());
    if (channel == nullptr) {
        return nullptr;
    }

    return T::NewStub(channel);
}

} // namespace

/***********************************************************************************************************************
 * Suite
 **********************************************************************************************************************/

class ProtectedMessageHandlerTest : public Test {
protected:
    void InitServer();

    NodeController                mNodeController;
    ProtectedMessageHandler       mServerHandler;
    std::unique_ptr<grpc::Server> mServer;

    // mocks
    iam::identhandler::IdentHandlerMock         mIdentHandler;
    iam::permhandler::PermHandlerMock           mPermHandler;
    iam::nodeinfoprovider::NodeInfoProviderMock mNodeInfoProvider;
    iam::nodemanager::NodeManagerMock           mNodeManager;
    iam::provisionmanager::ProvisionManagerMock mProvisionManager;
    iam::certprovider::CertProviderMock         mCertProvider;

private:
    void SetUp() override;
    void TearDown() override;
};

void ProtectedMessageHandlerTest::InitServer()
{
    grpc::ServerBuilder builder;

    builder.AddListeningPort(cServerURL, grpc::InsecureServerCredentials());
    mServerHandler.RegisterServices(builder);

    mServer = builder.BuildAndStart();
}

void ProtectedMessageHandlerTest::SetUp()
{
    test::InitLog();

    EXPECT_CALL(mNodeInfoProvider, GetNodeInfo).WillRepeatedly(Invoke([&](NodeInfo& nodeInfo) {
        nodeInfo.mNodeID   = "node0";
        nodeInfo.mNodeType = "test-type";
        nodeInfo.mAttrs.PushBack({"MainNode", ""});

        LOG_DBG() << "NodeInfoProvider::GetNodeInfo: " << nodeInfo.mNodeID.CStr() << ", " << nodeInfo.mNodeType.CStr();

        return ErrorEnum::eNone;
    }));

    auto err = mServerHandler.Init(mNodeController, mIdentHandler, mPermHandler, mNodeInfoProvider, mNodeManager,
        mCertProvider, mProvisionManager);

    ASSERT_TRUE(err.IsNone()) << "Failed to initialize public message handler: " << err.Message();

    InitServer();
}

void ProtectedMessageHandlerTest::TearDown()
{
    if (mServer) {
        mServer->Shutdown();
        mServer->Wait();
    }

    mServerHandler.Close();
}

/***********************************************************************************************************************
 * IAMNodesService tests
 **********************************************************************************************************************/

TEST_F(ProtectedMessageHandlerTest, PauseNodeSucceeds)
{
    auto clientStub = CreateClientStub<iamproto::IAMNodesService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext         context;
    iamproto::PauseNodeRequest  request;
    iamproto::PauseNodeResponse response;

    request.set_node_id("node0");

    EXPECT_CALL(mNodeManager, SetNodeStatus).WillOnce(Invoke([](const String& nodeID, NodeStatus status) {
        EXPECT_EQ(nodeID, "node0");
        EXPECT_EQ(status.GetValue(), NodeStatusEnum::ePaused);

        return ErrorEnum::eNone;
    }));

    auto status = clientStub->PauseNode(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "PauseNode failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    EXPECT_EQ(response.error().aos_code(), static_cast<int>(ErrorEnum::eNone));
    EXPECT_TRUE(response.error().message().empty());
}

TEST_F(ProtectedMessageHandlerTest, PauseNodeFails)
{
    auto clientStub = CreateClientStub<iamproto::IAMNodesService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext         context;
    iamproto::PauseNodeRequest  request;
    iamproto::PauseNodeResponse response;

    request.set_node_id("node0");

    EXPECT_CALL(mNodeManager, SetNodeStatus).WillOnce(Invoke([](const String& nodeID, NodeStatus status) {
        EXPECT_EQ(nodeID, "node0");
        EXPECT_EQ(status.GetValue(), NodeStatusEnum::ePaused);

        return ErrorEnum::eFailed;
    }));

    auto status = clientStub->PauseNode(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "PauseNode failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    EXPECT_EQ(response.error().aos_code(), static_cast<int>(ErrorEnum::eFailed));
    EXPECT_FALSE(response.error().message().empty());
}

TEST_F(ProtectedMessageHandlerTest, ResumeNodeSucceeds)
{
    auto clientStub = CreateClientStub<iamproto::IAMNodesService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext          context;
    iamproto::ResumeNodeRequest  request;
    iamproto::ResumeNodeResponse response;

    request.set_node_id("node0");

    EXPECT_CALL(mNodeManager, SetNodeStatus).WillOnce(Invoke([](const String& nodeID, NodeStatus status) {
        EXPECT_EQ(nodeID, "node0");
        EXPECT_EQ(status.GetValue(), NodeStatusEnum::eProvisioned);

        return ErrorEnum::eNone;
    }));

    auto status = clientStub->ResumeNode(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "ResumeNode failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    EXPECT_EQ(response.error().aos_code(), static_cast<int>(ErrorEnum::eNone));
    EXPECT_TRUE(response.error().message().empty());
}

TEST_F(ProtectedMessageHandlerTest, ResumeNodeFails)
{
    auto clientStub = CreateClientStub<iamproto::IAMNodesService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext          context;
    iamproto::ResumeNodeRequest  request;
    iamproto::ResumeNodeResponse response;

    request.set_node_id("node0");

    EXPECT_CALL(mNodeManager, SetNodeStatus).WillOnce(Invoke([](const String& nodeID, NodeStatus status) {
        EXPECT_EQ(nodeID, "node0");
        EXPECT_EQ(status.GetValue(), NodeStatusEnum::eProvisioned);

        return ErrorEnum::eFailed;
    }));

    auto status = clientStub->ResumeNode(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "ResumeNode failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    EXPECT_EQ(response.error().aos_code(), static_cast<int>(ErrorEnum::eFailed));
    EXPECT_FALSE(response.error().message().empty());
}

/***********************************************************************************************************************
 * IAMProvisioningService tests
 **********************************************************************************************************************/

TEST_F(ProtectedMessageHandlerTest, GetCertTypesSucceeds)
{
    auto clientStub = CreateClientStub<iamproto::IAMProvisioningService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext           context;
    iamproto::GetCertTypesRequest request;
    iamproto::CertTypes           response;

    request.set_node_id("node0");

    iam::provisionmanager::CertTypes certTypes;
    certTypes.PushBack("type1");
    certTypes.PushBack("type2");

    EXPECT_CALL(mProvisionManager, GetCertTypes)
        .WillOnce(Return(RetWithError<iam::provisionmanager::CertTypes>(certTypes, ErrorEnum::eNone)));

    auto status = clientStub->GetCertTypes(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "GetCertTypes failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    ASSERT_EQ(response.types_size(), certTypes.Size());
    for (size_t i = 0; i < certTypes.Size(); i++) {
        EXPECT_EQ(String(response.types(i).c_str()), certTypes[i]);
    }
}

TEST_F(ProtectedMessageHandlerTest, GetCertTypesFails)
{
    auto clientStub = CreateClientStub<iamproto::IAMProvisioningService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext           context;
    iamproto::GetCertTypesRequest request;
    iamproto::CertTypes           response;

    request.set_node_id("node0");

    EXPECT_CALL(mProvisionManager, GetCertTypes)
        .WillOnce(Return(RetWithError<iam::provisionmanager::CertTypes>({}, ErrorEnum::eFailed)));

    auto status = clientStub->GetCertTypes(&context, request, &response);

    ASSERT_FALSE(status.ok());
}

TEST_F(ProtectedMessageHandlerTest, StartProvisioningSucceeds)
{
    auto clientStub = CreateClientStub<iamproto::IAMProvisioningService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext                 context;
    iamproto::StartProvisioningRequest  request;
    iamproto::StartProvisioningResponse response;

    request.set_node_id("node0");

    EXPECT_CALL(mProvisionManager, StartProvisioning).WillOnce(Return(ErrorEnum::eNone));

    auto status = clientStub->StartProvisioning(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "StartProvisioning failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    EXPECT_EQ(response.error().aos_code(), static_cast<int>(ErrorEnum::eNone));
    EXPECT_TRUE(response.error().message().empty());
}

TEST_F(ProtectedMessageHandlerTest, StartProvisioningFails)
{
    auto clientStub = CreateClientStub<iamproto::IAMProvisioningService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext                 context;
    iamproto::StartProvisioningRequest  request;
    iamproto::StartProvisioningResponse response;

    request.set_node_id("node0");

    EXPECT_CALL(mProvisionManager, StartProvisioning).WillOnce(Return(ErrorEnum::eFailed));

    auto status = clientStub->StartProvisioning(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "StartProvisioning failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    EXPECT_EQ(response.error().aos_code(), static_cast<int>(ErrorEnum::eFailed));
    EXPECT_FALSE(response.error().message().empty());
}

TEST_F(ProtectedMessageHandlerTest, FinishProvisioningSucceeds)
{
    auto clientStub = CreateClientStub<iamproto::IAMProvisioningService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext                  context;
    iamproto::FinishProvisioningRequest  request;
    iamproto::FinishProvisioningResponse response;

    request.set_node_id("node0");

    EXPECT_CALL(mProvisionManager, FinishProvisioning).WillOnce(Return(ErrorEnum::eNone));

    auto status = clientStub->FinishProvisioning(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "FinishProvisioning failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    EXPECT_EQ(response.error().aos_code(), static_cast<int>(ErrorEnum::eNone));
    EXPECT_TRUE(response.error().message().empty());
}

TEST_F(ProtectedMessageHandlerTest, FinishProvisioningFails)
{
    auto clientStub = CreateClientStub<iamproto::IAMProvisioningService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext                  context;
    iamproto::FinishProvisioningRequest  request;
    iamproto::FinishProvisioningResponse response;

    request.set_node_id("node0");

    EXPECT_CALL(mProvisionManager, FinishProvisioning).WillOnce(Return(ErrorEnum::eFailed));

    auto status = clientStub->FinishProvisioning(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "FinishProvisioning failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    EXPECT_EQ(response.error().aos_code(), static_cast<int>(ErrorEnum::eFailed));
    EXPECT_FALSE(response.error().message().empty());
}

TEST_F(ProtectedMessageHandlerTest, DeprovisionSucceeds)
{
    auto clientStub = CreateClientStub<iamproto::IAMProvisioningService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext           context;
    iamproto::DeprovisionRequest  request;
    iamproto::DeprovisionResponse response;

    request.set_node_id("node0");

    EXPECT_CALL(mProvisionManager, Deprovision).WillOnce(Return(ErrorEnum::eNone));

    auto status = clientStub->Deprovision(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "Deprovision failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    EXPECT_EQ(response.error().aos_code(), static_cast<int>(ErrorEnum::eNone));
    EXPECT_TRUE(response.error().message().empty());
}

TEST_F(ProtectedMessageHandlerTest, DeprovisionFails)
{
    auto clientStub = CreateClientStub<iamproto::IAMProvisioningService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext           context;
    iamproto::DeprovisionRequest  request;
    iamproto::DeprovisionResponse response;

    request.set_node_id("node0");

    EXPECT_CALL(mProvisionManager, Deprovision).WillOnce(Return(ErrorEnum::eFailed));

    auto status = clientStub->Deprovision(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "Deprovision failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    EXPECT_EQ(response.error().aos_code(), static_cast<int>(ErrorEnum::eFailed));
    EXPECT_FALSE(response.error().message().empty());
}

/***********************************************************************************************************************
 * IAMCertificateService tests
 **********************************************************************************************************************/

TEST_F(ProtectedMessageHandlerTest, CreateKeySucceeds)
{
    auto clientStub = CreateClientStub<iamproto::IAMCertificateService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext         context;
    iamproto::CreateKeyRequest  request;
    iamproto::CreateKeyResponse response;

    request.set_node_id("node0");

    EXPECT_CALL(mProvisionManager, CreateKey).WillOnce(Return(ErrorEnum::eNone));
    EXPECT_CALL(mIdentHandler, GetSystemID).WillOnce(Return(RetWithError<StaticString<cSystemIDLen>>(cSystemID)));

    auto status = clientStub->CreateKey(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "CreateKey failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    EXPECT_EQ(response.error().aos_code(), static_cast<int>(ErrorEnum::eNone));
    EXPECT_TRUE(response.error().message().empty());
}

TEST_F(ProtectedMessageHandlerTest, ApplyCertSucceeds)
{
    auto clientStub = CreateClientStub<iamproto::IAMCertificateService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext         context;
    iamproto::ApplyCertRequest  request;
    iamproto::ApplyCertResponse response;

    request.set_node_id("node0");
    request.set_type("cert-type");

    EXPECT_CALL(mProvisionManager, ApplyCert).WillOnce(Return(ErrorEnum::eNone));

    auto status = clientStub->ApplyCert(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "ApplyCert failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    EXPECT_EQ(response.node_id(), "node0");
    EXPECT_EQ(response.type(), "cert-type");

    EXPECT_EQ(response.error().aos_code(), static_cast<int>(ErrorEnum::eNone));
    EXPECT_TRUE(response.error().message().empty());
}

TEST_F(ProtectedMessageHandlerTest, ApplyCertFails)
{
    auto clientStub = CreateClientStub<iamproto::IAMCertificateService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext         context;
    iamproto::ApplyCertRequest  request;
    iamproto::ApplyCertResponse response;

    request.set_node_id("node0");
    request.set_type("cert-type");

    EXPECT_CALL(mProvisionManager, ApplyCert).WillOnce(Return(ErrorEnum::eFailed));

    auto status = clientStub->ApplyCert(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "ApplyCert failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    EXPECT_EQ(response.node_id(), "node0");
    EXPECT_EQ(response.type(), "cert-type");

    EXPECT_EQ(response.error().aos_code(), static_cast<int>(ErrorEnum::eFailed));
    EXPECT_FALSE(response.error().message().empty());
}

/***********************************************************************************************************************
 * IAMPermissionsService tests
 **********************************************************************************************************************/

TEST_F(ProtectedMessageHandlerTest, RegisterInstanceSucceeds)
{
    auto clientStub = CreateClientStub<iamproto::IAMPermissionsService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext                context;
    iamproto::RegisterInstanceRequest  request;
    iamproto::RegisterInstanceResponse response;

    request.mutable_instance()->set_service_id("service-id-1");
    request.mutable_instance()->set_subject_id("subject-id-1");
    request.mutable_permissions()->operator[]("permission-1").mutable_permissions()->insert({"key", "value"});

    EXPECT_CALL(mPermHandler, RegisterInstance)
        .WillOnce(Return(RetWithError<StaticString<iam::permhandler::cSecretLen>>("test-secret")));

    const auto status = clientStub->RegisterInstance(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "RegisterInstance failed: code = " << status.error_code()
                             << ", message = " << status.error_message();

    ASSERT_EQ(response.secret(), "test-secret");
}

TEST_F(ProtectedMessageHandlerTest, RegisterInstanceFailsNoMemory)
{
    auto clientStub = CreateClientStub<iamproto::IAMPermissionsService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext                context;
    iamproto::RegisterInstanceRequest  request;
    iamproto::RegisterInstanceResponse response;

    request.mutable_instance()->set_service_id("service-id-1");
    request.mutable_instance()->set_subject_id("subject-id-1");

    // fill permissions with more items than allowed
    for (size_t i = 0; i < cMaxNumServices + 1; i++) {
        (*request.mutable_permissions())[std::to_string(i)].mutable_permissions()->insert({"key", "value"});
    }

    EXPECT_CALL(mPermHandler, RegisterInstance).Times(0);

    const auto status = clientStub->RegisterInstance(&context, request, &response);

    ASSERT_FALSE(status.ok());
}

TEST_F(ProtectedMessageHandlerTest, RegisterInstanceFailsOnPermHandler)
{
    auto clientStub = CreateClientStub<iamproto::IAMPermissionsService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext                context;
    iamproto::RegisterInstanceRequest  request;
    iamproto::RegisterInstanceResponse response;

    EXPECT_CALL(mPermHandler, RegisterInstance)
        .WillOnce(Return(RetWithError<StaticString<iam::permhandler::cSecretLen>>("", ErrorEnum::eFailed)));

    auto status = clientStub->RegisterInstance(&context, request, &response);

    ASSERT_FALSE(status.ok());
}

TEST_F(ProtectedMessageHandlerTest, UnregisterInstanceSucceeds)
{
    auto clientStub = CreateClientStub<iamproto::IAMPermissionsService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext                 context;
    iamproto::UnregisterInstanceRequest request;
    google::protobuf::Empty             response;

    EXPECT_CALL(mPermHandler, UnregisterInstance).WillOnce(Return(ErrorEnum::eNone));

    auto status = clientStub->UnregisterInstance(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "UnregisterInstanceFails failed: code = " << status.error_code()
                             << ", message = " << status.error_message();
}

TEST_F(ProtectedMessageHandlerTest, UnregisterInstanceFails)
{
    auto clientStub = CreateClientStub<iamproto::IAMPermissionsService>();
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext                 context;
    iamproto::UnregisterInstanceRequest request;
    google::protobuf::Empty             response;

    EXPECT_CALL(mPermHandler, UnregisterInstance).WillOnce(Return(ErrorEnum::eFailed));

    auto status = clientStub->UnregisterInstance(&context, request, &response);

    ASSERT_FALSE(status.ok());
}

} // namespace aos::iam::iamserver
