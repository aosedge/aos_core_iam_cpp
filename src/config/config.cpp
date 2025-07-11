/*
 * Copyright (C) 2024 Renesas Electronics Corporation.
 * Copyright (C) 2024 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fstream>
#include <iostream>
#include <unordered_map>

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <aos/common/pkcs11/pkcs11.hpp>
#include <aos/common/tools/fs.hpp>
#include <utils/exception.hpp>
#include <utils/json.hpp>

#include "config.hpp"
#include "logger/logmodule.hpp"

namespace aos::iam::config {

namespace {

/***********************************************************************************************************************
 * Constants
 **********************************************************************************************************************/

constexpr auto cDefaultCPUInfoPath            = "/proc/cpuinfo";
constexpr auto cDefaultMemInfoPath            = "/proc/meminfo";
constexpr auto cDefaultProvisioningStatusPath = "/var/aos/.provisionstate";
constexpr auto cDefaultNodeIDPath             = "/etc/machine-id";

/***********************************************************************************************************************
 * Static
 **********************************************************************************************************************/

IdentifierConfig ParseIdentifier(const aos::common::utils::CaseInsensitiveObjectWrapper& object)
{
    return IdentifierConfig {object.GetValue<std::string>("plugin"), object.Get("params")};
}

ModuleConfig ParseModuleConfig(const common::utils::CaseInsensitiveObjectWrapper& object)
{
    return ModuleConfig {
        object.GetValue<std::string>("id"),
        object.GetValue<std::string>("plugin"),
        object.GetValue<std::string>("algorithm"),
        object.GetValue<int>("maxItems"),
        common::utils::GetArrayValue<std::string>(
            object, "extendedKeyUsage", [](const Poco::Dynamic::Var& value) { return value.convert<std::string>(); }),
        common::utils::GetArrayValue<std::string>(
            object, "alternativeNames", [](const Poco::Dynamic::Var& value) { return value.convert<std::string>(); }),
        object.GetValue<bool>("disabled"),
        object.GetValue<bool>("skipValidation"),
        object.GetValue<bool>("selfSigned"),
        object.Get("params"),
    };
}

PartitionInfoConfig ParsePartitionInfoConfig(const common::utils::CaseInsensitiveObjectWrapper& object)
{
    PartitionInfoConfig partitionInfoConfig {};

    partitionInfoConfig.mName = object.GetValue<std::string>("name");
    partitionInfoConfig.mPath = object.GetValue<std::string>("path");

    const auto& types = common::utils::GetArrayValue<std::string>(
        object, "types", [](const Poco::Dynamic::Var& value) { return value.convert<std::string>(); });

    for (const auto& type : types) {
        partitionInfoConfig.mTypes.push_back(type);
    }

    return partitionInfoConfig;
}

NodeInfoConfig ParseNodeInfoConfig(const common::utils::CaseInsensitiveObjectWrapper& object)
{
    NodeInfoConfig nodeInfoConfig {};

    nodeInfoConfig.mProvisioningStatePath
        = object.GetValue<std::string>("provisioningStatePath", cDefaultProvisioningStatusPath);
    nodeInfoConfig.mCPUInfoPath = object.GetValue<std::string>("cpuInfoPath", cDefaultCPUInfoPath);
    nodeInfoConfig.mMemInfoPath = object.GetValue<std::string>("memInfoPath", cDefaultMemInfoPath);
    nodeInfoConfig.mNodeIDPath  = object.GetValue<std::string>("nodeIDPath", cDefaultNodeIDPath);
    nodeInfoConfig.mNodeName    = object.GetValue<std::string>("nodeName");
    nodeInfoConfig.mNodeType    = object.GetValue<std::string>("nodeType");
    nodeInfoConfig.mOSType      = object.GetValue<std::string>("osType");
    nodeInfoConfig.mMaxDMIPS    = object.GetValue<uint64_t>("maxDMIPS");

    if (object.Has("attrs")) {
        for (const auto& [key, value] : *object.Get("attrs").extract<Poco::JSON::Object::Ptr>()) {
            nodeInfoConfig.mAttrs.emplace(key, value.extract<std::string>());
        }
    }

    if (object.Has("partitions")) {
        nodeInfoConfig.mPartitions = common::utils::GetArrayValue<PartitionInfoConfig>(
            object, "partitions", [](const Poco::Dynamic::Var& value) {
                return ParsePartitionInfoConfig(
                    common::utils::CaseInsensitiveObjectWrapper(value.extract<Poco::JSON::Object::Ptr>()));
            });
    }

    return nodeInfoConfig;
}

