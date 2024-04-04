/*
 * Copyright (C) 2024 Renesas Electronics Corporation.
 * Copyright (C) 2024 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gmock/gmock.h>

#include <test/utils/log.hpp>
#include <test/utils/softhsmenv.hpp>

#include <aos/common/crypto/mbedtls/cryptoprovider.hpp>
#include <aos/iam/certhandler.hpp>
#include <aos/iam/certmodules/pkcs11/pkcs11.hpp>

#include "iam/client/iamclient.hpp"
#include "iam/grpchelper.hpp"
#include "iam/server/iamserver.hpp"
#include "mocks/identhandlermock.hpp"
#include "mocks/permissionhandlermock.hpp"
#include "mocks/remoteiamhandlermock.hpp"
#include "storagestub.hpp"

using namespace testing;
using namespace aos;
using namespace aos::iam;
using namespace aos::iam::certhandler;
using namespace iamanager::v4;

/***********************************************************************************************************************
 * Suite
 **********************************************************************************************************************/

class IAMTest : public Test {
protected:
    // Default parameters
    static constexpr auto cPIN                 = "admin";
    static constexpr auto cLabel               = "iam-server-test-slot";
    static constexpr auto cMaxModulesCount     = 3;
    static constexpr auto cSystemID            = "system-id";
    static constexpr auto cUnitModel           = "unit-model";
    static constexpr auto cProvisioningModeOn  = true;
    static constexpr auto cProvisioningModeOff = false;

    void RegisterPKCS11Module(const String& name, crypto::KeyType keyType = crypto::KeyTypeEnum::eRSA);
    void SetUpCertificates();

    template <typename T>
    std::unique_ptr<typename T::Stub> CreateCustomStub(
        const CertInfo& certInfo, const std::string& url, const bool insecure = false)
    {
        auto tlsChannelCreds = insecure
            ? grpc::InsecureChannelCredentials()
            : GetTlsChannelCredentials(certInfo, GetClientConfig().mCACert.c_str(), mCertLoader, mCryptoProvider);
        if (tlsChannelCreds == nullptr) {
            return nullptr;
        }

        auto channel = grpc::CreateCustomChannel(url, tlsChannelCreds, grpc::ChannelArguments());
        if (channel == nullptr) {
            return nullptr;
        }

        return T::NewStub(channel);
    }

    IAMServer mServer;
    CertInfo  mClientInfo;
    CertInfo  mServerInfo;
    Config    mServerConfig;
    Config    mClientConfig;

    CertHandler                   mCertHandler;
    crypto::MbedTLSCryptoProvider mCryptoProvider;
    cryptoutils::CertLoader       mCertLoader;

    // mocks
    identhandler::IdentHandlerMock        mIdentHandler;
    permhandler::PermHandlerMock          mPermHandler;
    std::unique_ptr<RemoteIAMHandlerMock> mRemoteIAMHandler;

private:
    void SetUp() override;
    void TearDown() override;

    // CertHandler function
    certhandler::ModuleConfig GetCertModuleConfig(crypto::KeyType keyType);
    PKCS11ModuleConfig        GetPKCS11ModuleConfig();
    void ApplyCertificate(const String& certType, const String& subject, const String& intermKeyPath,
        const String& intermCertPath, uint64_t serial, CertInfo& certInfo);

    Config GetServerConfig();
    Config GetClientConfig();

    test::SoftHSMEnv                            mSOFTHSMEnv;
    StorageStub                                 mStorage;
    StaticArray<PKCS11Module, cMaxModulesCount> mPKCS11Modules;
    StaticArray<CertModule, cMaxModulesCount>   mCertModules;
};

