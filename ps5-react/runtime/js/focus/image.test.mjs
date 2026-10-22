// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import assert from 'node:assert/strict';
import test from 'node:test';
import {remoteUri} from './image.js';

test('catalog file sources reach the native image loader instead of becoming baked names', () => {
  const uri = 'file:///data/homebrew/catalog-user/covers/обложка 1.jpg';
  assert.equal(remoteUri({uri}), uri);
  assert.equal(remoteUri(uri), uri);
  assert.equal(remoteUri({uri: 'file://relative/cover.jpg'}), null);
  assert.equal(remoteUri('app-cover'), null);
  assert.equal(remoteUri({uri: 'https://example.test/cover.jpg'}), 'https://example.test/cover.jpg');
});