IAMConfig ParseIAMConfig(const common::utils::CaseInsensitiveObjectWrapper& object)
{
    IAMConfig config;

    config.mCACert                   = object.GetValue<std::string>("caCert");
    config.mCertStorage              = object.GetValue<std::string>("certStorage");
    config.mStartProvisioningCmdArgs = common::utils::GetArrayValue<std::string>(object, "startProvisioningCmdArgs",
        [](const Poco::Dynamic::Var& value) { return value.convert<std::string>(); });
    config.mDiskEncryptionCmdArgs    = common::utils::GetArrayValue<std::string>(
        object, "diskEncryptionCmdArgs", [](const Poco::Dynamic::Var& value) { return value.convert<std::string>(); });
    config.mFinishProvisioningCmdArgs = common::utils::GetArrayValue<std::string>(object, "finishProvisioningCmdArgs",
        [](const Poco::Dynamic::Var& value) { return value.convert<std::string>(); });
    config.mDeprovisionCmdArgs        = common::utils::GetArrayValue<std::string>(
        object, "deprovisionCmdArgs", [](const Poco::Dynamic::Var& value) { return value.convert<std::string>(); });

    return config;
}

IAMClientConfig ParseIAMClientConfig(const common::utils::CaseInsensitiveObjectWrapper& object)
{
    IAMClientConfig config;
    static_cast<IAMConfig&>(config) = ParseIAMConfig(object);

    config.mMainIAMPublicServerURL    = object.GetValue<std::string>("mainIAMPublicServerURL");
    config.mMainIAMProtectedServerURL = object.GetValue<std::string>("mainIAMProtectedServerURL");
    auto nodeReconnectInterval        = object.GetOptionalValue<std::string>("nodeReconnectInterval").value_or("10s");

    Error err                               = ErrorEnum::eNone;
    Tie(config.mNodeReconnectInterval, err) = common::utils::ParseDuration(nodeReconnectInterval);
    AOS_ERROR_CHECK_AND_THROW(err, "nodeReconnectInterval parse error");

    return config;
}

IAMServerConfig ParseIAMServerConfig(const common::utils::CaseInsensitiveObjectWrapper& object)
{
    IAMServerConfig config;
    static_cast<IAMConfig&>(config) = ParseIAMConfig(object);

    config.mIAMPublicServerURL    = object.GetValue<std::string>("iamPublicServerURL");
    config.mIAMProtectedServerURL = object.GetValue<std::string>("iamProtectedServerURL");

    return config;
}

DatabaseConfig ParseDatabaseConfig(
    const common::utils::CaseInsensitiveObjectWrapper& object, const std::vector<ModuleConfig>& moduleConfigs)
{
    auto migration = object.GetObject("migration");

    DatabaseConfig config {};

    config.mWorkingDir          = object.GetValue<std::string>("workingDir");
    config.mMigrationPath       = migration.GetValue<std::string>("migrationPath");
    config.mMergedMigrationPath = migration.GetValue<std::string>("mergedMigrationPath");

    for (const auto& moduleConfig : moduleConfigs) {
        common::utils::CaseInsensitiveObjectWrapper params(moduleConfig.mParams);

        std::string                   pinPath = params.GetValue<std::string>("userPinPath");
        StaticString<pkcs11::cPINLen> userPIN;

        auto err = fs::ReadFileToString(pinPath.c_str(), userPIN);
        if (!err.IsNone()) {
            continue;
        }

        config.mPathToPin[pinPath] = userPIN.CStr();
    }

    return config;
}

} // namespace

/***********************************************************************************************************************
 * Public functions
 **********************************************************************************************************************/

