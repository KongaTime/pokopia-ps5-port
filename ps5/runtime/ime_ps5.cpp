// SPDX-License-Identifier: GPL-3.0-or-later
// The game's software keyboard (the swkbd library applet, runtime/src/services/am.cpp) on the console's own on-screen keyboard:
// sceImeDialog. The PC build shows a prompt window (swkbd_prompt.cpp); without one the console answered every request with
// "cancelled", and Pokopia asks for the player's name again at once, so the name entry never ended.
//
// The dialog's interface is the one the PS4 SDK has (also as OpenOrbis documents it: SceImeDialogParam, 96 bytes), which the PS5
// keeps. The dialog runs in the system; this thread waits for it. When it cannot be opened at all the answer is "Player", so the
// game can go on.
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include "nxrt/log.h"
#include "nxrt/services_api.h"

extern "C" {
struct PkpImeDialogParam {          // SceImeDialogParam
    int32_t user_id;
    int32_t type;                   // 0: default (any text)
    uint64_t supported_languages;   // 0: the system's
    int32_t enter_label;            // 0: default
    int32_t input_method;           // 0: default
    void* filter;                   // SceImeTextFilter, none
    uint32_t option;
    uint32_t max_text_length;
    char16_t* input_text_buffer;
    float posx, posy;
    int32_t horizontal_alignment;   // 1: centre
    int32_t vertical_alignment;     // 1: centre
    const char16_t* placeholder;
    const char16_t* title;
    int8_t reserved[16];
};
static_assert(sizeof(PkpImeDialogParam) == 96, "SceImeDialogParam layout");
struct PkpImeDialogResult {         // SceImeDialogResult
    int32_t end_status;             // 0 OK, 1 cancelled, 2 aborted
    int8_t reserved[12];
};
int sceImeDialogInit(const PkpImeDialogParam* param, const void* extended);
int sceImeDialogGetStatus(void);    // 0 none, 1 running, 2 finished
int sceImeDialogGetResult(PkpImeDialogResult* result);
int sceImeDialogTerm(void);
int sceUserServiceInitialize(const void*);
int sceCommonDialogInitialize(void);   // libSceCommonDialog (ps5/stubs): before any system dialog
int sceUserServiceGetInitialUser(int32_t* user);
}

namespace nxrt::services::swkbdui {
namespace {

std::u16string to_utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (n > 1) c &= 0xff >> (n + 1);
        for (int k = 1; k < n && i + k < s.size(); ++k) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        i += n;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<char16_t>(0xd800 + (c >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00 + (c & 0x3ff)));
        } else {
            out.push_back(static_cast<char16_t>(c));
        }
    }
    return out;
}

std::string to_utf8(const char16_t* s) {
    std::string out;
    for (size_t i = 0; s[i]; ++i) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xc0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { out += static_cast<char>(0xe0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3f)); out += static_cast<char>(0x80 | (c & 0x3f)); }
        else { out += static_cast<char>(0xf0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3f)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3f)); out += static_cast<char>(0x80 | (c & 0x3f)); }
    }
    return out;
}

}  // namespace

bool win32_prompt(const Request& r, std::string* text) {
    NXLOG_I("am", "swkbd: console keyboard: starting");
    (void)sceUserServiceInitialize(nullptr);
    int32_t user = 0;
    const int urc = sceUserServiceGetInitialUser(&user);
    NXLOG_I("am", "swkbd: console keyboard: user %d (0x%08x)", user, static_cast<unsigned>(urc));
    // The system dialogs need their common layer first; without it sceImeDialogInit never returned (the first console try).
    static const int cdrc = sceCommonDialogInitialize();
    NXLOG_I("am", "swkbd: console keyboard: sceCommonDialogInitialize -> 0x%08x", static_cast<unsigned>(cdrc));

    const uint32_t max_len = r.max_len ? r.max_len : 500;
    std::u16string buffer = to_utf16(r.initial);
    buffer.resize(max_len + 1, u'\0');
    buffer[max_len] = u'\0';
    const std::u16string title = to_utf16(!r.header.empty() ? r.header : r.title);
    const std::u16string placeholder = to_utf16(r.guide);

    PkpImeDialogParam p{};
    p.user_id = user;
    p.max_text_length = max_len;
    p.input_text_buffer = buffer.data();
    p.posx = 1920.0f / 2;
    p.posy = 1080.0f / 2;
    p.horizontal_alignment = 1;
    p.vertical_alignment = 1;
    p.title = title.c_str();
    p.placeholder = placeholder.empty() ? nullptr : placeholder.c_str();

    int rc = sceImeDialogInit(&p, nullptr);
    NXLOG_I("am", "swkbd: console keyboard: sceImeDialogInit -> 0x%08x, status %d", static_cast<unsigned>(rc), sceImeDialogGetStatus());
    if (rc < 0) {   // one already up (an earlier request that was not taken down): end it and try once more
        sceImeDialogTerm();
        rc = sceImeDialogInit(&p, nullptr);
    }
    if (rc < 0) {
        NXLOG_W("am", "swkbd: the console keyboard (sceImeDialogInit) failed: 0x%08x; answering \"Player\"", static_cast<unsigned>(rc));
        *text = "Player";
        return true;
    }
    NXLOG_I("am", "swkbd: console keyboard open (\"%s\", up to %u characters)", to_utf8(title.c_str()).c_str(), max_len);
    // Waits for the player; a dialog that never shows (and so never finishes) ends after 3 minutes with "Player".
    int status = 1, waited = 0;
    while ((status = sceImeDialogGetStatus()) == 1 && waited < 180000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        waited += 50;
        if (waited % 10000 == 0) NXLOG_I("am", "swkbd: console keyboard: still open after %d s", waited / 1000);
    }
    PkpImeDialogResult res{};
    const int grc = sceImeDialogGetResult(&res);
    NXLOG_I("am", "swkbd: console keyboard: status %d, result 0x%08x, end status %d", status, static_cast<unsigned>(grc), res.end_status);
    if (status == 1) {
        sceImeDialogTerm();
        *text = "Player";
        return true;
    }
    sceImeDialogTerm();
    if (res.end_status != 0) return false;
    *text = to_utf8(buffer.c_str());
    if (r.min_len && to_utf16(*text).size() < r.min_len) return false;
    return true;
}

}  // namespace nxrt::services::swkbdui
