// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

// This is a precompiled header, it must be on its own, followed by a blank
// line, to prevent clang-format from reordering it with other headers.
#include "pch.h"

#include "toggle_audio_mixer.h"

namespace Microsoft::MixedReality::WebRTC {

ToggleAudioMixer::ToggleAudioMixer()
    : base_impl_(webrtc::AudioMixerImpl::Create()) {}

bool ToggleAudioMixer::AddSource(Source* audio_source) {
  RTC_DCHECK(audio_source);

  rtc::CritScope lock(&crit_);
  // By default add the source as not rendered.
  auto result =
      source_from_id_.insert({audio_source->Ssrc(), {audio_source, false}});
  if (!result.second) {
    // The source has already been added through PlaySource. Update the Source*.
    auto& known_source = result.first->second;
    RTC_DCHECK(!known_source.source)
        << "Source " << audio_source->Ssrc() << " added twice";
    known_source.source = audio_source;

    // If RenderSource(true) has been called before, start mixing the source
    // through the base impl.
    if (known_source.is_rendered) {
      TryAddToBaseImpl(known_source);
    }
  }

  return true;
}

void ToggleAudioMixer::TryAddToBaseImpl(KnownSource& known_source) {
  bool added_succesfully = base_impl_->AddSource(known_source.source);
  if (!added_succesfully) {
    RTC_LOG_F(LS_WARNING) << "Cannot mix source "
                          << known_source.source->Ssrc();
    known_source.is_rendered = false;
  }
}

void ToggleAudioMixer::RemoveSource(Source* audio_source) {
  RTC_DCHECK(audio_source);

  rtc::CritScope lock(&crit_);
  // Check if the source is being played.
  const auto iter = source_from_id_.find(audio_source->Ssrc());
  RTC_DCHECK(iter != source_from_id_.end())
      << "Cannot find source " << audio_source->Ssrc();

  if (iter->second.is_rendered) {
    // Stop mixing the source.
    base_impl_->RemoveSource(audio_source);
  }
  // Forget the source.
  source_from_id_.erase(iter);
}

static const int16_t zerobuf[200]{};

void ToggleAudioMixer::Mix(size_t number_of_channels,
                           webrtc::AudioFrame* audio_frame_for_mixing) {
  std::vector<Source*> redirected_sources;
  bool some_source_is_played = false;
  {
    rtc::CritScope lock(&crit_);

    // Collect the redirected sources.
    for (auto&& pair : source_from_id_) {
      if (!pair.second.is_rendered) {
        redirected_sources.push_back(pair.second.source);
      } else {
        some_source_is_played = true;
      }
    }

    if (some_source_is_played) {
      // Mix rendered sources using the base impl. Do inside the lock in case
      // sources are added/removed by RenderSource on a different thread.
      base_impl_->Mix(number_of_channels, audio_frame_for_mixing);
    }
  }

  for (auto& source : redirected_sources) {
    // This pumps the source and fires the frame observer callbacks
    // which in turn fill the AudioTrackReadBuffer buffers
    const auto audio_frame_info = source->GetAudioFrameWithInfo(
        source->PreferredSampleRate(), audio_frame_for_mixing);

    if (audio_frame_info == Source::AudioFrameInfo::kError) {
      RTC_LOG_F(LS_WARNING) << "failed to GetAudioFrameWithInfo() from source";
      continue;
    }
  }

  if (!some_source_is_played) {
    // Return an empty frame.
    audio_frame_for_mixing->UpdateFrame(
        0, zerobuf, 80, 8000, webrtc::AudioFrame::kNormalSpeech,
        webrtc::AudioFrame::kVadUnknown, number_of_channels);
  }
}

void ToggleAudioMixer::RenderSource(int ssrc, bool render) {
  rtc::CritScope lock(&crit_);

  // If the source is unknown add a KnownSource with null Source* to remember
  // the choice.
  const auto result = source_from_id_.insert({ssrc, {nullptr, render}});

  if (!result.second) {
    // The source has already been added through RenderSource. Modify the render
    // state.
    KnownSource& known_source = result.first->second;
    if (render && !known_source.is_rendered) {
      // Add the source to the ones mixed by the base impl.
      TryAddToBaseImpl(known_source);
    } else if (!render && known_source.is_rendered) {
      // Remove the source from the ones mixed by the base impl.
      base_impl_->RemoveSource(known_source.source);
    }
    // else the state of the source is unchanged.
    known_source.is_rendered = render;
  }
}

}  // namespace Microsoft::MixedReality::WebRTC