void IAMTest::SetUp()
{
    InitLogs();

    ASSERT_TRUE(mCryptoProvider.Init().IsNone());
    ASSERT_TRUE(mSOFTHSMEnv
                    .Init("", "certhandler-integration-tests", SOFTHSM_BASE_DIR "/softhsm2.conf",
                        SOFTHSM_BASE_DIR "/tokens", SOFTHSM2_LIB)
                    .IsNone());
    ASSERT_TRUE(mCertLoader.Init(mCryptoProvider, mSOFTHSMEnv.GetManager()).IsNone());

    RegisterPKCS11Module("client");
    ASSERT_TRUE(mCertHandler.SetOwner("client", cPIN).IsNone());

    RegisterPKCS11Module("server");

    ApplyCertificate("client", "client", CERTIFICATES_DIR "/client_int.key", CERTIFICATES_DIR "/client_int.cer",
        0x3333444, mClientInfo);
    ApplyCertificate("server", "localhost", CERTIFICATES_DIR "/server_int.key", CERTIFICATES_DIR "/server_int.cer",
        0x3333333, mServerInfo);

    mServerConfig = GetServerConfig();
    mClientConfig = GetClientConfig();
}

void IAMTest::TearDown()
{
    FS::ClearDir(SOFTHSM_BASE_DIR "/tokens");
}

void IAMTest::RegisterPKCS11Module(const String& name, crypto::KeyType keyType)
{
    ASSERT_TRUE(mPKCS11Modules.EmplaceBack().IsNone());
    ASSERT_TRUE(mCertModules.EmplaceBack().IsNone());
    auto& pkcs11Module = mPKCS11Modules.Back().mValue;
    auto& certModule   = mCertModules.Back().mValue;
    ASSERT_TRUE(pkcs11Module.Init(name, GetPKCS11ModuleConfig(), mSOFTHSMEnv.GetManager(), mCryptoProvider).IsNone());
    ASSERT_TRUE(certModule.Init(name, GetCertModuleConfig(keyType), mCryptoProvider, pkcs11Module, mStorage).IsNone());
    ASSERT_TRUE(mCertHandler.RegisterModule(certModule).IsNone());
}

Config IAMTest::GetServerConfig()
{
    Config config;

    config.mCertStorage               = "server";
    config.mCACert                    = CERTIFICATES_DIR "/ca.cer";
    config.mIAMPublicServerURL        = "localhost:8088";
    config.mIAMProtectedServerURL     = "localhost:8089";
    config.mNodeID                    = "node0";
    config.mNodeType                  = "iam-node-type";
    config.mFinishProvisioningCmdArgs = config.mDiskEncryptionCmdArgs = {};

    return config;
}

Config IAMTest::GetClientConfig()
{
    Config config;

    config.mCertStorage               = "client";
    config.mCACert                    = CERTIFICATES_DIR "/ca.cer";
    config.mIAMPublicServerURL        = "localhost:8088";
    config.mIAMProtectedServerURL     = "localhost:8089";
    config.mNodeID                    = "iam-node-id";
    config.mNodeType                  = "iam-node-type";
    config.mFinishProvisioningCmdArgs = config.mDiskEncryptionCmdArgs = {};
    config.mRemoteIAMs = {RemoteIAM {"node0", "localhost:8089", std::chrono::seconds(100)}};

    return config;
}

certhandler::ModuleConfig IAMTest::GetCertModuleConfig(crypto::KeyType keyType)
{
    certhandler::ModuleConfig config;
    config.mKeyType         = keyType;
    config.mMaxCertificates = 2;
    config.mExtendedKeyUsage.EmplaceBack(ExtendedKeyUsageEnum::eClientAuth);
    config.mAlternativeNames.EmplaceBack("epam.com");
    config.mAlternativeNames.EmplaceBack("www.epam.com");
    config.mSkipValidation = false;
    return config;
}

PKCS11ModuleConfig IAMTest::GetPKCS11ModuleConfig()
{
    PKCS11ModuleConfig config;
    config.mLibrary         = SOFTHSM2_LIB;
    config.mSlotID          = mSOFTHSMEnv.GetSlotID();
    config.mUserPINPath     = CERTIFICATES_DIR "/pin.txt";
    config.mModulePathInURL = true;
    return config;
}

