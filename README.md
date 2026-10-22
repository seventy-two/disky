# disky

A light Discord client for Windows: DMs, servers and channels, a member
list, names in their role colours, images and videos inline, and a bell
that glows when someone mentions you. Nothing else.

It's written in C++ with Win32, Direct3D 11 and
[Dear ImGui](https://github.com/ocornut/imgui), using WinHTTP (REST and the
gateway's WebSocket), WIC for images and Media Foundation for video, so the
result is a single `disky.exe` with nothing to install.

> [!WARNING]
> Discord's terms don't allow third-party clients signing in as a user.
> disky keeps to what the web app does (same gateway capabilities, rate
> limits respected, no automation), but using it is at your own risk.

## Using it

- **Sign in** by scanning the QR code with the Discord app on your phone
  (profile picture, then Scan QR Code), or paste a token. The token is kept
  in `%APPDATA%\disky\config.json`, encrypted with DPAPI.
- **Servers** are small square icons down the left, in your Discord order (folders flattened);
  **Direct messages** is the top button. Unread channels are bright, with
  mention counts in red.
- **Messages**: Enter sends, Shift+Enter starts a new line. `@`, `:` and
  `#` suggest people, emoji and channels (Tab or Enter picks). Up in an empty
  box edits your last message; Escape cancels a reply or edit.
- **Fold** the server list, the channel list or the member list away with
  the small chevron on each; a thin strip brings it back.
- **Typing**: who else is typing shows under the box; disky tells Discord
  when you're typing, as its own app does.
- **Attach** with the paperclip, by dropping files on the window, or by
  pasting an image.
- Hover a message to react, reply, edit or delete (Shift+click the bin to
  skip the question); right-click for the same and to copy.
- Click a video to play it inline; click again to pause, use the bar
  to seek, and the mouse wheel over it for volume.
- **GIFs** play while the mouse is over them (uploads, Tenor and Giphy links,
  GIF stickers); animated emoji play while you're over their message.
  Clicking a GIF opens it in your browser.
- **Notifications** stay inside disky: the bell in the title bar glows,
  with a count, for mentions of you or your roles, @everyone where your
  Discord settings allow, and DMs, unless you're already looking at that
  channel. Click it for the list (newest first; click one to go there).
  Opening the list, or the channel, marks them seen. Discord's own mutes
  apply. The list lasts while disky runs. Closing the window keeps disky
  in the tray; Quit is in the tray menu.

## Building

`build.bat` builds `build\disky.exe` with Visual Studio 2022's C++ tools
and runs the tests. The portable parts (the store, the markdown parser and GIF frames)
also build on Linux:

```sh
g++ -std=c++17 -I src -I third_party tests/tests.cpp src/store.cpp src/markdown.cpp src/gif.cpp -o tests && ./tests
```

`tools/genemoji.py` regenerates `src/emoji_data.inc` (the picker's emoji and
shortcodes) from gemoji's `emoji.json`.

## How it talks to Discord

- Identifies as Discord's web app in Microsoft Edge (the installed Edge's
  version), with the web app's current build number, read from
  discord.com once a day.
- Gateway v9, JSON, no compression; resumes after drops. Member lists come
  from guild subscriptions (op 37), the colours of message authors from
  op 8 member requests.
- Images and emoji are drawn from Discord's CDN and Twemoji (via jsDelivr).

Third-party code: Dear ImGui (MIT), nlohmann/json (MIT), Nayuki's QR Code
generator (MIT), gemoji's emoji list (MIT).
