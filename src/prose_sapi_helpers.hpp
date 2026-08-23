// Two small helpers that would otherwise come from <sphelper.h>.
//
// sphelper.h is a large inline header that has not aged well: it pulls in ATL, calls
// deprecated version APIs, and does not compile cleanly as C++17. Only two of its helpers
// are needed here and both are a few lines against the plain COM interfaces, so they are
// written out rather than dragging the whole header in.

#pragma once

#include <string>

#include <windows.h>
#include <sapi.h>
#include <comdef.h>
#include <comip.h>

namespace prose {
namespace sapi {

_COM_SMARTPTR_TYPEDEF(ISpObjectTokenCategory, __uuidof(ISpObjectTokenCategory));

// Enumerates every voice token registered with SAPI, ours included.
[[nodiscard]] inline HRESULT enum_voice_tokens(IEnumSpObjectTokens** out)
{
    if (!out) {
        return E_POINTER;
    }
    *out = nullptr;

    ISpObjectTokenCategoryPtr category;
    HRESULT hr = category.CreateInstance(CLSID_SpObjectTokenCategory);
    if (FAILED(hr) || !category) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = category->SetId(SPCAT_VOICES, FALSE);
    if (FAILED(hr)) {
        return hr;
    }
    return category->EnumTokens(nullptr, nullptr, out);
}

// A token's display name is its default string value, which is what SAPI shows in a voice
// list and what SpGetDescription returns for the current language.
[[nodiscard]] inline HRESULT token_description(ISpObjectToken* token, std::wstring& out)
{
    out.clear();
    if (!token) {
        return E_INVALIDARG;
    }
    LPWSTR value = nullptr;
    const HRESULT hr = token->GetStringValue(nullptr, &value);
    if (SUCCEEDED(hr) && value) {
        out = value;
        CoTaskMemFree(value);
    }
    return hr;
}

}  // namespace sapi
}  // namespace prose
