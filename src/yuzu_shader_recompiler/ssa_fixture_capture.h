// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <memory>
namespace Shader::IR {struct Program;}
namespace Shader::SsaFixture {
struct CaptureState;
class Scope {
public:
    std::uint64_t key{},unique_hash{};
    Scope* previous{};
    std::unique_ptr<CaptureState> state;
    Scope(std::uint64_t key_,std::uint64_t unique_hash_) noexcept;
    ~Scope();
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};
void CapturePre(IR::Program& program) noexcept;
void CapturePost(IR::Program& program) noexcept;
}
