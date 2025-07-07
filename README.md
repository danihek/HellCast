# HellCast

![image](https://github.com/user-attachments/assets/195ae15c-b375-41ef-b8c2-4086e13d3dd5)

## What is it?
Hellcast allows you to show your music in terminal. If you terminal emulator supports sixel it can also render artcover :)

## Controls
You can do hjkl (or arrows) to seek by 5s or play next/prev track, space is for play-pause.

This thingy also has mouse support so you can click on progress bar IN TERMINAL to skip to certain moment.

## Dependecies
- curl
- ncurses
- glib-2.0
- pkg-config (compile time)

I recommend opening it in tmux, cuz colors are fcked for some reason.

## BUILD

```sh
make
```

---

# TODOs

- [ ] fix segfaults if music is not playing...
- [ ] **multi**player support and switching between them
- [ ] configuration file for layout modification
- [ ] hellwal support ( system colors based on music artcover? sick imo )
- [ ] if this is even possible cava support
