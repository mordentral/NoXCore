// (noxcpp) When the program asks which the player will play with, the game controller or the mouse
// and keyboard (pad/pad_mode.cpp asks; GamepadManager::setMode holds the answer). Pure, so the tests
// check the rules as they are:
//
//   - a pad there at start-up: asked before anything else is shown;
//   - a pad plugged in later while the mouse's mode is in use: asked once something may ask (a
//     menu up, never in the middle of a game's fight), unless the player already chose the mouse
//     this run (then it isn't asked again: the Controller screen's Use switches);
//   - the pad's mode chosen and the last pad gone: the mouse's by itself, and a pad back asks again;
//   - the answer isn't kept between runs: each start asks again.
#pragma once

namespace noxsdl::pad {

class InputModeChoice {
public:
    /// A pad came (or was there at start-up). `padMode`: the pad's mode is in use already;
    /// `fellBack`: the mouse's mode came by the last pad going (the pad had been chosen).
    void padArrived(bool padMode, bool fellBack)
    {
        if (!padMode && !asking_ && (!mouseChosen_ || fellBack)) {
            due_ = true;
        }
    }
    /// The last pad went: nothing to ask about (a question up is dropped by the caller).
    void lastPadGone()
    {
        due_ = false;
        asking_ = false;
    }
    /// Whether to put the question up now; `mayAsk`: nothing that mustn't be interrupted is up.
    bool askNow(bool mayAsk) const { return due_ && !asking_ && mayAsk; }
    bool due() const { return due_; }
    bool asking() const { return asking_; }
    void opened()
    {
        asking_ = true;
        due_ = false;
    }
    /// The answer (true: the pad's mode).
    void answered(bool pad)
    {
        asking_ = false;
        mouseChosen_ = !pad;
    }
    /// The mode set another way (the Controller screen's Use): no question due.
    void chose(bool pad)
    {
        due_ = false;
        mouseChosen_ = !pad;
    }

private:
    bool due_ = false;
    bool asking_ = false;
    bool mouseChosen_ = false;
};

} // namespace noxsdl::pad
