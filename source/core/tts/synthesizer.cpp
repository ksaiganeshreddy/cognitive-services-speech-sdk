//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// synthesizer.cpp: Implementation definitions for CSpxSynthesizer C++ class
//

#include "stdafx.h"
#include "synthesizer.h"
#include <future>
#include "handle_table.h"
#include "site_helpers.h"
#include "service_helpers.h"
#include "create_object_helpers.h"
#include "synthesis_helper.h"
#include "property_id_2_name_map.h"
#include "guid_utils.h"
#include "file_logger.h"

namespace Microsoft {
namespace CognitiveServices {
namespace Speech {
namespace Impl {


CSpxSynthesizer::CSpxSynthesizer() :
    m_fEnabled(true)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);
}

CSpxSynthesizer::~CSpxSynthesizer()
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);

    Term();
}

void CSpxSynthesizer::Init()
{
    // NOTE: Due to current ownership model, and our late-into-the-cycle changes for SpeechConfig objects
    // the CSpxSynthesizer is sited to the CSpxApiFactory. This ApiFactory is not held by the
    // dev user at or above the CAPI. Thus ... we must hold it alive in order for the properties to be
    // obtainable via the standard ISpxNamedProperties mechanisms... It will be released in ::Term()
    m_siteKeepAlive = GetSite();
    
    CheckLogFilename();

    EnsureTtsEngineAdapter();
}

void CSpxSynthesizer::Term()
{
    ReleaseTtsEngineAdapter();
    m_audioOutput->Close();
    SpxTermAndClear(m_siteKeepAlive);
}

bool CSpxSynthesizer::IsEnabled()
{
    return m_fEnabled;
}

void CSpxSynthesizer::Enable()
{
    m_fEnabled = true;
}

void CSpxSynthesizer::Disable()
{
    m_fEnabled = false;
}

void CSpxSynthesizer::SetOutput(std::shared_ptr<ISpxAudioOutput> output)
{
    m_audioOutput = output;
    m_ttsAdapter->SetOutput(output);
}

std::shared_ptr<ISpxSynthesisResult> CSpxSynthesizer::Speak(const std::string& text, bool isSsml)
{
    // Request ID is per speak, different events from same speak will share one request ID
    auto requestId = PAL::CreateGuidWithoutDashes();

    // Push request into queue
    PushRequestIntoQueue(requestId);

    // Wait until current request to be in front of the queue
    WaitUntilRequestInFrontOfQueue(requestId);

    // Fire SynthesisStarted event
    auto synthesisStartedResult = CreateResult(requestId, ResultReason::SynthesizingAudioStarted, nullptr, 0);
    FireResultEvent(synthesisStartedResult);

    // Speak
    auto synthesisDoneResult = m_ttsAdapter->Speak(text, isSsml, requestId);

    // Wait for audio output to be done
    m_audioOutput->WaitUntilDone();

    // Set events
    auto events = this->QueryInterfaceInternal<ISpxSynthesizerEvents>();
    auto resultInit = SpxQueryInterface<ISpxSynthesisResultInit>(synthesisDoneResult);
    resultInit->SetEvents(events);

    // Fire SynthesisCompleted or SynthesisCanceled (depending on the result reason) event
    FireResultEvent(synthesisDoneResult);

    // Pop processed request from queue
    PopRequestFromQueue();

    return synthesisDoneResult;
}

CSpxAsyncOp<std::shared_ptr<ISpxSynthesisResult>> CSpxSynthesizer::SpeakAsync(const std::string& text, bool isSsml)
{
    auto keepAlive = SpxSharedPtrFromThis<ISpxSynthesizer>(this);
    std::shared_future<std::shared_ptr<ISpxSynthesisResult>> waitForCompletion(std::async(std::launch::async, [this, keepAlive, text, isSsml]() {
        // Speak
        return Speak(text, isSsml);
    }));

    return CSpxAsyncOp<std::shared_ptr<ISpxSynthesisResult>>(waitForCompletion, AOS_Started);
}

