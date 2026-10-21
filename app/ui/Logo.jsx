// ../pub-game-store/native/ui/Logo.jsx
// reconstructed from the esbuild bundle in eboot.bin

import '../assets/Inter-Regular.ttf';
import '../assets/PirataOne-Regular.ttf';
import { jsx, jsxs } from 'react/jsx-runtime';
import { Text, View } from '@ps5-react/core';
import { colors } from './colors.js';
import { px } from './scale.js';

  function Logo({ color = colors.fg }) {
    return /* @__PURE__ */ (0, jsx)(View, { style: { flexDirection: "row", alignItems: "center" }, children:
      /* @__PURE__ */ (0, jsx)(Text, { style: [{ fontFamily: "PirataOne-Regular", fontSize: px(30), letterSpacing: 1 }, { color }], children: "TRAPSTARSHOP" })
    });
  }

export { Logo };
