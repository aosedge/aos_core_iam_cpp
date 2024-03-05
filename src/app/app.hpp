/*
 * Copyright (C) 2024 Renesas Electronics Corporation.
 * Copyright (C) 2024 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <Poco/Util/ServerApplication.h>

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
    int  main(const ArgVec& args);
    void defineOptions(Poco::Util::OptionSet& options);

private:
    void HandleHelp(const std::string& name, const std::string& value);
    void HandleVersion(const std::string& name, const std::string& value);
    void HandleProvisioning(const std::string& name, const std::string& value);
    void HandleJournal(const std::string& name, const std::string& value);
    void HandleLogLevel(const std::string& name, const std::string& value);

    bool mStopProcessing = false;
    bool mProvisioning   = false;
};
