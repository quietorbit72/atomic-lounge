// SPDX-License-Identifier: AGPL-3.0-only
// Atomic Lounge plugin: WebView2 start-up probe and per-host data folder (see WebView2Probe.h).
#include "WebView2Probe.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <memory>

namespace mfm1 {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

juce::String WebView2Probe::hostName()
{
    wchar_t buf[MAX_PATH] {};
    const DWORD n = GetModuleFileNameW (nullptr, buf, MAX_PATH);
    auto name = n > 0 ? juce::File (juce::String (buf)).getFileNameWithoutExtension() : juce::String();
    name = juce::File::createLegalFileName (name).trim();
    return name.isNotEmpty() ? name : juce::String ("host");
}

juce::File WebView2Probe::dataFolder()
{
    return juce::File::getSpecialLocation (juce::File::windowsLocalAppData)
        .getChildFile ("Atomic Lounge").getChildFile ("WebView2").getChildFile (hostName());
}

juce::String WebView2Probe::runtimeVersion()
{
    LPWSTR v = nullptr;
    juce::String s;
    if (SUCCEEDED (GetAvailableCoreWebView2BrowserVersionString (nullptr, &v)) && v != nullptr) s = juce::String (v);
    if (v != nullptr) CoTaskMemFree (v);
    return s;
}

juce::String WebView2Probe::describe (long hr)
{
    juce::String text;
    wchar_t* msg = nullptr;
    if (FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                        (DWORD) hr, 0, (LPWSTR) &msg, 0, nullptr) > 0 && msg != nullptr)
        text = juce::String (msg).trim();
    if (msg != nullptr) LocalFree (msg);
    return "0x" + juce::String::toHexString ((juce::uint32) hr).paddedLeft ('0', 8).toUpperCase() + (text.isNotEmpty() ? " (" + text + ")" : juce::String());
}

namespace {
struct Job
{
    WebView2Probe::Done done;
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> ctrl;
    bool finished = false;

    void finish (bool ok, const juce::String& step, long hr)
    {
        if (finished) return;
        finished = true;
        if (ctrl) ctrl->Close();
        ctrl.Reset();
        env.Reset();
        if (hwnd != nullptr) DestroyWindow (hwnd);
        hwnd = nullptr;
        auto d = std::move (done);
        if (d) d (ok, step, hr);
    }
};
}

void WebView2Probe::run (const juce::File& folder, Done done, int timeoutMs)
{
    auto job = std::make_shared<Job>();
    job->done = std::move (done);
    if (runtimeVersion().isEmpty())
    {
        LPWSTR v = nullptr;
        const HRESULT hr = GetAvailableCoreWebView2BrowserVersionString (nullptr, &v);
        if (v != nullptr) CoTaskMemFree (v);
        job->finish (false, "WebView2 runtime not found", FAILED (hr) ? hr : HRESULT_FROM_WIN32 (ERROR_FILE_NOT_FOUND));
        return;
    }
    if (! folder.createDirectory())
    {
        job->finish (false, "cannot create the data folder " + folder.getFullPathName(), HRESULT_FROM_WIN32 (GetLastError()));
        return;
    }
    // a hidden top-level window for the probe's controller (never shown)
    job->hwnd = CreateWindowExW (WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, GetModuleHandleW (nullptr), nullptr);
    if (job->hwnd == nullptr)
    {
        job->finish (false, "probe window", HRESULT_FROM_WIN32 (GetLastError()));
        return;
    }
    auto opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();   // the options JUCE uses too
    const auto path = folder.getFullPathName();
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions (nullptr, path.toWideCharPointer(), opts.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> (
            [job] (HRESULT r, ICoreWebView2Environment* env) -> HRESULT
            {
                if (job->finished) return S_OK;
                if (FAILED (r) || env == nullptr) { job->finish (false, "environment", FAILED (r) ? r : E_FAIL); return S_OK; }
                job->env = env;
                const HRESULT c = env->CreateCoreWebView2Controller (job->hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> (
                        [job] (HRESULT r2, ICoreWebView2Controller* ctrl) -> HRESULT
                        {
                            if (job->finished) return S_OK;
                            if (FAILED (r2) || ctrl == nullptr) { job->finish (false, "controller", FAILED (r2) ? r2 : E_FAIL); return S_OK; }
                            job->ctrl = ctrl;
                            job->finish (true, {}, S_OK);
                            return S_OK;
                        }).Get());
                if (FAILED (c)) job->finish (false, "controller", c);
                return S_OK;
            }).Get());
    if (FAILED (hr)) { job->finish (false, "environment", hr); return; }
    juce::Timer::callAfterDelay (timeoutMs, [job]
    {
        job->finish (false, "no answer from WebView2 (timed out)", HRESULT_FROM_WIN32 (ERROR_TIMEOUT));
    });
}

}
