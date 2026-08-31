// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>

// SecureTransport has been deprecated in its entirety in favor of
// Network.framework, but that does not allow layering TLS on top of an
// arbitrary socket.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <Security/SecureTransport.h>
#pragma GCC diagnostic pop
#endif

#include "core/hle/service/ssl/ssl_backend.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

namespace {

template <typename T>
struct CFReleaser {
    T ptr;

    YUZU_NON_COPYABLE(CFReleaser);
    constexpr CFReleaser() : ptr(nullptr) {}
    constexpr CFReleaser(T ptr) : ptr(ptr) {}
    constexpr operator T() {
        return ptr;
    }
    ~CFReleaser() {
        if (ptr) {
            CFRelease(ptr);
        }
    }
};

} // namespace

namespace Service::SSL {

class SSLConnectionBackendSecureTransport final : public SSLConnectionBackend {
public:
    Result Init() {
        UNIMPLEMENTED();
        return ResultSuccess;
    }

    void SetSocket(std::shared_ptr<Network::SocketBase> in_socket) override {
        socket = std::move(in_socket);
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

    Result GetServerCerts(std::vector<std::vector<u8>>* out_certs) override {
        UNIMPLEMENTED();
        return ResultSuccess;
    }

private:
    bool got_read_eof = false;

    std::shared_ptr<Network::SocketBase> socket;
};

Result CreateSSLConnectionBackend(std::unique_ptr<SSLConnectionBackend>* out_backend) {
    auto conn = std::make_unique<SSLConnectionBackendSecureTransport>();

    R_TRY(conn->Init());

    *out_backend = std::move(conn);
    return ResultSuccess;
}

} // namespace Service::SSL
