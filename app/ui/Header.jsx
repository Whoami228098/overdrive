// ../pub-game-store/native/ui/Header.jsx
// reconstructed from the esbuild bundle in eboot.bin
// MODIFIED: the settings button shows a gear.

import '../assets/Inter-Regular.ttf';
import { jsx, jsxs } from 'react/jsx-runtime';
import { useEffect, useRef, useState } from 'react';
import { focusEnd, useLocale, useT } from '../i18n/index.js';
import { NO_FILTERS, activeCount, genreLabel, genres, getGame } from '../state/catalog.js';
import { useIsImmersive, useNavActions, useNavState } from '../state/nav.jsx';
import { useWishlist } from '../state/store.jsx';
import { ButtonGlyph } from './ButtonHints.jsx';
import { DownloadToast } from './DownloadToast.jsx';
import { DownloadsBadge } from './DownloadsBadge.jsx';
import { FocusRing } from './FocusRing.jsx';
import { LanguageSheet } from './LanguageSheet.jsx';
import { NowPlaying } from './NowPlaying.jsx';
import { toggleMusic } from '../music/index.js';
import { Menu, MenuDivider, MenuItem } from './Menu.jsx';
import { NavItem } from './NavItem.jsx';
import { NavPill, useNavLayouts } from './NavPill.jsx';
import { useSections } from './useSections.js';
import { VERSION } from '../version.js';
import { FocusScope, Text, View, useControllerEvents } from '@ps5-react/core';

  var BADGE = /* @__PURE__ */ (0, jsx)(DownloadsBadge, {});
  function Header({ children }) {
    const t2 = useT();
    const { rtl } = useLocale();
    const { section, shown, filters, on, step, keyOf, browseFocus, leaveBrowse } = useSections();
    const wishlist = useWishlist();
    const favorites = wishlist.filter((id) => getGame(id)).length;
    const nav = useNavActions();
    const { top } = useNavState();
    const [browsing, setBrowsing] = (0, useState)(false);
    const browsingNow = (0, useRef)(false);
    browsingNow.current = browsing;
    // A closed menu returns focus a frame later: only then can it tell whether focus left Browse.
    (0, useEffect)(() => {
      if (browsing) return void 0;
      const id = setTimeout(leaveBrowse, 50);
      return () => clearTimeout(id);
    }, [browsing]);
    const [choosingLanguage, setChoosingLanguage] = (0, useState)(false);
    const immersive = useIsImmersive();
    const [layouts, track] = useNavLayouts();
    const onlyGenre = filters.genre.length === 1 && activeCount(filters) === 1 && filters.genre[0];
    const down = top.name === "browse" ? "page" : top.name === "homebrew" ? "homebrew-page" : "screen";
    const toastAction = (0, useRef)(null);
    // Stick clicks arrive through the controller state, outside the focus actions.
    useControllerEvents(event => {
      if (event.type === 'press' && event.button === 'r3') toggleMusic();
    });
    const onAction = (action) => {
      if (action === "r2" && toastAction.current) {
        toastAction.current();
        return true;
      }
      if (action !== "l1" && action !== "r1") return false;
      setBrowsing(false);
      // Right-to-left the sections run leftward, so L1 is the one that moves on (issue #30).
      const forward = action === (rtl ? "l1" : "r1");
      if (forward && section === "files") setChoosingLanguage(true);
      else
        step(forward);
      return true;
    };
    const onBack = () => {
      if (!nav.pop()) nav.focusLater("spotlight:cta");
      return true;
    };
    return /* @__PURE__ */ (0, jsxs)(FocusScope, { onAction, onBack, children: [
      children,
      /* @__PURE__ */ (0, jsx)(DownloadToast, { actionRef: toastAction }),
      choosingLanguage ? /* @__PURE__ */ (0, jsx)(LanguageSheet, { onClose: () => setChoosingLanguage(false) }) : null,
      /* @__PURE__ */ (0, jsx)(View, { style: immersive ? { position: "absolute", left: 0, right: 0, top: 0, zIndex: 40, display: "none" } : { position: "absolute", left: 0, right: 0, top: 0, zIndex: 40 }, children: /* @__PURE__ */ (0, jsx)(FocusScope, { focusKey: "header", inert: immersive, children: /* @__PURE__ */ (0, jsxs)(View, { onLayout: track("header"), style: { height: 96, flexDirection: "row", alignItems: "center", justifyContent: "space-between", backgroundColor: "#000000", paddingHorizontal: 48 }, children: [
        /* @__PURE__ */ (0, jsx)(NavPill, { section: shown, layouts }),
        /* @__PURE__ */ (0, jsxs)(View, { style: { flex: 1, flexDirection: "row", alignItems: "center", justifyContent: "space-between", gap: 24 }, children: [
          /* @__PURE__ */ (0, jsx)(View, { style: rtl ? { marginRight: -24 } : { marginLeft: -24 }, children: /* @__PURE__ */ (0, jsx)(
            NavItem,
            {
              focusKey: keyOf("search"),
              icon: "search",
              label: t2("nav.search"),
              current: shown === "search",
              onPress: on.search,
              onLayout: track("search"),
              nextFocusDown: down
            }
          ) }),
          /* @__PURE__ */ (0, jsxs)(View, { style: { flexDirection: "row", alignItems: "center", gap: 6 }, children: [
            /* @__PURE__ */ (0, jsx)(ButtonGlyph, { button: rtl ? "r1" : "l1" }),
            /* @__PURE__ */ (0, jsx)(
              NavItem,
              {
                focusKey: keyOf("discover"),
                label: t2("nav.discover"),
                current: shown === "discover",
                onPress: on.discover,
                onLayout: track("discover"),
                nextFocusDown: down
              }
            ),
            /* @__PURE__ */ (0, jsxs)(View, { style: { position: "relative" }, children: [
              /* @__PURE__ */ (0, jsx)(
                NavItem,
                {
                  focusKey: keyOf("catalog"),
                  label: t2("nav.browse"),
                  trailing: browsing ? "chevronUp" : "chevronDown",
                  current: shown === "catalog",
                  onPress: () => setBrowsing((open) => !open),
                  onLayout: track("catalog"),
                  nextFocusDown: down,
                  onFocus: browseFocus.onFocus,
                  onBlur: () => {
                    browseFocus.onBlur();
                    if (!browsingNow.current) leaveBrowse();
                  }
                }
              ),
              /* @__PURE__ */ (0, jsxs)(Menu, { open: browsing, onClose: () => setBrowsing(false), rows: genres.length + 3, children: [
                (0, jsx)(MenuItem, {label:t2("mods.title"),onSelect:()=>nav.push({name:"mods"},keyOf("catalog"))}),
                /* @__PURE__ */ (0, jsx)(
                  MenuItem,
                  {
                    label: t2("nav.allGames"),
                    checked: section === "catalog" && !filters.genre.length,
                    autoFocus: !onlyGenre,
                    onSelect: () => on.show(NO_FILTERS, "newest", void 0, "catalog")
                  }
                ),
                /* @__PURE__ */ (0, jsx)(MenuDivider, {}),
                genres.map(
                  (genre) => /* @__PURE__ */ (0, jsx)(
                    MenuItem,
                    {
                      label: genreLabel(genre),
                      checked: onlyGenre === genre,
                      onSelect: () => on.show({ ...NO_FILTERS, genre: [genre] }, "newest")
                    },
                    genre
                  )
                )
              ] })
            ] })
          ] })
        ] }),
        /* @__PURE__ */ (0, jsxs)(View, { style: { marginHorizontal: 36, flexDirection: "row", alignItems: "center", gap: 12, borderRadius: 6, paddingHorizontal: 12 }, children: [
          /* @__PURE__ */ (0, jsx)(NowPlaying, { nextFocusDown: down }),
          /* @__PURE__ */ (0, jsx)(Text, { style: { fontFamily: "Inter-Regular", fontSize: 21, color: "#8a8a94" }, children: `v${VERSION}` })
        ] }),
        /* @__PURE__ */ (0, jsxs)(View, { style: { flex: 1, flexDirection: "row", alignItems: "center", justifyContent: "space-between", gap: 24 }, children: [
          /* @__PURE__ */ (0, jsxs)(View, { style: { flexDirection: "row", alignItems: "center", gap: 6 }, children: [
            /* @__PURE__ */ (0, jsx)(
              NavItem,
              {
                focusKey: keyOf("top"),
                label: t2("nav.top"),
                current: shown === "top",
                onPress: on.top,
                onLayout: track("top"),
                nextFocusDown: down
              }
            ),
            /* @__PURE__ */ (0, jsx)(
              NavItem,
              {
                focusKey: keyOf("homebrew"),
                label: t2("nav.homebrew"),
                current: shown === "homebrew",
                onPress: on.homebrew,
                onLayout: track("homebrew"),
                nextFocusDown: down
              }
            )
          ] }),
          /* @__PURE__ */ (0, jsxs)(View, { style: { flexDirection: "row", alignItems: "center", gap: 6 }, children: [
            /* @__PURE__ */ (0, jsx)(
              NavItem,
              {
                focusKey: keyOf("favorites"),
                icon: "heart",
                label: favorites ? String(favorites) : null,
                current: shown === "favorites",
                onPress: on.favorites,
                onLayout: track("favorites"),
                nextFocusDown: down,
                ...focusEnd(rtl, keyOf("downloads"))
              }
            ),
            /* @__PURE__ */ (0, jsx)(
              NavItem,
              {
                focusKey: keyOf("downloads"),
                icon: "download",
                current: shown === "downloads",
                onPress: on.downloads,
                onLayout: track("downloads"),
                nextFocusDown: down,
                children: BADGE
              }
            ),
            /* @__PURE__ */ (0, jsx)(
              NavItem,
              {
                focusKey: keyOf("files"),
                icon: "folder",
                current: shown === "files",
                onPress: on.files,
                onLayout: track("files"),
                nextFocusDown: down
              }
            ),
            /* @__PURE__ */ (0, jsx)(
              NavItem,
              {
                focusKey: "nav:language",
                icon: "settings",
                onPress: () => setChoosingLanguage(true),
                nextFocusDown: down,
                ...focusEnd(rtl, "nav:language")
              }
            ),
            /* @__PURE__ */ (0, jsx)(ButtonGlyph, { button: rtl ? "l1" : "r1" })
          ] })
        ] })
      ] }) }) })
    ] });
  }

export { Header };