std::shared_ptr<ISpxSynthesisResult> CSpxSynthesizer::StartSpeaking(const std::string& text, bool isSsml)
{
    // Request ID is per speak, different events from same speak will share one request ID
    auto requestId = PAL::CreateGuidWithoutDashes();

    // Push request into queue
    PushRequestIntoQueue(requestId);

    // Wait until current request to be in front of the queue
    WaitUntilRequestInFrontOfQueue(requestId);

    // Fire SynthesisStarted event
    auto synthesisStartedResult = CreateResult(requestId, ResultReason::SynthesizingAudioStarted, nullptr, 0);
    FireResultEvent(synthesisStartedResult);

    auto keepAlive = SpxSharedPtrFromThis<ISpxSynthesizer>(this);
    std::shared_future<std::shared_ptr<ISpxSynthesisResult>> waitForCompletion(std::async(std::launch::async, [this, keepAlive, requestId, text, isSsml]() {
        // Speak
        auto synthesisDoneResult = m_ttsAdapter->Speak(text, isSsml, requestId);

        // Wait for audio output to be done
        m_audioOutput->WaitUntilDone();

        // Set events
        auto events = this->QueryInterfaceInternal<ISpxSynthesizerEvents>();
        auto resultInit = SpxQueryInterface<ISpxSynthesisResultInit>(synthesisDoneResult);
        resultInit->SetEvents(events);

        // Fire SynthesisCompleted or SynthesisCanceled (depending on the result reason) event
        FireResultEvent(synthesisDoneResult);

        // Pop processed request from queue
        PopRequestFromQueue();

        return synthesisDoneResult;
    }));

    auto asyncop = CSpxAsyncOp<std::shared_ptr<ISpxSynthesisResult>>(waitForCompletion, AOS_Started);
    auto futureResult = std::make_shared<CSpxAsyncOp<std::shared_ptr<ISpxSynthesisResult>>>(std::move(asyncop));
    auto resultInit = SpxQueryInterface<ISpxSynthesisResultInit>(synthesisStartedResult);
    resultInit->SetFutureResult(futureResult); // Assign asyncop to result to be brought out, otherwise it will be synchronous

    return synthesisStartedResult;
}

CSpxAsyncOp<std::shared_ptr<ISpxSynthesisResult>> CSpxSynthesizer::StartSpeakingAsync(const std::string& text, bool isSsml)
{
    auto keepAlive = SpxSharedPtrFromThis<ISpxSynthesizer>(this);
    std::shared_future<std::shared_ptr<ISpxSynthesisResult>> waitForSpeakStart(std::async(std::launch::async, [this, keepAlive, text, isSsml]() {
        // Start speaking
        return StartSpeaking(text, isSsml);
    }));

    return CSpxAsyncOp<std::shared_ptr<ISpxSynthesisResult>>(waitForSpeakStart, AOS_Started);
}

void CSpxSynthesizer::Close()
{
    m_audioOutput->Close();
}

void CSpxSynthesizer::ConnectSynthesisStartedCallback(void* object, SynthesisCallbackFunction_Type callback)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);

    std::unique_lock<std::mutex> lock(m_synthesisStartedMutex);

    auto iterator = SynthesisStarted.begin();
    while (iterator != SynthesisStarted.end() && iterator->first != object)
    {
        iterator++;
    }

    if (iterator != SynthesisStarted.end())
    {
        iterator->second->Connect(callback);
    }
    else
    {
        auto eventSignal = std::make_shared<EventSignal<std::shared_ptr<ISpxSynthesisEventArgs>>>();
        eventSignal->Connect(callback);
        SynthesisStarted.emplace_front(object, eventSignal);
    }
}

void CSpxSynthesizer::ConnectSynthesizingCallback(void* object, SynthesisCallbackFunction_Type callback)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);

    std::unique_lock<std::mutex> lock(m_synthesizingMutex);

    auto iterator = Synthesizing.begin();
    while (iterator != Synthesizing.end() && iterator->first != object)
    {
        iterator++;
    }

    if (iterator != Synthesizing.end())
    {
        iterator->second->Connect(callback);
    }
    else
    {
        auto eventSignal = std::make_shared<EventSignal<std::shared_ptr<ISpxSynthesisEventArgs>>>();
        eventSignal->Connect(callback);
        Synthesizing.emplace_front(object, eventSignal);
    }
}

void CSpxSynthesizer::ConnectSynthesisCompletedCallback(void* object, SynthesisCallbackFunction_Type callback)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);

    std::unique_lock<std::mutex> lock(m_synthesisCompletedMutex);

    auto iterator = SynthesisCompleted.begin();
    while (iterator != SynthesisCompleted.end() && iterator->first != object)
    {
        iterator++;
    }

    if (iterator != SynthesisCompleted.end())
    {
        iterator->second->Connect(callback);
    }
    else
    {
        auto eventSignal = std::make_shared<EventSignal<std::shared_ptr<ISpxSynthesisEventArgs>>>();
        eventSignal->Connect(callback);
        SynthesisCompleted.emplace_front(object, eventSignal);
    }
}

