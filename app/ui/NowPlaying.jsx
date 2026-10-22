import {memo} from 'react';
import {Image, Path, Svg, Text, View} from '@ps5-react/core';
import {toggleMusic, useMusicStatus} from '../music/index.js';
import {useT} from '../i18n/index.js';
import {fitBytes} from '../i18n/fit.js';

export const NowPlaying = memo(function NowPlaying({nextFocusDown}) {
  const status = useMusicStatus(), t = useT();
  const {track, paused} = status;
  const enabled = Boolean(track) && status.state !== 'failed';
  return <View focusKey="nav:music" nextFocusDown={nextFocusDown}
    onPress={enabled ? toggleMusic : undefined}
    style={({focused}) => ({width: 250, height: 66, flexDirection: 'row', alignItems: 'center', gap: 12,
      paddingHorizontal: 8, borderRadius: 10, borderWidth: 1,
      borderColor: focused ? '#cae9ff' : '#ffffff1a', backgroundColor: '#05090ddb'})}>
    {track?.cover ? <Image source={{uri: track.cover}} resizeMode="cover"
      style={{width: 44, height: 44, borderRadius: 7}} /> :
      <View style={{width: 44, height: 44, borderRadius: 7, alignItems: 'center', justifyContent: 'center',
        backgroundColor: '#18202b'}}><Svg width={24} height={24} viewBox="0 0 24 24">
        <Path fill="#cde9fa" d="M10 4 V16 A4 3 0 1 1 8 13.4 V5 Z M10 4 L20 2 V14 A4 3 0 1 1 18 11.4 V6 L10 8 Z"/>
      </Svg></View>}
    <View style={{flex: 1, gap: 3}}>
      <Text style={{fontFamily: 'Inter-Regular', color: '#f3f7fa', fontSize: 21,
        lineHeight: 25, fontWeight: 600, numberOfLines:1,ellipsizeMode:'tail'}}>{fitBytes(track?.title ?? t('music.empty'))}</Text>
      <Text style={{fontFamily: 'Inter-Regular', color: '#a6b5c3', fontSize: 18,
        lineHeight: 21, numberOfLines:1,ellipsizeMode:'tail'}}>{status.state === 'failed' ? t('music.unavailable') :
          paused ? t('music.paused') : fitBytes(track?.artist ?? t('music.pack'))}</Text>
    </View>
    {enabled ? <View style={{width: 24, alignItems: 'center', gap: 5}}>
      <Svg width={18} height={18} viewBox="0 0 18 18">
        <Path fill="#d9efff" d={paused ? 'M5 3 L15 9 L5 15 Z' : 'M4 3 H7 V15 H4 Z M11 3 H14 V15 H11 Z'} />
      </Svg>
      <Text style={{fontFamily: 'Inter-Regular', fontSize: 18, color: '#a6b5c3'}}>R3</Text>
    </View> : null}
  </View>;
});
