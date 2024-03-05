/*
 * Copyright (C) 2024 Renesas Electronics Corporation.
 * Copyright (C) 2024 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <Poco/Util/ServerApplication.h>

#include "logger/logger.hpp"

/**
 * Aos IAM application.
 */
class App : public Poco::Util::ServerApplication {
public:
    /**
     * Constructs new IAM application.
     */
    App() { }

protected:
    void initialize(Application& self);
    void uninitialize();
    void reinitialize(Application& self);
    int  main(const ArgVec& args);
    void defineOptions(Poco::Util::OptionSet& options);

private:
    static constexpr auto cSDNotifyReady = "READY=1";

    void HandleHelp(const std::string& name, const std::string& value);
    void HandleVersion(const std::string& name, const std::string& value);
    void HandleProvisioning(const std::string& name, const std::string& value);
    void HandleJournal(const std::string& name, const std::string& value);
    void HandleLogLevel(const std::string& name, const std::string& value);

    Logger mLogger;

    bool mStopProcessing = false;
    bool mProvisioning   = false;
};
