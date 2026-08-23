// The voice enumerator SAPI calls through the TokenEnums key.
//
// Registering one enumerator rather than three individual voice tokens keeps the registry
// footprint to a single key, and means the voice list is decided by the catalogue compiled
// into the DLL rather than by whatever an installer happened to write.

#pragma once

#include <vector>

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <comdef.h>
#include <comip.h>

#include "prose_com.hpp"
#include "prose_token.hpp"
#include "prose_voices.hpp"

namespace prose {
namespace sapi {

class __declspec(uuid("3b7a91c4-6e08-4d52-9f31-2a5c8d6e4701")) IEnumSpObjectTokensImpl :
    public IEnumSpObjectTokens
{
public:
    explicit IEnumSpObjectTokensImpl(bool initialize = true);

    IEnumSpObjectTokensImpl(const IEnumSpObjectTokensImpl&) = delete;
    IEnumSpObjectTokensImpl& operator=(const IEnumSpObjectTokensImpl&) = delete;

    STDMETHOD(Next)(ULONG celt, ISpObjectToken** pelt, ULONG* pceltFetched) override;
    STDMETHOD(Skip)(ULONG celt) override;
    STDMETHOD(Reset)() override;
    STDMETHOD(Clone)(IEnumSpObjectTokens** ppEnum) override;
    STDMETHOD(Item)(ULONG Index, ISpObjectToken** ppToken) override;
    STDMETHOD(GetCount)(ULONG* pulCount) override;

protected:
    [[nodiscard]] void* get_interface(REFIID riid) noexcept
    {
        return com::try_primary_interface<IEnumSpObjectTokens>(this, riid);
    }

private:
    _COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
    _COM_SMARTPTR_TYPEDEF(ISpObjectTokenInit, __uuidof(ISpObjectTokenInit));

    [[nodiscard]] ISpObjectTokenPtr create_token(const VoiceDesc& voice) const;

    std::size_t index_;
    std::vector<VoiceDesc> voices_;
};

}  // namespace sapi
}  // namespace prose
