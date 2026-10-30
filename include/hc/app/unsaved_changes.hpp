// =============================================================================
//  app/unsaved_changes.hpp — "Save changes?" logic for the studio window, kept free of Windows
//  code so it can be unit-tested.
//
//  The session is "dirty" when what a session file WOULD contain right now differs from what it
//  contained at the last save / open / new. The app passes that text (its "fingerprint") in:
//    markClean(fp)  after Save, Open, New (and at start-up)
//    update(fp)     every few frames and right before asking (any stem, knob, fader, switch,
//                   loop or tempo change shows up in the text, because the text IS the session)
//  Moving a knob away and back again therefore clears the "*" again, like the file on disk.
//
//  confirmDiscard() is what Close / New / Open do before throwing the session away:
//    clean            -> go on, no question
//    dirty + Yes      -> save (Save As when untitled); go on ONLY if the save worked
//    dirty + No       -> go on, changes dropped
//    dirty + Cancel   -> stay
// =============================================================================
#pragma once
#include <string>
#include <utility>

namespace hc {

class UnsavedChanges {
public:
    void markClean(std::string fingerprint) { clean_ = std::move(fingerprint); dirty_ = false; }
    // Returns the new dirty state.
    bool update(const std::string& fingerprint) { dirty_ = fingerprint != clean_; return dirty_; }
    bool dirty() const noexcept { return dirty_; }

private:
    std::string clean_;
    bool dirty_ = false;
};

enum class SaveAnswer { Yes, No, Cancel };

// ask():  shows "Save changes to <name>?" and returns the answer (only called when dirty).
// save(): saves the session (Save As if it has no file yet); returns true if it was written.
template <class Ask, class Save>
bool confirmDiscard(bool dirty, Ask&& ask, Save&& save) {
    if (!dirty) return true;
    switch (ask()) {
    case SaveAnswer::Yes: return save();
    case SaveAnswer::No: return true;
    case SaveAnswer::Cancel: break;
    }
    return false;
}

// "My Song.hcs" from a full path (either slash); "Untitled" when the session has no file yet.
inline std::string sessionDisplayName(const std::string& pathUtf8) {
    if (pathUtf8.empty()) return "Untitled";
    const auto p = pathUtf8.find_last_of("\\/");
    std::string n = p == std::string::npos ? pathUtf8 : pathUtf8.substr(p + 1);
    return n.empty() ? std::string("Untitled") : n;
}

// Window title: "*My Song.hcs - APP" while there are unsaved changes, "My Song.hcs - APP" otherwise.
inline std::string sessionWindowTitle(const std::string& app, const std::string& name, bool dirty) {
    return (dirty ? "*" : "") + name + " - " + app;
}

} // namespace hc
