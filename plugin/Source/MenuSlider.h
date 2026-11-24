#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// A slider that opens a menu on a right-click (onMenu) instead of moving.
class MenuSlider : public juce::Slider
{
public:
    std::function<void()> onMenu;

    void mouseDown (const juce::MouseEvent& e) override
    {
        menuClick = e.mods.isPopupMenu() && onMenu != nullptr;

        if (menuClick)
            onMenu();
        else
            Slider::mouseDown (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! menuClick)
            Slider::mouseDrag (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! menuClick)
            Slider::mouseUp (e);
    }

private:
    bool menuClick = false;
};
