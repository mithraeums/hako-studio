# hako studio — iOS shell

The app exists for one reason: **no browser on iOS can open a folder.**

- No File System Access API (`showDirectoryPicker`) — Safari implements none of
  it, on any platform.
- `<input webkitdirectory>` is unimplemented on iOS.
- iCloud Drive has no third-party API.

`UIDocumentPicker` plus a security-scoped bookmark is the only way to reach the
user's real files, and only a native app can hold one. Everything else here —
the UI, the agent, the providers, the tools — is the same `index.html` and
`hako.wasm` the website serves. **No agent logic is written in Swift, and none
ever should be.**

## Shape

```
UIDocumentPicker  ->  security-scoped bookmark  ->  FolderServer (127.0.0.1)
                                                          |
                                            WKWebView -> index.html
                                                          |
                                              Web Worker -> hako.wasm
```

`hako.wasm` blocks — it calls a filesystem function and waits. WKWebView's
JS-to-native channel is asynchronous and cannot answer a blocked caller, so the
folder is served over loopback HTTP and the worker reads it with a **synchronous
XHR**, the same mechanism the agent already uses for provider traffic. No
Asyncify, no SharedArrayBuffer, no COOP/COEP.

## The /fs contract

Served under `/t/<launch-token>/`, so every request the page makes inherits the
prefix and another app on the device cannot guess the URL.

| request | answer |
|---|---|
| `GET fs/root` | `{"path":"/FolderName"}` |
| `GET fs/read?path=` | file bytes, or 404 |
| `POST fs/write?path=&append=0` | body is the contents |
| `GET fs/stat?path=` | `{"size":,"mtime":,"isDir":}` or 404 |
| `POST fs/mkdirp?path=` · `POST fs/remove?path=` | `{"ok":true}` |
| `GET fs/list?prefix=` · `GET fs/listdirs?prefix=` | `{"files":[…]}` / `{"dirs":[…]}` |

Paths are resolved against the chosen folder and anything climbing out of it is
refused — a model writing an invented path must not reach the rest of the device.

Web side: `nativeStorage()` in `tools/hako-glue.js`. The worker probes
`fs/root` at boot; if it answers, the shell is hosting and OPFS is not used for
the workspace at all.

## Building it

Not built here — this checkout has Command Line Tools, not Xcode, and no iOS SDK.
What follows is what the project needs; expect to fix small things on first
compile.

1. Xcode → new **iOS App**, SwiftUI, name `hako`, then add `HakoApp.swift` and
   `FolderServer.swift` (delete the template's `ContentView`/`App`).
2. `make site` in the parent directory, then drag `dist/` into the project as a
   **folder reference** (blue) named `web` — `FolderServer` reads from
   `Bundle.main/web`.
3. Info.plist:
   - `NSAppTransportSecurity` → `NSAllowsLocalNetworking = YES` (loopback HTTP).
   - `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace` = YES, so the
     app's own documents show up in Files too.
4. Signing: your own team. Personal free provisioning works for a device build
   and expires after seven days; TestFlight internal skips review and lasts 90.

## Updates

Apple forbids downloading executable code, so the wasm and the page ship inside
the app and move at release cadence, while a hosted deployment updates whenever
the site does. That skew is permanent, which is why the UI asks
`/api/state` what the core supports instead of assuming a version.

## Android

The same shape works: `ACTION_OPEN_DOCUMENT_TREE` gives a persistable tree URI,
a small `NanoHTTPD`-style loopback server implements the table above, and the
WebView loads the same page. Not written yet.
