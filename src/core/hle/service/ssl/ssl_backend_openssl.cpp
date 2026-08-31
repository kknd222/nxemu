// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>

#include "yuzu_common/fs/file.h"
#include "yuzu_common/hex_util.h"
#include "yuzu_common/string_util.h"

#include "core/hle/service/ssl/ssl_backend.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

using namespace Common::FS;

namespace Service::SSL {

// Import OpenSSL's `SSL` type into the namespace.  This is needed because the
// namespace is also named `SSL`.

namespace {

std::once_flag one_time_init_flag;
bool one_time_init_success = false;

IOFile key_log_file; // only open if SSLKEYLOGFILE set in environment

Result CheckOpenSSLErrors();
void OneTimeInit();
void OneTimeInitLogFile();
bool OneTimeInitBIO();

} // namespace

class SSLConnectionBackendOpenSSL final : public SSLConnectionBackend {
public:
    Result Init() {
        UNIMPLEMENTED();
        return ResultSuccess;
    }

    void SetSocket(std::shared_ptr<Network::SocketBase> socket_in) override {
        socket = std::move(socket_in);
    }

    Result SetHostName(const std::string& hostname) override {
        UNIMPLEMENTED();
        return ResultSuccess;
    }

    Result DoHandshake() override {
        UNIMPLEMENTED();
        return ResultSuccess;
    }

    Result Read(size_t* out_size, std::span<u8> data) override {
        UNIMPLEMENTED();
        return ResultSuccess;
    }

    Result Write(size_t* out_size, std::span<const u8> data) override {
        UNIMPLEMENTED();
        return ResultSuccess;
    }

    Result HandleReturn(const char* what, size_t* actual, int ret) {
        UNIMPLEMENTED();
        return ResultSuccess;
    }

    Result GetServerCerts(std::vector<std::vector<u8>>* out_certs) override {
        UNIMPLEMENTED();
        return ResultSuccess;
    }

    ~SSLConnectionBackendOpenSSL() {
        UNIMPLEMENTED();
    }

    bool got_read_eof = false;

    std::shared_ptr<Network::SocketBase> socket;
};

Result CreateSSLConnectionBackend(std::unique_ptr<SSLConnectionBackend>* out_backend) {
    auto conn = std::make_unique<SSLConnectionBackendOpenSSL>();

    R_TRY(conn->Init());

    *out_backend = std::move(conn);
    return ResultSuccess;
}

namespace {

Result CheckOpenSSLErrors() {
    UNIMPLEMENTED();
    return ResultSuccess;
}

void OneTimeInit() {
    UNIMPLEMENTED();
}

void OneTimeInitLogFile() {
    UNIMPLEMENTED();
}

bool OneTimeInitBIO() {
    UNIMPLEMENTED();
    return false;
}

} // namespace

} // namespace Service::SSL
