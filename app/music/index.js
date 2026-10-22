import {useEffect, useState} from 'react';
import {FileSystem, Music, Platform} from '@ps5-react/core';
import {createMusicController} from './controller.js';

let instance;
function controller() {
  return instance ??= createMusicController({music: Music, fs: FileSystem,
    roots: Platform.OS === 'ps4' ? [`${FileSystem.appDir}/media`] :
      ['/data/homebrew/trapstar-media', `${FileSystem.appDir}/media`],
    log: text => console.log(`[overdrive] ${text}`)});
}
export function useBackgroundMusic() {
  useEffect(() => { const player = controller(); player.start(); return () => player.stop(); }, []);
}
export function useMusicStatus() {
  const [state, setState] = useState(() => controller().getSnapshot());
  useEffect(() => controller().subscribe(setState), []);
  return state;
}
export const toggleMusic = () => controller().togglePaused();
