#pragma once
// Native Peggle dialogs for the mod's menus, built with the game's own dialog code so they look and animate
// like every other dialog. Results come back through OnDialogResult (dialog id, button id).
#include <functional>
#include <string>

namespace dialogs {

// Button ids reported to the result handler.
constexpr int kFooter = 1000;  // the single bottom button of Message() / the OK of TextEntry()
constexpr int kCancel = 1001;  // the Cancel of TextEntry()
constexpr int kChoiceA = 2001;
constexpr int kChoiceB = 2002;

using ResultHandler = std::function<void(int dialog_id, int button_id)>;

void Install(ResultHandler handler);

// Header, text and one bottom button.
void Message(int id, const char* header, const std::string& text, const char* footer);
// Header, text, two side-by-side choices (kChoiceA, kChoiceB) and a bottom button (kFooter, e.g. "Cancel").
void Choice(int id, const char* header, const std::string& text, const char* choice_a, const char* choice_b,
            const char* footer);
// Header, a text box with OK (kFooter) and Cancel (kCancel).
void TextEntry(int id, const char* header, const std::string& initial);
std::string TextEntryValue(int id);

void SetText(int id, const std::string& text);  // replace the body text of an open Message/Choice dialog
bool IsOpen(int id);
void Close(int id);

// A dialog of a given size for the caller to fill with buttons. Its results (the bottom button, kFooter, and the
// added ones) go to `handler` rather than the one given to Install.
void* Panel(int id, const char* header, const std::string& text, int width, int height, const char* footer,
            ResultHandler handler);
void* AddButton(int id, int button_id, const char* label, int x, int y, int w, int h);  // placed within the dialog
void SetLabel(void* button, const std::string& label);
// A dialog-style button (the game's green one) for anywhere else; `listener` is a game ButtonListener.
void* NewButton(int button_id, void* listener, const char* label);

}  // namespace dialogs
