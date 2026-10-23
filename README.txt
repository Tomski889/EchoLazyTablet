EchoQueue 3.10 - Echo Arena matchmaking from your hand tablet (Echo VR, EchoVRCE)

What it does
  A PLAY tab, first on the hand tablet's tab bar:
  * FIND MATCH queues for a public Echo Arena match (CANCEL SEARCH while searching, with a
    queue timer) and joins it when one is found. F9 in Echo's window does the same.
  * PRIVATE MATCH creates a private Echo Arena match on the server set in EchoQueue.ini.
  * LOBBY takes you from a match back to the social lobby.
  * A stats box (tap to switch): hours played today, matches overall, players online.
  * Joining or leaving fades the headset to black (after [fade] delay_ms).
  In a public match, FIND MATCH and PRIVATE MATCH wait until the match is over (no leaving your
  team mid-game); private matches and the lobby are fine.
  Needs Echo VR build 1683152886 and EchoLoader. On any other build it switches itself off and
  says so in plugins\EchoQueue.log.

What it sends
  When Echo starts, and every 15 minutes while it runs, EchoQueue sends your Echo display name and
  account ID (the OVR-ORG-... number in your Echo profile) and the EchoQueue version to its
  author's access service, which answers whether EchoQueue may be used on this account. Nothing
  else is sent. The author can see who uses EchoQueue and turn it off for an account; if that
  happens, the PLAY page says "ECHOQUEUE IS OFF FOR THIS ACCOUNT" and Echo itself is unaffected.

Installing (players)
  Close Echo VR, run EchoQueueInstaller.exe. Its window (shown by Microsoft Edge, part of Windows;
  without Edge it opens in your browser) finds Echo (or use Change...), shows your tab bar as it will
  look, checks your setup, and offers Install / Repair / Uninstall.
  * Echo Mods: applying an Echo Mods profile switches off plugins the profile doesn't name, so the
    installer names EchoQueue (on) in every Echo Mods profile. If Echo Mods rebuilds the lobby from an
    older snapshot, the window says so: run Repair afterwards.
  * Other tablet mods: EchoQueue builds its tab on top of whatever is on the tablet. Every tab
    already there (stock or another mod's) is kept and spread evenly with PLAY first; their touch
    areas move with them. Up to 8 tabs fit.
  * If another mod's installer rewrites the tablet later, the PLAY tab disappears: run Repair.
  * EchoQueue never changes the element counts of the tablet canvases other mods use to find the
    tablet (its own tab lives in one overlay of its own). Tested with Echo Arcade's real tablet
    data: Echo Arcade's root/tab bar stay exactly as its plugin expects.
  * Echo Arcade (and mods like it) only install onto a stock tablet: install them first, then
    EchoQueue. Their uninstall puts their saved tablet back, which drops the PLAY tab: run
    Repair afterwards.
  * Uninstall puts back exactly what EchoQueue replaced (stock data, or another mod's version)
    and keeps your settings.
  * Before each change the manifest is copied to manifests\48037dc70b0ecab2.echoqueue-backup.
  * Support: EchoQueueInstaller.exe --scan "<game folder>" report.txt writes what it finds.
  Windows may need "Restart as administrator" if the game folder is not writable.

What it changes in the game folder
  bin\win10\plugins\EchoQueue.dll, EchoQueue.ini (settings, kept on uninstall), EchoQueue.layout
  (where EchoQueue's tablet elements are on this PC's tablet), and an EchoQueue.dll entry in
  bin\win10\echoloader.json. In _data\...\win10: one package (packages\48037dc70b0ecab2_<n>), the
  manifest entries for the tablet root, tab bar, touch areas, the PLAY page and its icon, and
  manifests\48037dc70b0ecab2.echoqueue.json (what those entries pointed at before).
  The tab's art is drawn from the player's own copy of Echo's tablet art at install time; no
  game files ship with EchoQueue.

Building
  build.cmd                    dist\EchoQueue.dll (llvm-mingw)
  python installer\build.py    dist\EchoQueueInstaller.exe (PyInstaller; Python libraries in
                               installer\pydeps) and dist\THIRD_PARTY_NOTICES.txt
  python installer\tablet.py --game "<...>" --header src\generated\queue_tab.h
                               regenerates the PLAY page constants (rebuild the DLL after)
  installer\echodata.py        EchoQueue's own reader/writer for Echo's manifest, packages,
                               canvases and component tables
  tools\legacy\                the first build/install scripts (need unlicensed third-party tools)

Credits: the tablet approach and addresses follow kotorvr/echo-arcade and heisthecat31/Doom-on-EchoVR.
MinHook (BSD-2) in vendor\minhook. The installer bundles Python, Tcl/Tk, Pillow, python-zstandard
and texture2ddecoder; licences in THIRD_PARTY_NOTICES.txt.
