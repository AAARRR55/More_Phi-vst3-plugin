#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <unordered_map>

namespace more_phi::ParameterBinding {

inline void setValueWithGesture(juce::AudioProcessorParameter& parameter, float normalisedValue)
{
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, normalisedValue));
    parameter.endChangeGesture();
}

inline bool setChoiceIndexWithGesture(juce::AudioProcessorValueTreeState& apvts,
                                      const juce::String& parameterId,
                                      int choiceIndex,
                                      int choiceCount)
{
    auto* parameter = apvts.getParameter(parameterId);
    if (parameter == nullptr || choiceCount <= 0)
        return false;

    const int clampedIndex = juce::jlimit(0, choiceCount - 1, choiceIndex);
    const float normalised = choiceCount <= 1
        ? 0.0f
        : static_cast<float>(clampedIndex) / static_cast<float>(choiceCount - 1);

    setValueWithGesture(*parameter, normalised);
    return true;
}

namespace detail {

template <typename Component, typename Attachment>
class AttachmentStore final : private juce::ComponentListener
{
public:
    static AttachmentStore& instance()
    {
        static AttachmentStore store;
        return store;
    }

    void bind(Component& component, std::unique_ptr<Attachment> attachment)
    {
        const auto key = static_cast<void*>(&component);
        attachments_[key] = std::move(attachment);
        component.addComponentListener(this);
    }

private:
    void componentBeingDeleted(juce::Component& component) override
    {
        attachments_.erase(static_cast<void*>(&component));
    }

    std::unordered_map<void*, std::unique_ptr<Attachment>> attachments_;
};

} // namespace detail

inline void bindSlider(juce::Slider& slider,
                       juce::AudioProcessorValueTreeState& apvts,
                       const juce::String& parameterId)
{
    if (apvts.getParameter(parameterId) == nullptr)
        return;

    using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    detail::AttachmentStore<juce::Slider, Attachment>::instance().bind(
        slider, std::make_unique<Attachment>(apvts, parameterId, slider));
}

inline void bindToggleButton(juce::Button& button,
                             juce::AudioProcessorValueTreeState& apvts,
                             const juce::String& parameterId)
{
    if (apvts.getParameter(parameterId) == nullptr)
        return;

    using Attachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    detail::AttachmentStore<juce::Button, Attachment>::instance().bind(
        button, std::make_unique<Attachment>(apvts, parameterId, button));
}

inline void bindComboBox(juce::ComboBox& comboBox,
                         juce::AudioProcessorValueTreeState& apvts,
                         const juce::String& parameterId)
{
    if (apvts.getParameter(parameterId) == nullptr)
        return;

    using Attachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    detail::AttachmentStore<juce::ComboBox, Attachment>::instance().bind(
        comboBox, std::make_unique<Attachment>(apvts, parameterId, comboBox));
}

} // namespace more_phi::ParameterBinding