void CSpxSynthesizer::ConnectSynthesisCanceledCallback(void* object, SynthesisCallbackFunction_Type callback)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);

    std::unique_lock<std::mutex> lock(m_synthesisCanceledMutex);

    auto iterator = SynthesisCanceled.begin();
    while (iterator != SynthesisCanceled.end() && iterator->first != object)
    {
        iterator++;
    }

    if (iterator != SynthesisCanceled.end())
    {
        iterator->second->Connect(callback);
    }
    else
    {
        auto eventSignal = std::make_shared<EventSignal<std::shared_ptr<ISpxSynthesisEventArgs>>>();
        eventSignal->Connect(callback);
        SynthesisCanceled.emplace_front(object, eventSignal);
    }
}

void CSpxSynthesizer::DisconnectSynthesisStartedCallback(void* object, SynthesisCallbackFunction_Type callback)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);

    std::unique_lock<std::mutex> lock(m_synthesisStartedMutex);

    auto iterator = SynthesisStarted.begin();
    while (iterator != SynthesisStarted.end() && iterator->first != object)
    {
        iterator++;
    }

    if (iterator != SynthesisStarted.end())
    {
        if (callback != nullptr)
        {
            iterator->second->Disconnect(callback);
        }
        else
        {
            iterator->second->DisconnectAll();
        }

        if (!iterator->second->IsConnected())
        {
            SynthesisStarted.remove(*iterator);
        }
    }
}

void CSpxSynthesizer::DisconnectSynthesizingCallback(void* object, SynthesisCallbackFunction_Type callback)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);

    std::unique_lock<std::mutex> lock(m_synthesizingMutex);

    auto iterator = Synthesizing.begin();
    while (iterator != Synthesizing.end() && iterator->first != object)
    {
        iterator++;
    }

    if (iterator != Synthesizing.end())
    {
        if (callback != nullptr)
        {
            iterator->second->Disconnect(callback);
        }
        else
        {
            iterator->second->DisconnectAll();
        }

        if (!iterator->second->IsConnected())
        {
            Synthesizing.remove(*iterator);
        }
    }
}

void CSpxSynthesizer::DisconnectSynthesisCompletedCallback(void* object, SynthesisCallbackFunction_Type callback)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);

    std::unique_lock<std::mutex> lock(m_synthesisCompletedMutex);

    auto iterator = SynthesisCompleted.begin();
    while (iterator != SynthesisCompleted.end() && iterator->first != object)
    {
        iterator++;
    }

    if (iterator != SynthesisCompleted.end())
    {
        if (callback != nullptr)
        {
            iterator->second->Disconnect(callback);
        }
        else
        {
            iterator->second->DisconnectAll();
        }

        if (!iterator->second->IsConnected())
        {
            SynthesisCompleted.remove(*iterator);
        }
    }
}

void CSpxSynthesizer::DisconnectSynthesisCanceledCallback(void* object, SynthesisCallbackFunction_Type callback)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);

    std::unique_lock<std::mutex> lock(m_synthesisCanceledMutex);

    auto iterator = SynthesisCanceled.begin();
    while (iterator != SynthesisCanceled.end() && iterator->first != object)
    {
        iterator++;
    }

    if (iterator != SynthesisCanceled.end())
    {
        if (callback != nullptr)
        {
            iterator->second->Disconnect(callback);
        }
        else
        {
            iterator->second->DisconnectAll();
        }

        if (!iterator->second->IsConnected())
        {
            SynthesisCanceled.remove(*iterator);
        }
    }
}

void CSpxSynthesizer::FireSynthesisStarted(std::shared_ptr<ISpxSynthesisResult> result)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);
    FireSynthesisEvent(SynthesisStarted, result);
}

void CSpxSynthesizer::FireSynthesizing(std::shared_ptr<ISpxSynthesisResult> result)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);
    FireSynthesisEvent(Synthesizing, result);
}

void CSpxSynthesizer::FireSynthesisCompleted(std::shared_ptr<ISpxSynthesisResult> result)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);
    FireSynthesisEvent(SynthesisCompleted, result);
}

void CSpxSynthesizer::FireSynthesisCanceled(std::shared_ptr<ISpxSynthesisResult> result)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);
    FireSynthesisEvent(SynthesisCanceled, result);
}

