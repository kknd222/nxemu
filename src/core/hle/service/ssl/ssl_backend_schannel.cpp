// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>

#include "yuzu_common/error.h"
#include "yuzu_common/fs/file.h"
#include "yuzu_common/hex_util.h"
#include "yuzu_common/string_util.h"

#include "core/hle/service/ssl/ssl_backend.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

namespace {

// These includes are inside the namespace to avoid a conflict on MinGW where
// the headers define an enum containing Network and Service as enumerators
// (which clash with the correspondingly named namespaces).
#define SECURITY_WIN32
#include <schnlsp.h>
#include <security.h>
#include <wincrypt.h>

std::once_flag one_time_init_flag;
bool one_time_init_success = false;

SCHANNEL_CRED schannel_cred{};
CredHandle cred_handle;

static void OneTimeInit() {
    UNIMPLEMENTED();
}

} // namespace

namespace Service::SSL {

class SSLConnectionBackendSchannel final : public SSLConnectionBackend {
public:
    Result Init() {
        std::call_once(one_time_init_flag, OneTimeInit);

        if (!one_time_init_success) {
            LOG_ERROR(
                Service_SSL,
                "Can't create SSL connection because Schannel one-time initialization failed");
            return ResultInternalError;
        }

        return ResultSuccess;
    }

    void SetSocket(std::shared_ptr<Network::SocketBase> socket_in) override {
        socket = std::move(socket_in);
    }

    Result SetHostName(const std::string& hostname_in) override {
        hostname = hostname_in;
        return ResultSuccess;
    }

    Result DoHandshake() override {
        while (1) {
            Result r;
            switch (handshake_state) {
            case HandshakeState::Initial:
                if ((r = FlushCiphertextWriteBuf()) != ResultSuccess ||
                    (r = CallInitializeSecurityContext()) != ResultSuccess) {
                    return r;
                }
                // CallInitializeSecurityContext updated `handshake_state`.
                continue;
            case HandshakeState::ContinueNeeded:
            case HandshakeState::IncompleteMessage:
                if ((r = FlushCiphertextWriteBuf()) != ResultSuccess ||
                    (r = FillCiphertextReadBuf()) != ResultSuccess) {
                    return r;
                }
                if (ciphertext_read_buf.empty()) {
                    LOG_ERROR(Service_SSL, "SSL handshake failed because server hung up");
                    return ResultInternalError;
                }
                if ((r = CallInitializeSecurityContext()) != ResultSuccess) {
                    return r;
                }
                // CallInitializeSecurityContext updated `handshake_state`.
                continue;
            case HandshakeState::DoneAfterFlush:
                if ((r = FlushCiphertextWriteBuf()) != ResultSuccess) {
                    return r;
                }
                handshake_state = HandshakeState::Connected;
                return ResultSuccess;
            case HandshakeState::Connected:
                LOG_ERROR(Service_SSL, "Called DoHandshake but we already handshook");
                return ResultInternalError;
            case HandshakeState::Error:
                return ResultInternalError;
            }
        }
    }

    Result FillCiphertextReadBuf() {
        const size_t fill_size = read_buf_fill_size ? read_buf_fill_size : 4096;
        read_buf_fill_size = 0;
        // This unnecessarily zeroes the buffer; oh well.
        const size_t offset = ciphertext_read_buf.size();
        ASSERT_OR_EXECUTE(offset + fill_size >= offset, { return ResultInternalError; });
        ciphertext_read_buf.resize(offset + fill_size, 0);
        const auto read_span = std::span(ciphertext_read_buf).subspan(offset, fill_size);
        const auto [actual, err] = socket->Recv(0, read_span);
        switch (err) {
        case Network::Errno::SUCCESS:
            ASSERT(static_cast<size_t>(actual) <= fill_size);
            ciphertext_read_buf.resize(offset + actual);
            return ResultSuccess;
        case Network::Errno::AGAIN:
            ciphertext_read_buf.resize(offset);
            return ResultWouldBlock;
        default:
            ciphertext_read_buf.resize(offset);
            LOG_ERROR(Service_SSL, "Socket recv returned Network::Errno {}", err);
            return ResultInternalError;
        }
    }

