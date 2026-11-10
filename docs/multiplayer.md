# Saved servers

In Network Game, click **Add/Remove**, enter a hostname or IPv4 address in the first
row and click **Add** (or press Enter), then confirm. An optional `:port` selects a
different UDP port; the default is `18590`. For example: `192.168.1.10`,
`example.com:18591`, or `127.0.0.1:18590` for a host running on your own computer.

Adding saves the address and selects its row immediately after discovery starts.
The row shows **Checking...**, then the game's name, map, players and ping when it
answers. **No response** means it did not answer the discovery attempts;
**Address not found** means the address could not be resolved. Both remain visible.
Only a responding game can be joined. **Refresh** checks again.

The dialog's second row is a scrollable dropdown of all saved addresses, including
offline and unresolved entries. Select an address, click **Remove**, then confirm.
This removes that address from both `servers.txt` sources; other aliases remain
saved. A game discovered independently on the LAN or through the public listing
can still appear afterward. Editing saved addresses lets an ongoing public listing
finish without starting another login.
Addresses supplied with `--server` are not removed from the command line.

New addresses go into `servers.txt` in the game-data folder. The browser also reads
`servers.txt` beside the executable. Files use one address per line, with `#` for
comments; comments and unrelated lines are preserved when editing. Refresh reloads
manual file changes. If one source cannot be read, the browser still shows addresses
from readable sources and reports the error. Editing requires all sources to be readable.
A failed save displays an error and keeps the dialog open. All replacement files
are prepared before removal changes any source. Each replacement is atomic, but a
failure during the final replacements can still leave a partially completed removal;
the browser reloads the files so the action can be retried.
Cancel or Escape dismisses a confirmation without changing the file. Escape closes
an open dropdown first; otherwise it closes the dialog. Each confirmed action saves
immediately. Command-V on Mac or Ctrl-V on Windows/Linux replaces the input with
clipboard text. Text that exceeds the input limit is rejected without changing the input.