void IAMTest::ApplyCertificate(const String& certType, const String& subject, const String& intermKeyPath,
    const String& intermCertPath, uint64_t serial, CertInfo& certInfo)
{
    StaticString<crypto::cCSRPEMLen> csr;
    ASSERT_TRUE(mCertHandler.CreateKey(certType, subject, cPIN, csr).IsNone());

    // create certificate from CSR, CA priv key, CA cert
    StaticString<crypto::cPrivKeyPEMLen> intermKey;
    ASSERT_TRUE(FS::ReadFileToString(intermKeyPath, intermKey).IsNone());

    StaticString<crypto::cCertPEMLen> intermCert;
    ASSERT_TRUE(FS::ReadFileToString(intermCertPath, intermCert).IsNone());

    auto                              serialArr = Array<uint8_t>(reinterpret_cast<uint8_t*>(&serial), sizeof(serial));
    StaticString<crypto::cCertPEMLen> clientCertChain;

    ASSERT_TRUE(mCryptoProvider.CreateClientCert(csr, intermKey, intermCert, serialArr, clientCertChain).IsNone());

    // add intermediate cert to the chain
    clientCertChain.Append(intermCert);

    // add CA certificate to the chain
    StaticString<crypto::cCertPEMLen> caCert;

    ASSERT_TRUE(FS::ReadFileToString(CERTIFICATES_DIR "/ca.cer", caCert).IsNone());
    clientCertChain.Append(caCert);

    // apply client certificate
    // FS::WriteStringToFile(CERTIFICATES_DIR "/client-out.pem", clientCertChain, 0666);
    ASSERT_TRUE(mCertHandler.ApplyCertificate(certType, clientCertChain, certInfo).IsNone());
    EXPECT_EQ(certInfo.mSerial, serialArr);
}

/***********************************************************************************************************************
 * IAMServer tests
 **********************************************************************************************************************/

TEST_F(IAMTest, InitSucceeds)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();
}

TEST_F(IAMTest, InitFails)
{
    mServerConfig.mCertStorage = "unknown";

    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, false);
    ASSERT_FALSE(err.IsNone());
}

/***********************************************************************************************************************
 * IAMCertificateService tests
 **********************************************************************************************************************/

TEST_F(IAMTest, CreateKey)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOff).IsNone());

    StaticString<crypto::cCSRPEMLen> csr;
    err = client.CreateKey("node0", "server", "Aos Cloud", cPIN, csr);
    ASSERT_TRUE(err.IsNone());
}

TEST_F(IAMTest, CreateKeyFailOnUnkownNodeId)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOff).IsNone());

    StaticString<crypto::cCSRPEMLen> csr;
    err = client.CreateKey("unknown-node", "server", "Aos Cloud", cPIN, csr);
    ASSERT_FALSE(err.IsNone()) << err.Message();
}

TEST_F(IAMTest, ApplyCertSucceeds)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOff).IsNone());

    CertInfo                         resultInfo;
    StaticString<crypto::cCSRPEMLen> cert;

    StaticString<crypto::cCSRPEMLen> csr;
    err = client.CreateKey("node0", "client", "Aos Cloud", cPIN, csr);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    // create certificate from CSR, CA priv key, CA cert
    StaticString<crypto::cPrivKeyPEMLen> caKey;
    ASSERT_TRUE(FS::ReadFileToString(CERTIFICATES_DIR "/ca.key", caKey).IsNone());

    StaticString<crypto::cCertPEMLen> caCert;
    ASSERT_TRUE(FS::ReadFileToString(CERTIFICATES_DIR "/ca.cer", caCert).IsNone());

    const uint64_t serial    = 0x3333555;
    auto           serialArr = Array<uint8_t>(reinterpret_cast<const uint8_t*>(&serial), sizeof(serial));
    StaticString<crypto::cCertPEMLen> clientCertChain;

    ASSERT_TRUE(mCryptoProvider.CreateClientCert(csr, caKey, caCert, serialArr, clientCertChain).IsNone());

    // add CA cert to the chain
    clientCertChain.Append(caCert);

    err = client.ApplyCertificate("node0", "client", clientCertChain, resultInfo);
    ASSERT_TRUE(err.IsNone()) << err.Message();
}

TEST_F(IAMTest, ApplyCertFails)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOff).IsNone());

    CertInfo                         resultInfo;
    StaticString<crypto::cCSRPEMLen> cert;

    err = client.ApplyCertificate("node0", "server", cert, resultInfo);
    ASSERT_TRUE(err.Is(aos::ErrorEnum::eFailed)) << err.Message();
}