    // Returns success if the write buffer has been completely emptied.
    Result FlushCiphertextWriteBuf() {
        while (!ciphertext_write_buf.empty()) {
            const auto [actual, err] = socket->Send(ciphertext_write_buf, 0);
            switch (err) {
            case Network::Errno::SUCCESS:
                ASSERT(static_cast<size_t>(actual) <= ciphertext_write_buf.size());
                ciphertext_write_buf.erase(ciphertext_write_buf.begin(),
                                           ciphertext_write_buf.begin() + actual);
                break;
            case Network::Errno::AGAIN:
                return ResultWouldBlock;
            default:
                LOG_ERROR(Service_SSL, "Socket send returned Network::Errno {}", err);
                return ResultInternalError;
            }
        }
        return ResultSuccess;
    }

    Result CallInitializeSecurityContext() {
        UNIMPLEMENTED();
        return ResultSuccess;
    }

    Result GrabStreamSizes() {
        const SECURITY_STATUS ret =
            QueryContextAttributes(&ctxt, SECPKG_ATTR_STREAM_SIZES, &stream_sizes);
        if (ret != SEC_E_OK) {
            LOG_ERROR(Service_SSL, "QueryContextAttributes(SECPKG_ATTR_STREAM_SIZES) failed: {}",
                      Common::NativeErrorToString(ret));
            handshake_state = HandshakeState::Error;
            return ResultInternalError;
        }
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

    Result WriteAlreadyEncryptedData(size_t* out_size) {
        const Result r = FlushCiphertextWriteBuf();
        if (r != ResultSuccess) {
            return r;
        }
        // write buf is empty
        *out_size = cleartext_write_buf.size();
        cleartext_write_buf.clear();
        return ResultSuccess;
    }

    Result GetServerCerts(std::vector<std::vector<u8>>* out_certs) override {
        UNIMPLEMENTED();
        return ResultSuccess;
    }

    ~SSLConnectionBackendSchannel() {
        UNIMPLEMENTED();
    }

    enum class HandshakeState {
        // Haven't called anything yet.
        Initial,
        // `SEC_I_CONTINUE_NEEDED` was returned by
        // `InitializeSecurityContext`; must finish sending data (if any) in
        // the write buffer, then read at least one byte before calling
        // `InitializeSecurityContext` again.
        ContinueNeeded,
        // `SEC_E_INCOMPLETE_MESSAGE` was returned by
        // `InitializeSecurityContext`; hopefully the write buffer is empty;
        // must read at least one byte before calling
        // `InitializeSecurityContext` again.
        IncompleteMessage,
        // `SEC_E_OK` was returned by `InitializeSecurityContext`; must
        // finish sending data in the write buffer before having `DoHandshake`
        // report success.
        DoneAfterFlush,
        // We finished the above and are now connected.  At this point, writing
        // and reading are separate 'state machines' represented by the
        // nonemptiness of the ciphertext and cleartext read and write buffers.
        Connected,
        // Another error was returned and we shouldn't allow initialization
        // to continue.
        Error,
    } handshake_state = HandshakeState::Initial;

    CtxtHandle ctxt;
    SecPkgContext_StreamSizes stream_sizes;

    std::shared_ptr<Network::SocketBase> socket;
    std::optional<std::string> hostname;

    std::vector<u8> ciphertext_read_buf;
    std::vector<u8> ciphertext_write_buf;
    std::vector<u8> cleartext_read_buf;
    std::vector<u8> cleartext_write_buf;

    bool got_read_eof = false;
    size_t read_buf_fill_size = 0;
};

Result CreateSSLConnectionBackend(std::unique_ptr<SSLConnectionBackend>* out_backend) {
    auto conn = std::make_unique<SSLConnectionBackendSchannel>();

    R_TRY(conn->Init());

    *out_backend = std::move(conn);
    return ResultSuccess;
}

} // namespace Service::SSL
