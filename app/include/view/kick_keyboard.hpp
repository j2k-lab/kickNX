#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>
#include <vector>

// On-screen keyboard used by the Kick chat composers, so typing a message
// never has to leave the app for the system keyboard.
class KickKeyboard : public brls::Box {
public:
    struct Callbacks {
        std::function<void(const std::string&)> text;
        std::function<void()> backspace;
        std::function<void()> emotes;
        std::function<void()> send;
    };

    struct Options {
        float keyHeight = 68.0f;
        float fontSize = 18.0f;
        float spacing = 5.0f;
        // A composer with its own Send button gets a Caps Lock key instead.
        bool sendKey = true;
        // See-through keys, for keyboards drawn over the stream.
        bool translucent = false;
    };

    KickKeyboard(Callbacks callbacks, const Options& options);

    /// Drops a pending one-shot Shift, e.g. after a message was sent.
    void resetShift();

    /// The "q" key, a natural place to start typing.
    brls::View* getStartKey();

private:
    struct KeySlot {
        brls::Button* button = nullptr;
        size_t row = 0;
        size_t column = 0;
    };

    const std::vector<std::vector<std::string>>& currentLayout() const;
    void buildRows();
    void refreshKeys();
    void pressKey(size_t row, size_t column);
    bool upperCase() const { return shifted || capsLock; }

    Callbacks callbacks;
    Options options;
    std::vector<std::vector<std::string>> alphaLayout;
    std::vector<std::vector<std::string>> symbolLayout;
    bool shifted = false;
    bool capsLock = false;
    bool symbols = false;
    std::vector<KeySlot> slots;
};
