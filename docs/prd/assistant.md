# PRD: Claude on every screen (ASSIST) and global scroll (SCROLL)

## User story
"When I spawn a new screen it's empty and navigating it with the mouse is a pain." Each screen has a Claude button.
I press it and talk; Claude sees and drives that screen ("open YouTube, search this") and answers in a chat bubble.

## Headset (c4)
- [ ] Button always visible on the right edge of every screen (Anthropic-style icon); on press, an animation (orbiting dots)
- [ ] When pressed, dictation (existing Parakeet path) goes straight to that screen's assistant: no gesture, no Enter
- [ ] A chat panel further right: Markdown, both sides of the conversation, scrollable
- [ ] Flag ASSIST (off by default)

## Scroll (c4), flag SCROLL
- [ ] Hand wave up/down scrolls vertically, left/right horizontally, on the screen being pointed at
- [ ] Works on every screen and in the chat panel (global)

## Backend (42)
- [ ] One session per screen id: text in, streamed Markdown plus action events out, persistent history
- [ ] Computer use scoped to that screen's display only (screenshot and input mapped to its bounds)
- [ ] Latest Claude model; existing credentials only
- [ ] Protocol doc agreed with c4; mock-model tests; one real end-to-end on a spare display
