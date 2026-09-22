#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mbedtls/base64.h"
#include "sodium.h"

#ifndef LUDANT_OTA_PUBLIC_KEY_DER_HEX
#ifdef CONFIG_LUDANT_OTA_PUBLIC_KEY_DER_HEX
#define LUDANT_OTA_PUBLIC_KEY_DER_HEX CONFIG_LUDANT_OTA_PUBLIC_KEY_DER_HEX
#else
#define LUDANT_OTA_PUBLIC_KEY_DER_HEX ""
#endif
#endif

namespace ludant::ota_auth {

inline int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

inline bool decodeHex(const char* input, uint8_t* output, size_t output_capacity, size_t& output_length) {
    if (input == nullptr) return false;
    const size_t input_length = std::strlen(input);
    if (input_length == 0 || (input_length & 1U) != 0 || input_length / 2 > output_capacity) return false;
    for (size_t index = 0; index < input_length / 2; ++index) {
        const int high = hexValue(input[index * 2]);
        const int low = hexValue(input[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[index] = static_cast<uint8_t>((high << 4) | low);
    }
    output_length = input_length / 2;
    return true;
}

inline bool decodeEd25519PublicKey(uint8_t output[crypto_sign_ed25519_PUBLICKEYBYTES]) {
    // The release tooling stores the Ed25519 key as a DER SubjectPublicKeyInfo
    // value. libsodium verifies with the raw 32-byte public-key representation.
    uint8_t public_key_der[96]{};
    size_t public_key_length = 0;
    if (!decodeHex(LUDANT_OTA_PUBLIC_KEY_DER_HEX, public_key_der, sizeof(public_key_der), public_key_length) ||
        public_key_length < crypto_sign_ed25519_PUBLICKEYBYTES) {
        return false;
    }

    std::memcpy(output,
                public_key_der + public_key_length - crypto_sign_ed25519_PUBLICKEYBYTES,
                crypto_sign_ed25519_PUBLICKEYBYTES);
    return true;
}

inline bool verifyDigestSignature(const uint8_t digest[32], const char* signature_base64) {
    if (digest == nullptr || signature_base64 == nullptr || LUDANT_OTA_PUBLIC_KEY_DER_HEX[0] == '\0') return false;

    if (sodium_init() < 0) return false;

    uint8_t public_key[crypto_sign_ed25519_PUBLICKEYBYTES]{};
    if (!decodeEd25519PublicKey(public_key)) return false;

    uint8_t signature[crypto_sign_ed25519_BYTES]{};
    size_t signature_length = 0;
    const int decode_result = mbedtls_base64_decode(
        signature, sizeof(signature), &signature_length,
        reinterpret_cast<const uint8_t*>(signature_base64), std::strlen(signature_base64));
    if (decode_result != 0 || signature_length != crypto_sign_ed25519_BYTES) return false;

    return crypto_sign_ed25519_verify_detached(
               signature, digest, 32, public_key) == 0;
}

} // namespace ludant::ota_auth
