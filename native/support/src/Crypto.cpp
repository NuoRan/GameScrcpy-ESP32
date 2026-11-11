#include "gs/support/Crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdlib>
#include <limits>

namespace gs::platform {

namespace {

void require(NTSTATUS status) noexcept
{
    if (!BCRYPT_SUCCESS(status)) {
        std::abort();
    }
}

ULONG length(std::span<const std::uint8_t> bytes) noexcept
{
    if (bytes.size() > std::numeric_limits<ULONG>::max()) {
        std::abort();
    }
    return static_cast<ULONG>(bytes.size());
}

}

Sha256Digest sha256(std::span<const std::uint8_t> data)
{
    Sha256Digest digest{};
    require(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()), length(data),
                       digest.data(), static_cast<ULONG>(digest.size())));
    return digest;
}

void randomBytes(std::span<std::uint8_t> out)
{
    if (!out.empty()) {
        require(BCryptGenRandom(nullptr, out.data(), length(out), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    }
}

std::string toHex(std::span<const std::uint8_t> bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        text.push_back(kDigits[byte >> 4U]);
        text.push_back(kDigits[byte & 0x0FU]);
    }
    return text;
}

}