RetWithError<Config> ParseConfig(const std::string& filename)
{
    std::ifstream file(filename);

    if (!file.is_open()) {
        return {Config {}, ErrorEnum::eNotFound};
    }

    Config config {};

    try {
        Poco::JSON::Parser                          parser;
        auto                                        result = parser.parse(file);
        common::utils::CaseInsensitiveObjectWrapper object(result.extract<Poco::JSON::Object::Ptr>());

        config.mCertModules
            = common::utils::GetArrayValue<ModuleConfig>(object, "certModules", [](const Poco::Dynamic::Var& value) {
                  return ParseModuleConfig(
                      common::utils::CaseInsensitiveObjectWrapper(value.extract<Poco::JSON::Object::Ptr>()));
              });

        config.mNodeInfo                 = ParseNodeInfoConfig(object.GetObject("nodeInfo"));
        config.mIAMClient                = ParseIAMClientConfig(object);
        config.mIAMServer                = ParseIAMServerConfig(object);
        config.mDatabase                 = ParseDatabaseConfig(object, config.mCertModules);
        config.mEnablePermissionsHandler = object.GetValue<bool>("enablePermissionsHandler");

        if (object.Has("identifier")) {
            config.mIdentifier = ParseIdentifier(object.GetObject("identifier"));
        }

    } catch (const std::exception& e) {
        return {{}, common::utils::ToAosError(e, ErrorEnum::eInvalidArgument)};
    }

    return config;
}

RetWithError<PKCS11ModuleParams> ParsePKCS11ModuleParams(Poco::Dynamic::Var params)
{
    PKCS11ModuleParams moduleParams;

    try {
        common::utils::CaseInsensitiveObjectWrapper object(params.extract<Poco::JSON::Object::Ptr>());

        moduleParams.mLibrary         = object.GetValue<std::string>("library");
        moduleParams.mSlotID          = object.GetOptionalValue<uint32_t>("slotID");
        moduleParams.mSlotIndex       = object.GetOptionalValue<int>("slotIndex");
        moduleParams.mTokenLabel      = object.GetValue<std::string>("tokenLabel");
        moduleParams.mUserPINPath     = object.GetValue<std::string>("userPinPath");
        moduleParams.mModulePathInURL = object.GetValue<bool>("modulePathInUrl");
        moduleParams.mUID             = object.GetOptionalValue<uint32_t>("uid").value_or(0);
        moduleParams.mGID             = object.GetOptionalValue<uint32_t>("gid").value_or(0);

    } catch (const std::exception& e) {
        return {{}, common::utils::ToAosError(e, ErrorEnum::eInvalidArgument)};
    }

    return moduleParams;
}

RetWithError<VISIdentifierModuleParams> ParseVISIdentifierModuleParams(Poco::Dynamic::Var params)
{
    VISIdentifierModuleParams moduleParams;

    try {
        common::utils::CaseInsensitiveObjectWrapper object(params.extract<Poco::JSON::Object::Ptr>());

        moduleParams.mVISServer  = object.GetValue<std::string>("visServer");
        moduleParams.mCaCertFile = object.GetValue<std::string>("caCertFile");

        Error err;

        Tie(moduleParams.mWebSocketTimeout, err)
            = common::utils::ParseDuration(object.GetValue<std::string>("webSocketTimeout", "120s"));
        AOS_ERROR_CHECK_AND_THROW(err, "failed to parse webSocketTimeout");
    } catch (const std::exception& e) {
        return {{}, common::utils::ToAosError(e, ErrorEnum::eInvalidArgument)};
    }

    return moduleParams;
}

RetWithError<FileIdentifierModuleParams> ParseFileIdentifierModuleParams(Poco::Dynamic::Var params)
{
    FileIdentifierModuleParams moduleParams;

    try {
        common::utils::CaseInsensitiveObjectWrapper object(params.extract<Poco::JSON::Object::Ptr>());

        moduleParams.mSystemIDPath  = object.GetValue<std::string>("systemIDPath");
        moduleParams.mUnitModelPath = object.GetValue<std::string>("unitModelPath");
        moduleParams.mSubjectsPath  = object.GetValue<std::string>("subjectsPath");
    } catch (const std::exception& e) {
        return {{}, common::utils::ToAosError(e, ErrorEnum::eInvalidArgument)};
    }

    return moduleParams;
}

} // namespace aos::iam::config