/***********************************************************************************************************************
 * IAMPermissionsService tests
 **********************************************************************************************************************/

TEST_F(IAMTest, RegisterInstanceSucceeds)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPermissionsService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext     context;
    RegisterInstanceRequest request;
    request.mutable_instance()->set_service_id("service-id-1");
    request.mutable_instance()->set_subject_id("subject-id-1");
    request.mutable_permissions()->operator[]("permission-1").mutable_permissions()->insert({"key", "value"});

    RegisterInstanceResponse response;

    EXPECT_CALL(mPermHandler, RegisterInstance)
        .WillOnce(Return(RetWithError<StaticString<uuid::cUUIDStrLen>>("test-secret")));

    const auto status = clientStub->RegisterInstance(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
    ASSERT_EQ(response.secret(), "test-secret");
}

TEST_F(IAMTest, RegisterInstanceSucceedsNoMemory)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPermissionsService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext     context;
    RegisterInstanceRequest request;
    request.mutable_instance()->set_service_id("service-id-1");
    request.mutable_instance()->set_subject_id("subject-id-1");

    // fill permissions with more items than allowed
    for (size_t i = 0; i < aos::cMaxNumServices + 1; i++) {
        (*request.mutable_permissions())[std::to_string(i)].mutable_permissions()->insert({"key", "value"});
    }

    RegisterInstanceResponse response;

    EXPECT_CALL(mPermHandler, RegisterInstance).Times(0);

    const auto status = clientStub->RegisterInstance(&context, request, &response);
    ASSERT_FALSE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
}

TEST_F(IAMTest, RegisterInstanceFailsOnPermHandler)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPermissionsService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext      context;
    RegisterInstanceRequest  request;
    RegisterInstanceResponse response;

    EXPECT_CALL(mPermHandler, RegisterInstance)
        .WillOnce(Return(RetWithError<StaticString<uuid::cUUIDStrLen>>("", aos::ErrorEnum::eFailed)));

    const auto status = clientStub->RegisterInstance(&context, request, &response);
    ASSERT_FALSE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
}

TEST_F(IAMTest, UnregisterInstanceSucceeds)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPermissionsService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext       context;
    UnregisterInstanceRequest request;
    google::protobuf::Empty   response;

    EXPECT_CALL(mPermHandler, UnregisterInstance).WillOnce(Return(aos::ErrorEnum::eNone));

    const auto status = clientStub->UnregisterInstance(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
}

TEST_F(IAMTest, UnregisterInstanceFails)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPermissionsService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext       context;
    UnregisterInstanceRequest request;
    google::protobuf::Empty   response;

    EXPECT_CALL(mPermHandler, UnregisterInstance).WillOnce(Return(aos::ErrorEnum::eFailed));

    const auto status = clientStub->UnregisterInstance(&context, request, &response);
    ASSERT_FALSE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
}

/***********************************************************************************************************************
 * IAMPublicService tests
 **********************************************************************************************************************/

TEST_F(IAMTest, GetAPIVersion)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPublicService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext     context;
    google::protobuf::Empty request;
    APIVersion              response;

    const auto status = clientStub->GetAPIVersion(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
    ASSERT_EQ(response.version(), 4);
}

TEST_F(IAMTest, GetNodeInfo)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPublicService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext     context;
    google::protobuf::Empty request;
    NodeInfo                response;

    const auto status = clientStub->GetNodeInfo(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
    ASSERT_EQ(response.node_id(), mServerConfig.mNodeID);
    ASSERT_EQ(response.node_type(), mServerConfig.mNodeType);
}

TEST_F(IAMTest, GetCertSucceeds)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPublicService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext context;
    GetCertRequest      request;
    request.set_type("server");

    GetCertResponse response;

    const auto status = clientStub->GetCert(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
    ASSERT_EQ(response.type(), request.type());
    ASSERT_FALSE(response.cert_url().empty());
    ASSERT_FALSE(response.key_url().empty());
}

TEST_F(IAMTest, GetCertFailsOnUnknownCertType)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPublicService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext context;
    GetCertRequest      request;
    GetCertResponse     response;

    const auto status = clientStub->GetCert(&context, request, &response);
    ASSERT_FALSE(status.ok());
}

