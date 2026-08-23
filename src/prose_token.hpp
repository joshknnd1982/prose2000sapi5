// A SAPI 5 voice token backed by one entry from the voice catalogue.
//
// The token is built in memory rather than read back out of the registry, so installing
// the voices costs nothing beyond the single TokenEnums key that points SAPI at our
// enumerator.

#pragma once

#include <map>
#include <string>

#include "prose_datakey.hpp"
#include "prose_voices.hpp"

namespace prose {
namespace sapi {

// Attribute names carrying our own data through the token. SAPI hands the whole token to
// the engine, so SetObjectToken can recover the exact voice without parsing display names.
inline constexpr wchar_t kAttrVoiceIndex[] = L"ProseVoiceIndex";
inline constexpr wchar_t kAttrTokenName[] = L"ProseTokenName";

class voice_token : public ISpDataKeyImpl
{
public:
    explicit voice_token(const VoiceDesc& voice);

    STDMETHOD(OpenKey)(LPCWSTR pszSubKeyName, ISpDataKey** ppSubKey) override;
    STDMETHOD(EnumKeys)(ULONG Index, LPWSTR* ppszSubKeyName) override;

private:
    using attribute_map = std::map<std::wstring, std::wstring, str_less>;

    attribute_map attributes_;
};

}  // namespace sapi
}  // namespace prose
