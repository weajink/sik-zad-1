#include <gtest/gtest.h>
#include <signal.h>

#include "kayles_signal.h"

namespace {

    // Reset the flag before each test so ordering doesn't matter.
    class SignalFixture : public ::testing::Test {
       protected:
        void SetUp() override {
            kayles::sig::shutdown_requested = 0;
        }
        void TearDown() override {
            kayles::sig::shutdown_requested = 0;
        }
    };

    TEST_F(SignalFixture, HandlerSetsFlagOnSigint) {
        kayles::sig::install();
        ASSERT_EQ(kayles::sig::shutdown_requested, 0);
        raise(SIGINT);
        EXPECT_EQ(kayles::sig::shutdown_requested, 1);
    }

    TEST_F(SignalFixture, HandlerSetsFlagOnSigterm) {
        kayles::sig::install();
        ASSERT_EQ(kayles::sig::shutdown_requested, 0);
        raise(SIGTERM);
        EXPECT_EQ(kayles::sig::shutdown_requested, 1);
    }

    TEST_F(SignalFixture, HandlerInstallationDoesNotTerminate) {
        // Installing must not segfault or otherwise crash the test binary.
        kayles::sig::install();
        SUCCEED();
    }

    TEST_F(SignalFixture, InstallIsIdempotent) {
        kayles::sig::install();
        kayles::sig::install();
        kayles::sig::install();
        raise(SIGINT);
        EXPECT_EQ(kayles::sig::shutdown_requested, 1);
    }

    TEST_F(SignalFixture, SigactionWithoutSaRestartSoBlockingCallsEintr) {
        // Verify the installed handler uses sa_flags=0 (no SA_RESTART). If
        // SA_RESTART were set, blocking syscalls like recvfrom would auto-retry
        // and the server loop could never observe the shutdown flag.
        kayles::sig::install();
        struct sigaction sa {};
        ASSERT_EQ(sigaction(SIGINT, nullptr, &sa), 0);
        EXPECT_EQ(sa.sa_flags & SA_RESTART, 0);
    }

    TEST_F(SignalFixture, HandlerInstalledForSigterm) {
        kayles::sig::install();
        struct sigaction sa {};
        ASSERT_EQ(sigaction(SIGTERM, nullptr, &sa), 0);
        // Handler pointer must match kayles::sig::handler, not SIG_DFL/SIG_IGN.
        EXPECT_EQ(sa.sa_handler, &kayles::sig::handler);
    }

    TEST_F(SignalFixture, HandlerInstalledForSigint) {
        kayles::sig::install();
        struct sigaction sa {};
        ASSERT_EQ(sigaction(SIGINT, nullptr, &sa), 0);
        EXPECT_EQ(sa.sa_handler, &kayles::sig::handler);
    }

}  // namespace