/***********************************************************************************************************************
 * IAMPublicIdentityService tests
 **********************************************************************************************************************/

TEST_F(IAMTest, GetSystemInfoSucceeds)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPublicIdentityService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext     context;
    google::protobuf::Empty request;
    SystemInfo              response;

    EXPECT_CALL(mIdentHandler, GetSystemID)
        .WillOnce(Return(aos::RetWithError<aos::StaticString<aos::cSystemIDLen>>(cSystemID)));
    EXPECT_CALL(mIdentHandler, GetUnitModel)
        .WillOnce(Return(aos::RetWithError<aos::StaticString<aos::cUnitModelLen>>(cUnitModel)));

    const auto status = clientStub->GetSystemInfo(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
    ASSERT_EQ(response.system_id(), cSystemID);
    ASSERT_EQ(response.unit_model(), cUnitModel);
}

TEST_F(IAMTest, GetSystemInfoFailsOnSystemId)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPublicIdentityService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext     context;
    google::protobuf::Empty request;
    SystemInfo              response;

    EXPECT_CALL(mIdentHandler, GetSystemID)
        .WillOnce(Return(aos::RetWithError<aos::StaticString<aos::cSystemIDLen>>("", aos::ErrorEnum::eFailed)));
    EXPECT_CALL(mIdentHandler, GetUnitModel).Times(0);

    const auto status = clientStub->GetSystemInfo(&context, request, &response);
    ASSERT_FALSE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
}

TEST_F(IAMTest, GetSystemInfoFailsOnUnitModel)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPublicIdentityService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext     context;
    google::protobuf::Empty request;
    SystemInfo              response;

    EXPECT_CALL(mIdentHandler, GetSystemID)
        .WillOnce(Return(aos::RetWithError<aos::StaticString<aos::cSystemIDLen>>(cSystemID)));
    EXPECT_CALL(mIdentHandler, GetUnitModel)
        .WillOnce(Return(aos::RetWithError<aos::StaticString<aos::cUnitModelLen>>("", aos::ErrorEnum::eFailed)));

    const auto status = clientStub->GetSystemInfo(&context, request, &response);
    ASSERT_FALSE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
}

TEST_F(IAMTest, GetSubjectsSucceeds)
{
    aos::StaticArray<aos::StaticString<aos::cSubjectIDLen>, 10> subjects;

    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPublicIdentityService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext     context;
    google::protobuf::Empty request;
    Subjects                response;

    EXPECT_CALL(mIdentHandler, GetSubjects).WillOnce(Invoke([&subjects](auto& out) {
        out = subjects;

        return aos::ErrorEnum::eNone;
    }));

    const auto status = clientStub->GetSubjects(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
    ASSERT_EQ(response.subjects_size(), subjects.Size());
}

TEST_F(IAMTest, GetSubjectsFails)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPublicIdentityService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext     context;
    google::protobuf::Empty request;
    Subjects                response;

    EXPECT_CALL(mIdentHandler, GetSubjects).WillOnce(Return(aos::ErrorEnum::eFailed));

    const auto status = clientStub->GetSubjects(&context, request, &response);
    ASSERT_FALSE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
}

TEST_F(IAMTest, SubscribeSubjectsChanged)
{
    const std::vector<std::string> cSubjects = {"subject1", "subject2", "subject3"};

    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPublicIdentityService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext     context;
    google::protobuf::Empty request;

    const auto clientReader = clientStub->SubscribeSubjectsChanged(&context, request);
    ASSERT_NE(clientReader, nullptr) << "Failed to create client reader";

    aos::StaticArray<aos::StaticString<aos::cSubjectIDLen>, 3> newSubjects;
    for (const auto& subject : cSubjects) {
        EXPECT_TRUE(newSubjects.PushBack(subject.c_str()).IsNone());
    }

    auto* observerItf = static_cast<aos::iam::identhandler::SubjectsObserverItf*>(&mServer);
    observerItf->SubjectsChanged(newSubjects);

    Subjects response;
    while (clientReader->Read(&response)) {
        ASSERT_EQ(cSubjects.size(), response.subjects_size());
        for (size_t i = 0; i < cSubjects.size(); i++) {
            ASSERT_EQ(cSubjects[i], response.subjects(i));
        }

        break;
    }

    auto status = clientReader->Finish();
    ASSERT_TRUE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
}