void CSpxSynthesizer::FireWordBoundary(uint64_t audioOffset, uint32_t textOffset, uint32_t wordLength)
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);
    auto wordBoundaryEvent = SpxCreateObjectWithSite<ISpxWordBoundaryEventArgs>("CSpxWordBoundaryEventArgs", SpxSiteFromThis(this));
    auto argsInit = SpxQueryInterface<ISpxWordBoundaryEventArgsInit>(wordBoundaryEvent);
    argsInit->Init(audioOffset, textOffset, wordLength);
    WordBoundary.Signal(wordBoundaryEvent);
}

uint32_t CSpxSynthesizer::Write(ISpxTtsEngineAdapter* adapter, const std::wstring& requestId, uint8_t* buffer, uint32_t size)
{
    UNUSED(adapter);

    // Fire Synthesizing event
    auto result = CreateResult(requestId, ResultReason::SynthesizingAudio, buffer, size);
    FireResultEvent(result);

    // Write audio data to output
    return m_audioOutput->Write(buffer, size);
}

std::shared_ptr<ISpxNamedProperties> CSpxSynthesizer::GetParentProperties() const
{
    return SpxQueryService<ISpxNamedProperties>(GetSite());
}

void CSpxSynthesizer::CheckLogFilename()
{
    auto filename = GetStringValue(GetPropertyName(PropertyId::Speech_LogFilename), "");
    if (!filename.empty())
    {
        FileLogger::Instance().SetFilename(std::move(filename));
    }
}

void CSpxSynthesizer::PushRequestIntoQueue(const std::wstring requestId)
{
    std::unique_lock<std::mutex> lock(m_queueOperationMutex);
    m_requestQueue.emplace(requestId);
    lock.unlock();
}

void CSpxSynthesizer::WaitUntilRequestInFrontOfQueue(const std::wstring& requestId)
{
    std::unique_lock<std::mutex> lock(m_requestWaitingMutex);

#ifdef _DEBUG
    while (!m_cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
        std::unique_lock<std::mutex> lock(m_queueOperationMutex);
        return m_requestQueue.front() == requestId; }))
    {
        SPX_DBG_TRACE_VERBOSE("%s: waiting for processing speak request ...", __FUNCTION__);
    }
#else
    m_cv.wait(lock, [&] {
        std::unique_lock<std::mutex> lock(m_queueOperationMutex);
        return m_requestQueue.front() == requestId; });
#endif
}

void CSpxSynthesizer::PopRequestFromQueue()
{
    std::unique_lock<std::mutex> lock(m_queueOperationMutex);
    m_requestQueue.pop();
    m_cv.notify_all();
}

std::shared_ptr<ISpxSynthesisResult> CSpxSynthesizer::CreateResult(const std::wstring& requestId, ResultReason reason, uint8_t* audio_buffer, size_t audio_length)
{
    // Get output format
    auto audioStream = SpxQueryInterface<ISpxAudioStream>(m_audioOutput);
    auto requiredFormatSize = audioStream->GetFormat(nullptr, 0);
    auto format = SpxAllocWAVEFORMATEX(requiredFormatSize);
    audioStream->GetFormat(format.get(), requiredFormatSize);

    // Build result
    auto result = SpxCreateObjectWithSite<ISpxSynthesisResult>("CSpxSynthesisResult", SpxSiteFromThis(this));
    auto resultInit = SpxQueryInterface<ISpxSynthesisResultInit>(result);
    resultInit->InitSynthesisResult(requestId, reason, REASON_CANCELED_NONE, CancellationErrorCode::NoError,
        audio_buffer, audio_length, format.get(), SpxQueryInterface<ISpxAudioOutputFormat>(m_audioOutput)->HasHeader());
    auto events = this->QueryInterfaceInternal<ISpxSynthesizerEvents>();
    resultInit->SetEvents(events);

    return result;
}

void CSpxSynthesizer::FireResultEvent(std::shared_ptr<ISpxSynthesisResult> result)
{
    auto reason = result->GetReason();
    switch (reason)
    {
    case Microsoft::CognitiveServices::Speech::ResultReason::SynthesizingAudioStarted:
        FireSynthesisStarted(result);
        break;

    case Microsoft::CognitiveServices::Speech::ResultReason::SynthesizingAudio:
        FireSynthesizing(result);
        break;

    case Microsoft::CognitiveServices::Speech::ResultReason::SynthesizingAudioCompleted:
        FireSynthesisCompleted(result);
        break;

    case Microsoft::CognitiveServices::Speech::ResultReason::Canceled:
        FireSynthesisCanceled(result);
        break;

    default:
        break;
    }
}

