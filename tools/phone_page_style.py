# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Native Home stylesheet, following Apple's dark-appearance system values.

Colours are the iOS dark system colours (background, grouped surfaces, fills,
labels, separators and tints). Text sizes and tracking follow the iOS text
styles: Large Title 34, Title 2 22, Headline/Body 17, Subheadline 15,
Footnote 13. Content sits on opaque grouped surfaces. The dock, Home cards,
translucent actions and progress track use recovered glass.
"""

STYLESHEET = b'''
  window { background: #000000; color: #ffffff;
           font-family: sans-serif; font-size: 17px; letter-spacing: -0.4px; }

  /* Text styles */
  .title { font-family: sans-serif;
           font-size: 34px; font-weight: 700; letter-spacing: 0.4px; }
  .section { font-size: 17px; font-weight: 600; letter-spacing: -0.4px; }
  .eyebrow { color: rgba(235,235,245,0.6); font-size: 13px; font-weight: 400; letter-spacing: -0.1px; }
  .muted { color: rgba(235,235,245,0.6); font-size: 15px; letter-spacing: -0.2px; }
  .small { font-size: 13px; letter-spacing: -0.1px; }
  .connected { color: #30d158; }
  .pairing-code { font-family: sans-serif;
                  font-size: 44px; font-weight: 600; letter-spacing: 6px; }
  .pairing-warning { color: #ffd60a; }
  separator { background: #38383a; min-height: 1px; }

  /* Grouped surfaces */
  .card { background: #1c1c1e; border-radius: 26px; padding: 20px; }
  .grouped > .card { border-radius: 0; padding: 14px 20px; border-bottom: 1px solid #38383a; }
  .grouped > .card:first-child { border-top-left-radius: 26px; border-top-right-radius: 26px; }
  .grouped > .card:last-child { border-bottom-left-radius: 26px; border-bottom-right-radius: 26px;
                                border-bottom: none; }
  .phone-icon { background: #3a3a3c; border-radius: 12px; padding: 10px; }
  .phone-active .phone-icon { background: #30d158; }

  /* Buttons: grey fill capsules; blue for the primary action */
  button { min-height: 44px; min-width: 24px; padding: 0 20px; border-radius: 999px;
           background: rgba(120,120,128,0.32); color: #ffffff; border: none; box-shadow: none;
           font-size: 17px; font-weight: 600; letter-spacing: -0.4px; }
  button:hover { background: rgba(120,120,128,0.42); }
  button:active { background: rgba(120,120,128,0.22); }
  button:disabled { color: rgba(235,235,245,0.3); background: rgba(120,120,128,0.16); }
  button:disabled image { opacity: 0.3; }
  button:focus-visible { outline: 2px solid #0a84ff; outline-offset: 2px; }
  .quiet { background: rgba(120,120,128,0.32); }
  .suggested-action { background: #0a84ff; color: #ffffff; }
  .suggested-action:hover { background: #409cff; }
  .suggested-action:active { background: #0071e3; }
  .suggested-action:disabled { background: rgba(10,132,255,0.3); color: rgba(255,255,255,0.4); }
  .destructive-action { background: transparent; color: #ff453a; }
  .destructive-action:hover { background: rgba(255,69,58,0.14); }
  popover contents { background: #2c2c2e; color: #ffffff; border-radius: 22px; padding: 8px;
                     box-shadow: 0 12px 40px rgba(0,0,0,0.6); }
  popover button { min-width: 180px; }

  /* Liquid glass is drawn by phone_glass.py; CSS only lays it out. */
  .glass { background: transparent; }
  button.glass-control, button.glass-control:hover, button.glass-control:active,
  button.glass-control:disabled { background: transparent; box-shadow: none; }
  .glass-progress trough { background: transparent; }
  .tab-bar { padding: 8px; }
  .nav { background: transparent; border-radius: 999px; padding: 6px 12px; min-width: 64px;
         min-height: 60px; color: #ffffff; }
  /* A wide, shallow lens leaves room around the symbol and its caption. */
  .tab-bar.vertical .nav { min-width: 88px; min-height: 60px; padding: 6px 4px; }
  .nav label { font-size: 12px; font-weight: 500; letter-spacing: 0; }
  .nav:checked { background: transparent; color: #0a84ff; }
  .nav:hover { background: transparent; }
  .nav:disabled { background: transparent; color: rgba(235,235,245,0.3); }

  /* Lists of choices with a trailing checkmark */
  .cluster-choice { background: #1c1c1e; border-radius: 26px; padding: 16px 20px;
                    font-weight: 400; color: #ffffff; }
  .cluster-choice:hover { background: #242426; }
  .cluster-choice:active { background: #2c2c2e; }
  .choice-icon { padding: 7px; border-radius: 10px; }
  .tint-blue { background: #0a84ff; }
  .tint-pink { background: #ff375f; }
  .tint-indigo { background: #5e5ce6; }
  .tint-orange { background: #ff9f0a; }
  .tint-green { background: #30d158; }
  .tint-gray { background: #636366; }
  .selection-mark { color: #0a84ff; font-size: 20px; font-weight: 600; }
  .list-group flowboxchild { padding: 0; }
  .list-row { background: #1c1c1e; border-radius: 0; min-height: 52px; padding: 0 20px;
              font-size: 17px; font-weight: 400; color: #ffffff; border-bottom: 1px solid #38383a; }
  .list-row:hover { background: #242426; }
  .list-row:active { background: #2c2c2e; }
  .list-row.selected { color: #0a84ff; font-weight: 600; }
  .list-group flowboxchild:first-child .list-row { border-top-left-radius: 26px; border-top-right-radius: 26px; }
  .list-group flowboxchild:last-child .list-row { border-bottom-left-radius: 26px; border-bottom-right-radius: 26px;
                                                  border-bottom: none; }
  .choice-label { font-size: 17px; font-weight: 400; }
  .list-row.selected .choice-label { font-weight: 600; }

  /* Cluster formats: miniature previews ringed in blue when chosen */
  .format-face { background: transparent; padding: 0; min-height: 0; border-radius: 22px; }
  .format-face:hover { background: transparent; }
  .format-face:active { opacity: 0.7; }
  .format-name { font-size: 15px; font-weight: 500; letter-spacing: -0.2px; color: rgba(235,235,245,0.6); }
  .format-face.selected .format-name { color: #0a84ff; font-weight: 600; }
  .row-icon { padding: 6px; border-radius: 8px; }

  /* Car readings */
  .car-tile { padding: 18px 20px; }
  .car-value { font-family: sans-serif; font-size: 40px;
               font-weight: 600; letter-spacing: -0.5px; font-feature-settings: "tnum"; }
  .car-value.unavailable, .car-row-value.unavailable { color: rgba(235,235,245,0.3); }
  .car-row-value { font-size: 17px; color: rgba(235,235,245,0.6); font-feature-settings: "tnum"; }
  .car-tile progressbar trough { min-height: 4px; }
  .car-tile progressbar progress { min-height: 4px; background: #30d158; }
  .simulated-badge { color: #ff9f0a; background: rgba(255,159,10,0.16); border-radius: 999px;
                     padding: 4px 10px; font-size: 12px; font-weight: 700; letter-spacing: 0.6px; }

  /* Segmented control */
  .segmented { background: rgba(118,118,128,0.24); border-radius: 999px; padding: 3px; }
  .segment { background: transparent; min-height: 38px; border-radius: 999px; font-size: 15px;
             font-weight: 500; letter-spacing: -0.2px; }
  .segment:hover { background: rgba(255,255,255,0.06); }
  .segment.selected { background: #636366; font-weight: 600;
                      box-shadow: 0 3px 8px rgba(0,0,0,0.25); }

  /* Home: compact time/date above the player and CarPlay launchers. */
  .home-clock { font-family: sans-serif; font-size: 52px;
                font-weight: 600; letter-spacing: -1px; font-feature-settings: "tnum"; }
  .home-date { font-size: 18px; font-weight: 400; color: rgba(235,235,245,0.6); }
  .home-connection .widget-title { font-size: 17px; }
  .home-connection .widget-subtitle { font-size: 15px; }
  .home-shortcut { min-height: 64px; padding: 16px 24px; border-radius: 24px; }
  .home-shortcut-title { font-size: 21px; font-weight: 600; }
  .home-shortcut-detail { font-size: 15px; font-weight: 400; color: rgba(235,235,245,0.6); }
  .home-shortcut-chevron { font-size: 28px; font-weight: 400; color: rgba(235,235,245,0.4); }
  .home-shortcut:disabled image { opacity: 0.5; }
  .home-shortcut:disabled .home-shortcut-title { color: rgba(235,235,245,0.45); }
  .widget { padding: 20px; font-weight: 400; }
  .widget-surface { background: transparent; border-radius: 30px; min-height: 0; padding: 20px; font-weight: 400; }
  .widget-surface:hover { background: rgba(255,255,255,0.04); }
  .widget-surface:active { background: rgba(255,255,255,0.08); }
  .now-playing { padding: 24px; }
  .widget-track, .widget-track:hover, .widget-track:active {
    background: transparent; min-height: 0; padding: 0; border-radius: 10px; }
  .widget-track:active { opacity: 0.7; }
  .widget-eyebrow { color: rgba(235,235,245,0.6); font-size: 13px; font-weight: 600; letter-spacing: -0.1px; }
  .widget-title { font-size: 20px; font-weight: 600; letter-spacing: -0.4px; color: #ffffff; }
  .widget-subtitle { font-size: 17px; color: rgba(235,235,245,0.6); letter-spacing: -0.4px; }
  .widget-track-title { font-size: 28px; font-weight: 600; }
  .widget-track-artist { font-size: 21px; font-weight: 400; }
  .widget-artwork { border-radius: 10px; background: #2c2c2e; }
  .widget-badge { background: #30d158; border-radius: 12px; padding: 9px; }
  .widget-badge.offline { background: #3a3a3c; }
  .widget-transport { background: transparent; min-width: 76px; min-height: 76px; padding: 0; }
  .widget-transport:hover { background: rgba(255,255,255,0.08); }
  .widget-transport:active { background: rgba(255,255,255,0.14); }
  .widget-action { min-height: 44px; }
  .status-dot { min-width: 8px; min-height: 8px; border-radius: 999px; background: #636366; }
  .status-dot.live { background: #30d158; }
  .app-launcher { background: transparent; padding: 0; min-height: 0; border-radius: 18px; }
  .app-launcher:hover { background: transparent; }
  .app-launcher:active image { opacity: 0.6; }
  .app-launcher:disabled { background: transparent; }
  .app-label { font-size: 13px; font-weight: 500; letter-spacing: -0.1px; color: #ffffff; }

  /* Music: dark artwork wash, plain transport glyphs */
  .artwork { background: #2c2c2e; border-radius: 14px; box-shadow: 0 24px 60px rgba(0,0,0,0.55); }
  .track { font-family: sans-serif;
           font-size: 28px; font-weight: 700; letter-spacing: 0.35px; }
  .transport button { background: transparent; padding: 0; min-width: 64px; min-height: 64px; }
  .transport button:hover { background: rgba(255,255,255,0.08); }
  .transport button:active { background: rgba(255,255,255,0.14); }
  .music-card { background: transparent; padding: 20px 0; }
  .media-time { font-size: 13px; font-weight: 500; font-feature-settings: "tnum"; color: rgba(235,235,245,0.6); }
  progressbar trough { border: none; min-height: 6px; border-radius: 6px; background: rgba(255,255,255,0.22); }
  progressbar progress { border: none; margin: 0; min-width: 6px; min-height: 6px;
                         border-radius: 6px; background: rgba(255,255,255,0.85); }

  /* Overlay Home button drawn above CarPlay */
  .home-window { background: transparent; }
  .home-button { min-height: 0; min-width: 0; padding: 0; border-radius: 50%;
                 background: rgba(255,255,255,0.7); border: 1px solid rgba(255,255,255,0.95);
                 box-shadow: 0 6px 18px rgba(0,0,0,0.18), inset 0 2px rgba(255,255,255,1); }
  .home-button:hover { background: rgba(255,255,255,0.9); }
'''