/***********************************************************************************************************************
 * IAMPublicPermissionsService tests
 **********************************************************************************************************************/

TEST_F(IAMTest, GetPermissionsSucceeds)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPublicPermissionsService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext context;
    PermissionsRequest  request;
    PermissionsResponse response;

    EXPECT_CALL(mPermHandler, GetPermissions).WillOnce(Return(aos::ErrorEnum::eNone));

    const auto status = clientStub->GetPermissions(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
}

TEST_F(IAMTest, GetPermissionsFails)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOff);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMPublicPermissionsService>(mClientInfo, mServerConfig.mIAMProtectedServerURL);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext context;
    PermissionsRequest  request;
    PermissionsResponse response;

    EXPECT_CALL(mPermHandler, GetPermissions).WillOnce(Return(aos::ErrorEnum::eFailed));

    const auto status = clientStub->GetPermissions(&context, request, &response);
    ASSERT_FALSE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
}

/***********************************************************************************************************************
 * IAMProvisioningService tests
 **********************************************************************************************************************/

TEST_F(IAMTest, GetAllNodeIDsSucceeds)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    auto clientStub = CreateCustomStub<IAMProvisioningService>(
        mClientInfo, mServerConfig.mIAMProtectedServerURL, cProvisioningModeOn);
    ASSERT_NE(clientStub, nullptr) << "Failed to create client stub";

    grpc::ClientContext     context;
    google::protobuf::Empty request;
    NodesID                 response;

    const auto status = clientStub->GetAllNodeIDs(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message() << " (" << status.error_code() << ")";
}

TEST_F(IAMTest, GetCertTypesSucceeds)
{
    aos::StaticArray<aos::StaticString<aos::iam::certhandler::cCertTypeLen>, 2> registeredCertTypes;
    ASSERT_TRUE(registeredCertTypes.PushBack("client").IsNone());
    ASSERT_TRUE(registeredCertTypes.PushBack("server").IsNone());

    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOn).IsNone());

    aos::StaticArray<aos::StaticString<aos::iam::certhandler::cCertTypeLen>, 2> receivedCertTypes;

    err = client.GetCertTypes("node0", receivedCertTypes);
    ASSERT_TRUE(err.IsNone()) << err.Message();
    ASSERT_EQ(receivedCertTypes, registeredCertTypes);

    receivedCertTypes.Clear();

    err = client.GetCertTypes("node1", receivedCertTypes);
    ASSERT_TRUE(err.Is(aos::ErrorEnum::eFailed)) << err.Message();
    ASSERT_TRUE(receivedCertTypes.IsEmpty());
}

TEST_F(IAMTest, GetCertTypesFailOnUnknownNodeId)
{
    mServerConfig.mNodeID = "node10";

    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOn).IsNone());

    aos::StaticArray<aos::StaticString<aos::iam::certhandler::cCertTypeLen>, 2> receivedCertTypes;

    err = client.GetCertTypes("node0", receivedCertTypes);
    ASSERT_TRUE(err.Is(aos::ErrorEnum::eFailed)) << err.Message();
    ASSERT_TRUE(receivedCertTypes.IsEmpty());
}

TEST_F(IAMTest, SetOwnerSucceeds)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOn).IsNone());

    aos::StaticArray<aos::StaticString<aos::iam::certhandler::cCertTypeLen>, 2> receivedCertTypes;

    err = client.SetOwner("node0", "client", cPIN);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    err = client.SetOwner("node0", "client", "wrong-pin");
    ASSERT_TRUE(err.Is(aos::ErrorEnum::eFailed)) << err.Message();
}

TEST_F(IAMTest, SetOwnerFailOnUnknownNodeId)
{
    mServerConfig.mNodeID = "node10";

    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOn).IsNone());

    aos::StaticArray<aos::StaticString<aos::iam::certhandler::cCertTypeLen>, 2> receivedCertTypes;

    err = client.SetOwner("node0", "client", cPIN);
    ASSERT_TRUE(err.Is(aos::ErrorEnum::eFailed)) << err.Message();
}