void CSpxSynthesizer::FireSynthesisEvent(std::list<std::pair<void*, std::shared_ptr<SynthEvent_Type>>> events, std::shared_ptr<ISpxSynthesisResult> result)
{
    auto iterator = events.begin();
    while (iterator != events.end())
    {
        auto pevent = iterator->second;
        if (pevent != nullptr)
        {
            auto synthEvent = SpxCreateObjectWithSite<ISpxSynthesisEventArgs>("CSpxSynthesisEventArgs", SpxSiteFromThis(this));
            auto argsInit = SpxQueryInterface<ISpxSynthesisEventArgsInit>(synthEvent);
            argsInit->Init(result);

            pevent->Signal(synthEvent);
        }

        iterator++;
    }
}

void CSpxSynthesizer::EnsureTtsEngineAdapter()
{
    if (m_ttsAdapter == nullptr)
    {
        InitializeTtsEngineAdapter();
    }
}

void CSpxSynthesizer::InitializeTtsEngineAdapter()
{
    // determine which type (or types) of tts engine adapters we should try creating...
    bool tryRest = false, tryUsp = false;
    std::string endpoint = GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_Endpoint), "");
    if (!endpoint.empty())
    {
        auto url = CSpxSynthesisHelper::ParseUrl(endpoint);
        if (Protocol::HTTP == url.protocol)
        {
            tryRest = true;
        }
        else if (Protocol::WebSocket == url.protocol)
        {
            tryUsp = true;
        }
    }
    
    bool tryMock = PAL::ToBool(GetStringValue("SDK-INTERNAL-UseTtsEngine-Mock", PAL::BoolToString(false).c_str())) ||
                   PAL::ToBool(GetStringValue("CARBON-INTERNAL-UseTtsEngine-Mock", PAL::BoolToString(false).c_str()));
    tryRest = tryRest || PAL::ToBool(GetStringValue("SDK-INTERNAL-UseTtsEngine-Rest", PAL::BoolToString(false).c_str())) ||
                   PAL::ToBool(GetStringValue("CARBON-INTERNAL-UseTtsEngine-Rest", PAL::BoolToString(false).c_str()));
    tryUsp = tryUsp || PAL::ToBool(GetStringValue("SDK-INTERNAL-UseTtsEngine-Usp", PAL::BoolToString(false).c_str())) ||
                  PAL::ToBool(GetStringValue("CARBON-INTERNAL-UseTtsEngine-Usp", PAL::BoolToString(false).c_str()));
    bool tryLocal = PAL::ToBool(GetStringValue("SDK-INTERNAL-UseTtsEngine-Local", PAL::BoolToString(false).c_str())) ||
                    PAL::ToBool(GetStringValue("CARBON-INTERNAL-UseTtsEngine-Local", PAL::BoolToString(false).c_str()));

    // if nobody specified which type(s) of TTS engine adapters this session should use, we'll use the REST
    if (!tryMock && !tryRest && !tryUsp && !tryLocal)
    {
        tryRest = true;
    }

    // try to create the REST API adapter...
    if (m_ttsAdapter == nullptr && tryRest)
    {
        m_ttsAdapter = SpxCreateObjectWithSite<ISpxTtsEngineAdapter>("CSpxRestTtsEngineAdapter", this);
    }

    // try to create the USP adapter...
    if (m_ttsAdapter == nullptr && tryUsp)
    {
        m_ttsAdapter = SpxCreateObjectWithSite<ISpxTtsEngineAdapter>("CSpxUspTtsEngineAdapter", this);
    }

    // try to create the mock tts engine adapter...
    if (m_ttsAdapter == nullptr && tryMock)
    {
        m_ttsAdapter = SpxCreateObjectWithSite<ISpxTtsEngineAdapter>("CSpxMockTtsEngineAdapter", this);
    }

    // try to create the local tts engine adapter...
    if (m_ttsAdapter == nullptr && tryLocal)
    {
        m_ttsAdapter = SpxCreateObjectWithSite<ISpxTtsEngineAdapter>("CSpxLocalTtsEngineAdapter", this);
    }

    // if we still don't have an adapter... that's an exception
    SPX_IFTRUE_THROW_HR(m_ttsAdapter == nullptr, SPXERR_NOT_FOUND);
}

void CSpxSynthesizer::ReleaseTtsEngineAdapter()
{
    if (m_ttsAdapter != nullptr)
    {
        m_ttsAdapter->Term();
    }
}

} } } } // Microsoft::CognitiveServices::Speech::Impl
