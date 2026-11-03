#pragma once
#include <cstdint>

#include "app_event.h"

namespace App {

// A page owns the whole 320x240 canvas while it is on top of the stack.
// draw() must be the only writer of Display::canvas() for that frame.
class Page {
public:
    virtual ~Page() = default;
    virtual const char* id() const = 0;
    virtual void enter() {}
    virtual void leave() {}
    virtual void handle(const Event&) {}
    virtual void tick(uint32_t) {}
    virtual void draw() = 0;

    void invalidate() { dirty_ = true; }
    void clean() { dirty_ = false; }
    bool dirty() const { return dirty_; }

protected:
    bool dirty_ = true;
};

// Page factories. Each returns a process-lifetime singleton: pages are kept
// alive so a push/pop never touches the heap while audio is streaming.
Page* createHomePage();
Page* createSettingsPage();
Page* createMusicListPage();
Page* createTrackListPage();
Page* createPlayerPage();
Page* createStoryListPage();
Page* createStoryPlayerPage();

// Navigation stack helpers.
void pushPage(Page* page);
void popPage();
void goHome();
void openSettings();

}