TEST_F(IAMTest, ClearSucceeds)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOn).IsNone());

    aos::StaticArray<aos::StaticString<aos::iam::certhandler::cCertTypeLen>, 2> receivedCertTypes;

    err = client.Clear("node0", "client");
    ASSERT_TRUE(err.IsNone()) << err.Message();

    err = client.Clear("node0", "client");
    ASSERT_TRUE(err.Is(aos::ErrorEnum::eFailed)) << err.Message();
}

TEST_F(IAMTest, ClearFailOnInvalidNodeId)
{
    mServerConfig.mNodeID = "unknown-id";

    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOn).IsNone());

    aos::StaticArray<aos::StaticString<aos::iam::certhandler::cCertTypeLen>, 2> receivedCertTypes;

    err = client.Clear("node0", "client");
    ASSERT_TRUE(err.Is(aos::ErrorEnum::eFailed)) << err.Message();
}

TEST_F(IAMTest, EncryptDiskFailsOnEmptyCmdArgs)
{
    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOn).IsNone());

    aos::StaticArray<aos::StaticString<aos::iam::certhandler::cCertTypeLen>, 2> receivedCertTypes;

    err = client.EncryptDisk("node0", "client");
    ASSERT_TRUE(err.Is(aos::ErrorEnum::eFailed)) << err.Message();
}

TEST_F(IAMTest, EncryptDiskCmdSucceeds)
{
    RegisterPKCS11Module("diskencryption");

    mServerConfig.mDiskEncryptionCmdArgs = {"true"};

    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOn).IsNone());

    aos::StaticArray<aos::StaticString<aos::iam::certhandler::cCertTypeLen>, 2> receivedCertTypes;

    err = client.EncryptDisk("node0", "client");
    ASSERT_TRUE(err.IsNone()) << err.Message();
}

TEST_F(IAMTest, EncryptDiskCmdFails)
{
    mServerConfig.mDiskEncryptionCmdArgs = {"false"};

    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOn).IsNone());

    aos::StaticArray<aos::StaticString<aos::iam::certhandler::cCertTypeLen>, 2> receivedCertTypes;

    err = client.EncryptDisk("node0", "client");
    ASSERT_TRUE(err.Is(aos::ErrorEnum::eFailed)) << err.Message();
}

TEST_F(IAMTest, EncryptDiskFailOnUnknownNodeId)
{
    mServerConfig.mNodeID = "unknown-id";

    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOn).IsNone());

    aos::StaticArray<aos::StaticString<aos::iam::certhandler::cCertTypeLen>, 2> receivedCertTypes;

    err = client.EncryptDisk("node0", "client");
    ASSERT_TRUE(err.Is(aos::ErrorEnum::eFailed)) << err.Message();
}

TEST_F(IAMTest, FinishProvisioningSucceedsOnEmptyCmdArgs)
{
    // make sure that disk encryption command args are empty
    mServerConfig.mFinishProvisioningCmdArgs.clear();

    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOn).IsNone());

    err = client.FinishProvisioning("node0");
    ASSERT_TRUE(err.IsNone()) << err.Message();
}

TEST_F(IAMTest, FinishProvisioningCmdSucceeds)
{
    mServerConfig.mFinishProvisioningCmdArgs = {"true"};

    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOn).IsNone());

    err = client.FinishProvisioning("node0");
    ASSERT_TRUE(err.IsNone()) << err.Message();
}

TEST_F(IAMTest, FinishProvisioningCmdFails)
{
    mServerConfig.mFinishProvisioningCmdArgs = {"false"};

    auto err = mServer.Init(mServerConfig, mCertHandler, &mIdentHandler, &mPermHandler, mRemoteIAMHandler.get(),
        mCertLoader, mCryptoProvider, cProvisioningModeOn);
    ASSERT_TRUE(err.IsNone()) << err.Message();

    IAMClient client;
    ASSERT_TRUE(client.Init(mClientConfig, mCertHandler, mCertLoader, mCryptoProvider, cProvisioningModeOn).IsNone());

    err = client.FinishProvisioning("node0");
    ASSERT_TRUE(err.Is(aos::ErrorEnum::eFailed)) << err.Message();
}
