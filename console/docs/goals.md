# AthenaSIP Admin - Goals

1. **The server owns the truth.** The console is a view over the admin API. Nothing that
   matters lives only in the browser, and nothing is shown that the API cannot supply.
2. **Works out of the box.** AthenaSIP serves the bundle itself: no build step and no separate
   web server on a fresh install.
3. **Low dependency.** React, React Router and JsSIP, with hand-written CSS. A new runtime
   dependency needs a reason a few lines of our own code cannot meet.
4. **Usable without a telecoms background.** Each screen explains its concept before asking
   you to configure it.
5. **No secrets in the bundle or the browser.** Passwords are typed and held in memory. The
   phone remembers its SIP address, connection and devices, never a password.
