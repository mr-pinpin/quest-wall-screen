# PRD: mini-apps on surfaces

## User story
"I spawned a small screen above the oven. I want to tell Claude what mini app I want there, e.g. recipes from the
ingredients I say, and have it built right then. Apps evolve, and always live in git."

## Requirements
- [ ] Apps repo (git, one folder per app: index.html + manifest); every change is a commit
- [ ] house.json maps screen/surface → app id (schema agreed with c4)
- [ ] Runtime: a chromeless browser window on that screen's Mac virtual display (streamed by the atlas)
- [ ] SDK: big-type layout for small far-away screens, voice-text hook, storage, cloud fetch through a Mini proxy
- [ ] Create/update API called by the assistant (42): write, commit, reload the screen
- [ ] Starter apps: kitchen recipes (ingredients → suggestions via Claude) and weather (open-meteo)
